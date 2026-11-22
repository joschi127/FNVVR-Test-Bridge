// FNVVR-Test-Bridge. Copyright (c) 2026 joschi127. zlib licence, see LICENSE.

#pragma once

#include <atomic>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace bridge
{
    // One request from a tool, handed from the pipe thread to the game's main thread.
    struct Job
    {
        std::string line;
        std::promise<std::vector<std::string>> reply;  // all answer lines, the last one "ok ..." / "err ..."
        std::atomic<bool> abandoned{false};            // the pipe gave up waiting; the main thread drops the job
    };

    // A named pipe for one tool at a time, for the current Windows user only, no remote clients.
    // Blocking I/O on its own thread (64 KB stack reservation); Stop() cancels it with CancelSynchronousIo.
    class PipeServer
    {
    public:
        PipeServer() = default;
        PipeServer(const PipeServer&) = delete;
        PipeServer& operator=(const PipeServer&) = delete;
        ~PipeServer();

        // Listens on "\\.\pipe\<name>", or "<name>-<process id>" if another game already has that name.
        // Returns the full pipe name, or an empty string on failure.
        std::wstring Start(const std::wstring& name);
        void Stop();

        // Main thread, every frame. Without a waiting request this is one atomic load, no lock, so an idle
        // bridge costs next to nothing per frame.
        std::shared_ptr<Job> Poll();

    private:
        static unsigned long __stdcall ThreadMain(void* self);
        void Serve();
        void ServeClient();
        std::vector<std::string> Hand(const std::string& line);

        void* m_pipe = nullptr;
        void* m_thread = nullptr;
        std::atomic<bool> m_quit{false};
        std::atomic<bool> m_waiting{false};
        std::mutex m_lock;
        std::shared_ptr<Job> m_job;
    };
}
