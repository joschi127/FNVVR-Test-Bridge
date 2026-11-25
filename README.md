# FNVVR-Test-Bridge

A small [xNVSE](https://github.com/xNVSE/NVSE) plugin for *Fallout: New Vegas* that lets test tools on the
same PC drive the running game, plus Python tools that also speak to FNVVR's VR test channel. It exists so
that tests (and AI agents running them) do not need a person at the console or in the headset.

Part of the [FNVVR](https://github.com/joschi127/FNVVR) test setup, but independent of it: it works with or
without FNVVR and with or without the co-op mod
([FNVVR-Coop-Compat](https://github.com/joschi127/FNVVR-Coop-Compat)). Why it is a separate project:
[FNVVR ADR 0007](https://github.com/joschi127/FNVVR/blob/coop-compat/docs/adr/0007-where-test-automation-lives.md).

It does nothing unless switched on. Switched on and idle it costs no measurable frame time (measured), so it
may stay on while playing; switch it off when measuring the performance of other changes.

## Features

| Feature | Request | Notes |
|---|---|---|
| Run console commands | `<console line>` or `run <line>` | Answer carries the text the console printed (many game commands print only when typed into the console itself; read values with `eval`) |
| Read values | `eval <expression>` | Any xNVSE expression: `player.getav health`, `getstage VCG01` ... |
| Act on another reference | `on <ref id> <line>`, `evalon <ref id> <expr>` | Hex form ID |
| Game state | `state` | Game frame, world loaded, current cell, console open |
| Wait for frames | `wait <n>`, `waitframe <n>` | The game keeps running |
| Memory status | `memory` | Free/largest/used address space, private and resident memory, MB |
| Tell the person in the headset | `notify [seconds] <text>` | The game's HUD message with a sound (default 8 s) |
| Ask the person in the headset | `ask <text> \| <button> ...`, `answer` | The game's message box, one to four buttons |
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

## Working with a person in the headset

**Guided headset sessions** ([docs/headset-sessions.md](docs/headset-sessions.md)): the test tool greets the
person in the game, gives short numbered steps in the HUD, asks questions in the game's message box, takes a
hands-up gesture as "done" and thanks them at the end, so they never take the headset off and many topics fit
into one session. The guide has the recommended flow, rules that proved useful and a complete example
([examples/headset_session.py](examples/headset_session.py)).

<img src="https://github.com/user-attachments/assets/4ece22cf-94ed-4203-b3f5-1c2e2c4924f9" width="390" alt="A question from the test tool over the Pip-Boy panel: Casing and round corners ok? Yes, No, Not sure; answered with the laser">

While someone tests in the headset, they cannot read the test tool's output. `fnvtest.headset` talks to them
in the game:

```python
from fnvtest import Session, headset
with Session(game=True, vr=True) as s:
    headset.start(s)                              # turns the done signal on and says "Raise both hands to signal me"
    headset.notify(s, "1/3: open the Pip-Boy")    # HUD message with a sound, 8 s
    headset.remind(s, "1/3: open the Pip-Boy",    # again every 30 s until done() is true
                   done=lambda: headset.pipboy(s)["state"] == "open")
    choice = headset.ask(s, "Casing ok?", ["Yes", "No"])   # message box, waits for the click
    if headset.signal_count(s):                   # both hands held above the head for about a second
        ...
```

- Keep notifications short: a HUD message is on screen only for its seconds, and a long one is hard to read
  in time. Repeat it when the person does not react (`remind`).
- Start every session with `headset.start()`: the done signal (both hands above the head, counted by FNVVR's
  VR channel) is off until a tool turns it on, so it never fires while someone plays alone with the channels
  left on. Agree beforehand what a signal means (usually "done"); when one comes without that, ask what it was
  for (`ask` with the guesses as buttons) instead of guessing.
- A question is a menu: depending on the game's settings and mods the world may pause, and it covers an open
  Pip-Boy. Only one question at a time.
- Watching: `python -I tools/watch.py <log>` waits until a world is loaded, then logs menu and hover changes;
  lines written to `<log>.cmd` are sent through the same connection (the bridge takes one client at a time).
  `python -I tools/window_shot.py "<window title>" <file.png>` saves a window, for example FNVVR's emulator
  preview ("VR preview (left eye | right eye)").
- The Pip-Boy: `headset.open_pipboy(s)` opens it the way looking at the wrist does, `open_pipboy_by_gesture(s)`
  by setting the head and left hand so that the wrist is looked at, `pipboy(s)` tells its state and where its
  screen is (all through FNVVR's VR channel).

## Without a headset or a screen

- No headset: run FNVVR with its emulator (`d3d9.vrBackend = emulator`), with SteamVR's null driver (OpenVR),
  or with the [null OpenXR runtime](openxr-null-runtime/README.md) in this repository (OpenXR, no VR software
  needed).
- No screen: FNVVR's captures (`vr:capture eyes`, `vr:capture panel`) are read back from the GPU, so tests run
  and give full images with the monitor switched off or showing another input (tried with the emulator and the
  null OpenXR runtime). `tools/window_shot.py` needs the window to be shown. A PC with no display at all
  reported by Windows is untested.

## Performance and memory

Idle and enabled, the plugin does one atomic load per game frame; its pipe thread reserves 64 KB of stack.
Measured in VR (2026-11-22, bridge and FNVVR's VR channel on and idle against both off, same spot, 90 fps):
game thread about 6.3 against 6.6 ms per frame, process about 1.10 against 1.15 cores, so no measurable cost. Memory: the game is a 32-bit process, so per request the plugin only holds
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
