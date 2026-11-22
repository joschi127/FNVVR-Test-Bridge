// FNVVR-Test-Bridge. Copyright (c) 2026 joschi127. zlib licence, see LICENSE.

#pragma once

#include <string>

namespace bridge
{
    // The game process's memory, for the "memory" request: address space (the scarce resource of a 32-bit
    // game) and committed / resident memory. Values in MB. Walks the address space with VirtualQuery
    // (about a millisecond); only done on request, safe from any thread.
    std::string MemoryStatus();
}
