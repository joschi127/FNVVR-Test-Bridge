// FNVVR-Test-Bridge. Copyright (c) 2026 joschi127. zlib licence, see LICENSE.

#pragma once

#include "protocol.h"

#include <string>
#include <vector>

struct NVSEInterface;

namespace bridge
{
    // Fetches the xNVSE interfaces the bridge needs. False if one is missing (old xNVSE).
    bool InitGame(const NVSEInterface* nvse);

    // Main thread only. Runs one parsed request and returns all answer lines (console text as "| ...",
    // the last line "ok ..." or "err ...").
    std::vector<std::string> Execute(const Request& request, uint64_t frame);

    // Forgets the compiled eval expressions (on loading a save or a new game: they may name forms that are gone)
    void ForgetCompiled();
}
