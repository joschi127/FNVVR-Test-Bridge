# FNVVR-Test-Bridge

A small [xNVSE](https://github.com/xNVSE/NVSE) plugin for *Fallout: New Vegas* that lets test tools on the
same PC drive the running game, plus Python tools that also speak to FNVVR's VR test channel. It exists so
that tests (and AI agents running them) do not need a person at the console or in the headset.

Part of the [FNVVR](https://github.com/joschi127/FNVVR) test setup, but independent of it: it works with or
without FNVVR and with or without the co-op mod
([FNVVR-Coop-Compat](https://github.com/joschi127/FNVVR-Coop-Compat)). Why it is a separate project:
[FNVVR ADR 0007](https://github.com/joschi127/FNVVR/blob/coop-compat/docs/adr/0007-where-test-automation-lives.md).

**Not for normal play.** It does nothing unless switched on; keep it off while playing and when measuring
performance.

## Features

| Feature | Request | Notes |
|---|---|---|
| Run console commands | `<console line>` or `run <line>` | Answer carries the text the console printed |
| Read values | `eval <expression>` | Any xNVSE expression: `player.getav health`, `getstage VCG01` ... |
| Act on another reference | `on <ref id> <line>`, `evalon <ref id> <expr>` | Hex form ID |
| Game state | `state` | Game frame, world loaded, current cell, console open |
| Wait for frames | `wait <n>`, `waitframe <n>` | The game keeps running |
| Memory status | `memory` | Free/largest/used address space, private and resident memory, MB |
| Alive check | `ping` | |

Changes are only in memory, like typing into the console: nothing is saved unless someone saves. Full
request and answer format: [docs/protocol.md](docs/protocol.md).

## Requirements

- `FalloutNV.exe` 1.4.0.525 (other versions are refused), xNVSE 6.2.6 or newer.
- Python 3 for the tools (no extra packages).

## Install and switch on

1. Copy `FNVVR-Test-Bridge.dll` and [`data/FNVVR-Test-Bridge.ini`](data/FNVVR-Test-Bridge.ini) to
   `Data\NVSE\Plugins\`.
2. For a test session set `Enabled=1` in the INI (read at game start). `Enabled=0`: no pipe, no thread, no
   log, nothing per frame.
3. `FNVVR-Test-Bridge.log` next to `FalloutNV.exe` names the pipe (`\\.\pipe\FNVVR-Test-Bridge`). Only the
   Windows user running the game can connect, and only from the same PC.

## Use

The tools in [`tools/`](tools/) speak to two channels; each is optional:

| Channel | Needs | Gives |
|---|---|---|
| game | this plugin (`Enabled=1`), xNVSE | the requests above |
| vr | FNVVR with `d3d9.vrTestChannel = True` | head/hand poses, actions, sticks, aiming at panels, captures, VR frame, memory ([protocol](https://github.com/joschi127/FNVVR/blob/coop-compat/docs/testing/vr-test-channel.md)) |

Neither needs the co-op mod: a flat game with xNVSE has the game channel, FNVVR without xNVSE the vr channel,
FNVVR with xNVSE both.

Command line (each argument is one request; `vr:` sends it to the vr channel; `sync` measures the offset
between the two frame counters):

```
python -I tools/fnvtest.py ping state memory "player.additem 000E3778 1" "eval player.getitemcount 000E3778"
python -I tools/fnvtest.py vr:state "vr:aim right menu 0.5 0.9" "vr:press menuselect" sync
```

In test scripts:

```python
from fnvtest import Session            # tools/ on sys.path
with Session(game=True, vr="auto") as s:   # True = required, "auto" = if reachable, False = not used
    s.run("player.additem 000E3778 1")
    print(s.eval("player.getitemcount 000E3778").detail)
    print(s.memory()["vlargest"])
    if s.vr:
        s.vr_do("aim right menu 0.5 0.9"); s.vr_do("press menuselect"); s.wait_frames(10)
```

Every answer is an `Answer` with `ok`, `detail`, `console` (printed lines) and `fields()` (`key=value` pairs).

## Performance and memory

Idle and enabled, the plugin does one atomic load per game frame; its pipe thread reserves 64 KB of stack.
Whether that is measurable is checked by comparing frame times with `Enabled=0` and `Enabled=1`; until then
keep `Enabled=0` for real play. Memory: the game is a 32-bit process, so per request the plugin only holds
the request and its answer; compiled `eval` expressions are bounded (see the protocol).

## Documentation rule

Every feature of the bridge and the tools is documented here and in [docs/protocol.md](docs/protocol.md):
short, what it does and how to use it. A change that adds or alters a feature updates both in the same
commit.

## Build

Windows, Visual Studio 2026 with C++ and CMake:

```
git submodule update --init
cmake --preset x86
cmake --build --preset release      # build/Release/FNVVR-Test-Bridge.dll
ctest --preset release              # protocol and Python tool tests (no game needed)
```

Only xNVSE's headers are used; game addresses come from xNVSE's public headers and sources.

## Licence

zlib, see [LICENSE](LICENSE). xNVSE is a separate project with its own terms.

Developed with AI assistance; tested and maintained by the repository owner.
