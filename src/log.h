// FNVVR-Test-Bridge. Copyright (c) 2026 joschi127. zlib licence, see LICENSE.

#pragma once

#include <string>

namespace bridge
{
    // Plain text log next to FalloutNV.exe (FNVVR-Test-Bridge.log), rewritten at each game start.
    void OpenLog(const std::string& path);
    void Log(const char* fmt, ...);
}
