// FNVVR-Test-Bridge. Copyright (c) 2026 joschi127. zlib licence, see LICENSE.

#include "pipe_server.h"

#include "log.h"
#include "protocol.h"

#include <windows.h>
#include <sddl.h>

namespace bridge
{
    namespace
    {
        constexpr auto kReplyTimeout = std::chrono::seconds(30);

        // DACL with a single entry: full access for the user running the game. Nobody else may open the pipe.
        PSECURITY_DESCRIPTOR OwnerOnlyDescriptor()
        {
            PSECURITY_DESCRIPTOR result = nullptr;
            HANDLE token = nullptr;
            if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
                return nullptr;
            DWORD needed = 0;
            GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
            std::vector<BYTE> info(needed);
            LPWSTR sidText = nullptr;
            if (needed && GetTokenInformation(token, TokenUser, info.data(), needed, &needed) &&
                ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(info.data())->User.Sid, &sidText))
            {
                std::wstring sddl = std::wstring(L"D:P(A;;GA;;;") + sidText + L")";
                if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &result,
                        nullptr))
                    result = nullptr;
                LocalFree(sidText);
            }
            CloseHandle(token);
            return result;
        }

        HANDLE OpenPipe(const std::wstring& path, PSECURITY_DESCRIPTOR descriptor)
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
        PSECURITY_DESCRIPTOR descriptor = OwnerOnlyDescriptor();
        if (!descriptor)
        {
            Log("pipe: no owner-only security descriptor (error %lu); not listening", GetLastError());
            return {};
        }
        std::wstring path = L"\\\\.\\pipe\\" + name;
        HANDLE pipe = OpenPipe(path, descriptor);
        if (pipe == INVALID_HANDLE_VALUE && GetLastError() == ERROR_ACCESS_DENIED)
        {
            path += L"-" + std::to_wstring(GetCurrentProcessId());
            pipe = OpenPipe(path, descriptor);
        }
        LocalFree(descriptor);
        if (pipe == INVALID_HANDLE_VALUE)
        {
            Log("pipe: CreateNamedPipe failed (error %lu); not listening", GetLastError());
            return {};
        }
        m_pipe = pipe;
        m_worker = std::thread(&PipeServer::Serve, this);
        return path;
    }

    void PipeServer::Stop()
    {
        if (!m_worker.joinable())
            return;
        m_quit = true;
        // The worker blocks in ConnectNamedPipe, ReadFile or WriteFile: cancel that call until it notices m_quit.
        while (WaitForSingleObject(m_worker.native_handle(), 50) == WAIT_TIMEOUT)
            CancelSynchronousIo(m_worker.native_handle());
        m_worker.join();
        CloseHandle(m_pipe);
        m_pipe = nullptr;
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
        for (auto waited = std::chrono::milliseconds(0); waited < kReplyTimeout; waited += std::chrono::milliseconds(100))
        {
            if (answer.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready)
                return answer.get();
            if (m_quit)
                break;
        }
        // Withdraw it if the main thread has not picked it up yet; if it has, its late reply goes nowhere.
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
                std::vector<std::string> answer = overflow ? std::vector<std::string>{Err("line too long")} : Hand(line);
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
            BOOL connected = ConnectNamedPipe(m_pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED;
            if (m_quit)
                break;
            if (connected)
                ServeClient();
            else if (GetLastError() != ERROR_OPERATION_ABORTED)
            {
                Log("pipe: ConnectNamedPipe failed (error %lu)", GetLastError());
                Sleep(500);
            }
            DisconnectNamedPipe(m_pipe);
        }
    }
}
