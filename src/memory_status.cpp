// FNVVR-Test-Bridge. Copyright (c) 2026 joschi127. zlib licence, see LICENSE.

#include "memory_status.h"

#include <cstdio>

#include <windows.h>
#include <psapi.h>

namespace bridge
{
    std::string MemoryStatus()
    {
        SYSTEM_INFO info;
        GetSystemInfo(&info);
        auto low = reinterpret_cast<uintptr_t>(info.lpMinimumApplicationAddress);
        auto high = reinterpret_cast<uintptr_t>(info.lpMaximumApplicationAddress);

        unsigned long long freeBytes = 0, largest = 0;
        for (uintptr_t at = low; at < high;)
        {
            MEMORY_BASIC_INFORMATION region;
            if (!VirtualQuery(reinterpret_cast<void*>(at), &region, sizeof(region)) || !region.RegionSize)
                break;
            if (region.State == MEM_FREE)
            {
                freeBytes += region.RegionSize;
                if (region.RegionSize > largest)
                    largest = region.RegionSize;
            }
            uintptr_t next = reinterpret_cast<uintptr_t>(region.BaseAddress) + region.RegionSize;
            if (next <= at)
                break;
            at = next;
        }

        // K32GetProcessMemoryInfo lives in kernel32 since Windows 7; looked up to avoid linking psapi
        using GetInfo = BOOL(WINAPI*)(HANDLE, PPROCESS_MEMORY_COUNTERS, DWORD);
        auto getInfo = reinterpret_cast<GetInfo>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "K32GetProcessMemoryInfo"));
        PROCESS_MEMORY_COUNTERS_EX counters{};
        counters.cb = sizeof(counters);
        bool haveCounters = getInfo &&
            getInfo(GetCurrentProcess(), reinterpret_cast<PPROCESS_MEMORY_COUNTERS>(&counters), sizeof(counters));

        constexpr double kMB = 1024.0 * 1024.0;
        unsigned long long total = high - low + 1;
        char out[192];
        std::snprintf(out, sizeof(out), "vfree=%.0f vlargest=%.0f vused=%.0f vtotal=%.0f private=%.0f workingset=%.0f",
            freeBytes / kMB, largest / kMB, (total - freeBytes) / kMB, total / kMB,
            haveCounters ? counters.PrivateUsage / kMB : -1.0, haveCounters ? counters.WorkingSetSize / kMB : -1.0);
        return out;
    }
}
