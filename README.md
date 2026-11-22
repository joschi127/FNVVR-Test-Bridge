# FNVVR-Test-Bridge

A small [xNVSE](https://github.com/xNVSE/NVSE) plugin for *Fallout: New Vegas* that lets test tools on the
same PC drive the running game: run console commands, evaluate script expressions and read back what the
console prints. It exists so that tests (and AI agents running them) do not need a person to type into the
console.

Part of the [FNVVR](https://github.com/joschi127/FNVVR) test setup, but independent of it: it works with or
without FNVVR, with or without the co-op mod
([FNVVR-Coop-Compat](https://github.com/joschi127/FNVVR-Coop-Compat)), flat or in VR. Why it is a separate
project and how the test automation is split between the projects:
[FNVVR ADR 0007](https://github.com/joschi127/FNVVR/blob/coop-compat/docs/adr/0007-where-test-automation-lives.md).

**Not for normal play.** It is off unless switched on, and it is meant to stay off while playing.

## What it does

- Listens on a named pipe (`\\.\pipe\FNVVR-Test-Bridge`) for the Windows user running the game; nobody
  else and no remote machine can connect.
- Runs each request on the game's main thread and answers with the console text it caused and a final
  `ok`/`err` line. Protocol: [docs/protocol.md](docs/protocol.md).
- Changes are only in memory, like typing into the console: nothing is saved unless someone saves.

## Requirements

- `FalloutNV.exe` 1.4.0.525 (the plugin refuses other versions), xNVSE 6.x.

## Install

1. Copy `FNVVR-Test-Bridge.dll` and [`data/FNVVR-Test-Bridge.ini`](data/FNVVR-Test-Bridge.ini) to
   `Data\NVSE\Plugins\`.
2. Set `Enabled=1` in the INI for a test session; the INI is read at game start.
3. The log `FNVVR-Test-Bridge.log` next to `FalloutNV.exe` names the pipe it listens on.

With `Enabled=0` the plugin does nothing at all: no pipe, no thread, no log, no game-loop listener.

## Use

The Python tools in [`tools/`](tools/) speak to both test channels of the FNVVR setup; each is optional:

| Channel | Pipe | Needs | Gives |
|---|---|---|---|
| game | `FNVVR-Test-Bridge` | this plugin (`Enabled=1`), xNVSE | console lines, `eval`, console text, game frame number |
| vr | `FNVVR-Test-VR` | FNVVR with `d3d9.vrTestChannel = True` | head/hand poses, actions, sticks, aiming at panels, captures, VR frame number ([protocol](https://github.com/joschi127/FNVVR/blob/coop-compat/docs/testing/vr-test-channel.md)) |

Neither needs the co-op mod. What works with which setup:

| Setup | game | vr |
|---|---|---|
| Flat game with xNVSE | yes | no |
| FNVVR alone, without xNVSE | no | yes |
| FNVVR + xNVSE | yes | yes |
| FNVVR + xNVSE + co-op mod | yes | yes |

```
python -I tools/fnvtest.py ping state "player.additem 000E3778 1" "eval player.getitemcount 000E3778"
python -I tools/fnvtest.py vr:state "vr:aim right menu 0.5 0.9" "vr:press menuselect" sync
```

Requests go to the game channel unless prefixed with `vr:`; `sync` measures the offset between the two
frame counters. In test scripts: `from fnvtest import Session` (`Session(game=True, vr="auto")`, then
`run`, `eval`, `vr_do`, `sync`, `frame`, `wait_frames`, `wait_until`).

## Performance

Requirement: the bridge must not cost frame time. When enabled and idle it does one atomic load per frame;
whether that is measurable has to be checked (frame times with `Enabled=0` against `Enabled=1`, idle) before
it is relied on. Until then: `Enabled=0` for real play and for performance measurements.

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
