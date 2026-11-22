// FNVVR-Test-Bridge: lets local test tools drive Fallout: New Vegas through a named pipe.
// Copyright (c) 2026 joschi127. zlib licence, see LICENSE.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Pure parsing and formatting of the line protocol (no game, no Windows), so it can be unit tested.
// See docs/protocol.md.
namespace bridge
{
    constexpr size_t kMaxLine = 4096;

    enum class Verb
    {
        Ping,     // ping                      -> ok pong <version>
        State,    // state                     -> ok frame=<n> world=<0|1> cell=<hex> console=<0|1>
        Wait,     // wait <frames>             -> ok frame=<n>, after that many game frames
        WaitFrame,// waitframe <n>             -> ok frame=<n>, once game frame n is reached
        Run,      // run <console line>, or a bare console line
        Eval,     // eval <expression>         -> ok <value>
        RunOn,    // on <ref id hex> <console line>
        EvalOn,   // evalon <ref id hex> <expression>
        Invalid,  // see Request::error
    };

    struct Request
    {
        Verb verb = Verb::Invalid;
        uint32_t refId = 0;  // RunOn / EvalOn
        uint64_t count = 0;  // Wait / WaitFrame
        std::string text;    // console line or expression
        std::string error;   // Invalid: why
    };

    Request Parse(std::string_view line);

    // Collects bytes from the pipe and hands out complete lines ("\n", a "\r" before it is dropped).
    // A line longer than the limit is reported once as overflow and its rest is skipped.
    class LineBuffer
    {
    public:
        explicit LineBuffer(size_t limit) : m_limit(limit) {}
        void Feed(std::string_view bytes);
        // True if a line (or an overflow) is ready.
        bool Next(std::string& line, bool& overflow);

    private:
        size_t m_limit;
        std::string m_data;
        bool m_dropping = false;   // inside an over-long line, until its "\n"
        bool m_overflowed = false; // an overflow waits to be reported
    };

    // Console text -> "| " lines (one per line of text, empty lines dropped, \r removed).
    std::vector<std::string> ConsoleLines(std::string_view text);

    std::string Ok(std::string_view detail = {});
    std::string Err(std::string_view detail);
}
