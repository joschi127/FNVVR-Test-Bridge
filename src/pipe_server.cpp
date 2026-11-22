// FNVVR-Test-Bridge. Copyright (c) 2026 joschi127. zlib licence, see LICENSE.

#include "pipe_server.h"

#include "log.h"
#include "memory_status.h"
#include "protocol.h"

#include <chrono>

#include <windows.h>

namespace bridge
{
    namespace
    {
        constexpr auto kReplyTimeout = std::chrono::seconds(60);
        constexpr SIZE_T kThreadStack = 64 * 1024;

        // Access rules that let only the game's own user open the pipe. The descriptor and its list point into
        // this object, so it has to live until the pipe is created.
        struct UserOnlyAccess
        {
            std::vector<BYTE> user;
            std::vector<BYTE> acl;
            SECURITY_DESCRIPTOR descriptor{};

            bool Build()
            {
                HANDLE token = nullptr;
                if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
                    return false;
                DWORD size = 0;
                GetTokenInformation(token, TokenUser, nullptr, 0, &size);
                user.resize(size);
                bool ok = size && GetTokenInformation(token, TokenUser, user.data(), size, &size);
                CloseHandle(token);
                if (!ok)
                    return false;

                PSID sid = reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid;
                acl.resize(sizeof(ACL) + sizeof(ACCESS_ALLOWED_ACE) + GetLengthSid(sid));
                auto* list = reinterpret_cast<ACL*>(acl.data());
                return InitializeAcl(list, static_cast<DWORD>(acl.size()), ACL_REVISION) &&
                    AddAccessAllowedAce(list, ACL_REVISION, GENERIC_ALL, sid) &&
                    InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION) &&
                    SetSecurityDescriptorDacl(&descriptor, TRUE, list, FALSE);
            }
        };

        HANDLE OpenPipe(const std::wstring& path, SECURITY_DESCRIPTOR* descriptor)
        {
            SECURITY_ATTRIBUTES attributes{};
            attributes.nLength = sizeof(attributes);
            attributes.lpSecurityDescriptor = descriptor;
            return CreateNamedPipeW(path.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
                PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 64 * 1024, 8 * 1024, 0, &attributes);
        }
    }

    PipeServer::~PipeServer()
    {
        Stop();
    }

    std::wstring PipeServer::Start(const std::wstring& name)
    {
        UserOnlyAccess access;
        if (!access.Build())
        {
            Log("pipe: no user-only access rules (error %lu); not listening", GetLastError());
            return {};
        }
        std::wstring path = L"\\\\.\\pipe\\" + name;
        HANDLE pipe = OpenPipe(path, &access.descriptor);
        if (pipe == INVALID_HANDLE_VALUE && GetLastError() == ERROR_ACCESS_DENIED)
        {
            path += L"-" + std::to_wstring(GetCurrentProcessId());
            pipe = OpenPipe(path, &access.descriptor);
        }
        if (pipe == INVALID_HANDLE_VALUE)
        {
            Log("pipe: CreateNamedPipe failed (error %lu); not listening", GetLastError());
            return {};
        }
        m_pipe = pipe;
        m_thread = CreateThread(nullptr, kThreadStack, &PipeServer::ThreadMain, this,
            STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
        if (!m_thread)
        {
            Log("pipe: CreateThread failed (error %lu); not listening", GetLastError());
            CloseHandle(m_pipe);
            m_pipe = nullptr;
            return {};
        }
        return path;
    }

    unsigned long __stdcall PipeServer::ThreadMain(void* self)
    {
        static_cast<PipeServer*>(self)->Serve();
        return 0;
    }

    void PipeServer::Stop()
    {
        if (!m_thread)
            return;
        m_quit = true;
        // The worker blocks in ConnectNamedPipe, ReadFile or WriteFile: cancel that call until it notices m_quit.
        while (WaitForSingleObject(m_thread, 50) == WAIT_TIMEOUT)
            CancelSynchronousIo(m_thread);
        CloseHandle(m_thread);
        CloseHandle(m_pipe);
        m_thread = m_pipe = nullptr;
    }

    std::shared_ptr<Job> PipeServer::Poll()
    {
        if (!m_waiting.load(std::memory_order_acquire))
            return nullptr;
        std::lock_guard guard(m_lock);
        m_waiting.store(false, std::memory_order_relaxed);
        return std::exchange(m_job, nullptr);
    }

    std::vector<std::string> PipeServer::Hand(const std::string& line)
    {
        auto job = std::make_shared<Job>();
        job->line = line;
        std::future<std::vector<std::string>> answer = job->reply.get_future();
        {
            std::lock_guard guard(m_lock);
            m_job = job;
            m_waiting.store(true, std::memory_order_release);
        }
        auto deadline = std::chrono::steady_clock::now() + kReplyTimeout;
        while (std::chrono::steady_clock::now() < deadline && !m_quit)
        {
            if (answer.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready)
                return answer.get();
        }
        bool picked;
        {
            std::lock_guard guard(m_lock);
            picked = m_job != job;
            if (!picked)
            {
                m_job = nullptr;
                m_waiting.store(false, std::memory_order_relaxed);
            }
        }
        job->abandoned = true;  // a picked-up job may still wait for frames on the main thread
        if (m_quit)
            return {Err("the game is closing")};
        return {Err(picked ? "no answer in time" : "the game did not pick up the request in time (loading?)")};
    }

    void PipeServer::ServeClient()
    {
        LineBuffer lines(kMaxLine);
        char chunk[2048];
        while (!m_quit)
        {
            DWORD got = 0;
            if (!ReadFile(m_pipe, chunk, sizeof(chunk), &got, nullptr) || got == 0)
                return;  // tool gone, or cancelled
            lines.Feed(std::string_view(chunk, got));
            std::string line;
            bool overflow = false;
            while (lines.Next(line, overflow))
            {
                std::vector<std::string> answer;
                if (overflow)
                    answer = {Err("line too long")};
                else if (Parse(line).verb == Verb::Memory)
                    answer = {Ok(MemoryStatus())};  // answered here, so it also works during loading screens
                else
                    answer = Hand(line);
                std::string out;
                for (const std::string& l : answer)
                    out.append(l).push_back('\n');
                DWORD wrote = 0;
                if (!WriteFile(m_pipe, out.data(), static_cast<DWORD>(out.size()), &wrote, nullptr) ||
                    wrote != out.size())
                    return;
            }
        }
    }

    void PipeServer::Serve()
    {
        while (!m_quit)
        {
            BOOL connected = ConnectNamedPipe(m_pipe, nullptr);
            DWORD error = connected ? 0 : GetLastError();
            if (m_quit)
                break;
            if (connected || error == ERROR_PIPE_CONNECTED)
                ServeClient();
            else if (error != ERROR_OPERATION_ABORTED && error != ERROR_NO_DATA)
            {
                Log("pipe: ConnectNamedPipe failed (error %lu)", error);
                Sleep(500);
            }
            DisconnectNamedPipe(m_pipe);
        }
    }
}
