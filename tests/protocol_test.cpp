// FNVVR-Test-Bridge. Copyright (c) 2026 joschi127. zlib licence, see LICENSE.

#include "protocol.h"

#include <cstdio>
#include <string>

using namespace bridge;

static int g_failed = 0;

#define CHECK(cond)                                                       \
    do                                                                    \
    {                                                                     \
        if (!(cond))                                                      \
        {                                                                 \
            std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failed;                                                   \
        }                                                                 \
    } while (0)

static void ParseVerbs()
{
    CHECK(Parse("ping").verb == Verb::Ping);
    CHECK(Parse("  PING \r").verb == Verb::Ping);
    CHECK(Parse("state").verb == Verb::State);
    CHECK(Parse("memory").verb == Verb::Memory);

    Request run = Parse("run player.additem 000E3778 1");
    CHECK(run.verb == Verb::Run && run.text == "player.additem 000E3778 1");

    Request bare = Parse("player.additem 000E3778 1");
    CHECK(bare.verb == Verb::Run && bare.text == "player.additem 000E3778 1");

    Request eval = Parse("eval player.getitemcount 000E3778");
    CHECK(eval.verb == Verb::Eval && eval.text == "player.getitemcount 000E3778");

    Request on = Parse("on 0x0010A3F2 kill");
    CHECK(on.verb == Verb::RunOn && on.refId == 0x0010A3F2 && on.text == "kill");

    Request evalon = Parse("evalon 14 getav health");
    CHECK(evalon.verb == Verb::EvalOn && evalon.refId == 0x14 && evalon.text == "getav health");

    Request wait = Parse("wait 5");
    CHECK(wait.verb == Verb::Wait && wait.count == 5);
    CHECK(Parse("waitframe 123").verb == Verb::WaitFrame && Parse("waitframe 123").count == 123);

    // "ping" with arguments is an ordinary console line, not the ping verb.
    CHECK(Parse("ping x").verb == Verb::Run);
}

static void ParseErrors()
{
    CHECK(Parse("").verb == Verb::Invalid);
    CHECK(Parse("   ").verb == Verb::Invalid);
    CHECK(Parse("run").verb == Verb::Invalid);
    CHECK(Parse("eval   ").verb == Verb::Invalid);
    CHECK(Parse("on zz kill").verb == Verb::Invalid);
    CHECK(Parse("on 14").verb == Verb::Invalid);
    CHECK(Parse("on 123456789 kill").verb == Verb::Invalid);
    CHECK(Parse("wait").verb == Verb::Invalid);
    CHECK(Parse("wait x").verb == Verb::Invalid);
    CHECK(Parse(std::string("a\x01" "b")).verb == Verb::Invalid);
    CHECK(Parse(std::string(kMaxLine + 1, 'a')).verb == Verb::Invalid);
}

static void Lines()
{
    LineBuffer b(16);
    std::string line;
    bool overflow = true;
    CHECK(!b.Next(line, overflow));
    b.Feed("ping\r\nsta");
    CHECK(b.Next(line, overflow) && line == "ping" && !overflow);
    CHECK(!b.Next(line, overflow));
    b.Feed("te\n");
    CHECK(b.Next(line, overflow) && line == "state" && !overflow);

    // Over-long line: one overflow, its rest skipped, the next line is fine.
    b.Feed("0123456789abcdefXYZ");
    CHECK(b.Next(line, overflow) && overflow);
    CHECK(!b.Next(line, overflow));
    b.Feed("more\nok\n");
    CHECK(b.Next(line, overflow) && line == "ok" && !overflow);
    CHECK(!b.Next(line, overflow));

    // Overflow and a following line in one read: answers keep the order of the requests
    b.Feed("0123456789abcdefXYZ\nping\n");
    CHECK(b.Next(line, overflow) && overflow);
    CHECK(b.Next(line, overflow) && line == "ping" && !overflow);
}

static void Headset()
{
    Request n = Parse("notify Raise both hands");
    CHECK(n.verb == Verb::Notify && n.text == "Raise both hands" && n.seconds == kDefaultNotifySeconds);
    Request t = Parse("notify 15 Look at the wrist");
    CHECK(t.verb == Verb::Notify && t.text == "Look at the wrist" && t.seconds == 15.0f);
    Request big = Parse("notify 500 apples");
    CHECK(big.verb == Verb::Notify && big.text == "500 apples");
    CHECK(Parse("notify").verb == Verb::Invalid);
    CHECK(Parse("notify 10").verb == Verb::Invalid);

    Request a = Parse("ask Done? | Yes | No ");
    CHECK(a.verb == Verb::Ask && a.text == "Done?" && a.buttons.size() == 2 && a.buttons[1] == "No");
    CHECK(Parse("ask Done?").verb == Verb::Invalid);
    CHECK(Parse("ask Done? | Yes | | No").verb == Verb::Invalid);
    CHECK(Parse("ask Q | 1 | 2 | 3 | 4 | 5").verb == Verb::Invalid);
    CHECK(Parse("answer").verb == Verb::Answer);
}

static void Console()
{
    auto l = ConsoleLines("first\r\n\r\n  \nsecond");
    CHECK(l.size() == 2 && l[0] == "| first" && l[1] == "| second");
    CHECK(ConsoleLines("").empty());
    CHECK(Ok() == "ok" && Ok("1") == "ok 1" && Err("x") == "err x");
}

int main()
{
    ParseVerbs();
    ParseErrors();
    Lines();
    Headset();
    Console();
    std::printf(g_failed ? "%d check(s) failed\n" : "all checks passed\n", g_failed);
    return g_failed ? 1 : 0;
}
