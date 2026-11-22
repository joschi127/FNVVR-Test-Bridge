// FNVVR-Test-Bridge. Copyright (c) 2026 joschi127. zlib licence, see LICENSE.

#include "log.h"

#include <cstdarg>
#include <cstdio>
#include <mutex>

#include <windows.h>

namespace bridge
{
    namespace
    {
        std::mutex g_logLock;
        FILE* g_log = nullptr;
    }

    void OpenLog(const std::string& path)
    {
        std::lock_guard guard(g_logLock);
        if (!g_log)
            g_log = _fsopen(path.c_str(), "w", _SH_DENYWR);
    }

    void Log(const char* fmt, ...)
    {
        std::lock_guard guard(g_logLock);
        if (!g_log)
            return;
        SYSTEMTIME t;
        GetLocalTime(&t);
        std::fprintf(g_log, "%02u:%02u:%02u.%03u ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
        va_list args;
        va_start(args, fmt);
        std::vfprintf(g_log, fmt, args);
        va_end(args);
        std::fputc('\n', g_log);
        std::fflush(g_log);
    }
}
