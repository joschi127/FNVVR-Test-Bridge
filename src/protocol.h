// FNVVR-Test-Bridge: lets local test tools drive Fallout: New Vegas through a named pipe.
// Copyright (c) 2026 joschi127. zlib licence, see LICENSE.

#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

// Pure parsing and formatting of the line protocol (no game, no Windows), so it can be unit tested.
// See docs/protocol.md.
namespace bridge
{
    constexpr size_t kMaxLine = 4096;
    constexpr size_t kMaxButtons = 4;
    constexpr float kDefaultNotifySeconds = 8.0f;

    enum class Verb
    {
        Ping,     // ping                      -> ok pong <version>
        Memory,   // memory                    -> ok vfree=.. vlargest=.. vused=.. vtotal=.. private=.. workingset=.. (MB)
        State,    // state                     -> ok frame=<n> world=<0|1> cell=<hex> console=<0|1>
        Wait,     // wait <frames>             -> ok frame=<n>, after that many game frames
        WaitFrame,// waitframe <n>             -> ok frame=<n>, once game frame n is reached
        Run,      // run <console line>, or a bare console line
        Eval,     // eval <expression>         -> ok <value>
        RunOn,    // on <ref id hex> <console line>
        EvalOn,   // evalon <ref id hex> <expression>
        Notify,   // notify [seconds] <text>   -> ok, the game's HUD message with a sound
        Ask,      // ask <text> | <button> ... -> ok, the game's message box with up to four buttons
        Answer,   // answer                    -> ok none | ok pending | ok <index> <button>
        Invalid,  // see Request::error
    };

    struct Request
    {
        Verb verb = Verb::Invalid;
        uint32_t refId = 0;  // RunOn / EvalOn
        uint64_t count = 0;  // Wait / WaitFrame
        float seconds = 8.0f;              // Notify: time on screen
        std::string text;                  // console line, expression, notification or question
        std::vector<std::string> buttons;  // Ask
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
        struct Item
        {
            std::string line;
            bool overflow = false;
        };

        size_t m_limit;
        std::string m_current;
        bool m_dropping = false;  // inside an over-long line, until its "\n"
        std::deque<Item> m_ready; // complete lines and overflows, in the order they arrived
    };

    // Console text -> "| " lines (one per line of text, empty lines dropped, \r removed).
    std::vector<std::string> ConsoleLines(std::string_view text);

    std::string Ok(std::string_view detail = {});
    std::string Err(std::string_view detail);
}
