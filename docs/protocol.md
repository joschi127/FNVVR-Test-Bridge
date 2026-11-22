# Protocol

Named pipe `\\.\pipe\FNVVR-Test-Bridge` (name from `PipeName` in the INI). If another game on the same PC
already uses that name, the second one listens on `FNVVR-Test-Bridge-<process id>`; the log names the pipe.

- One tool at a time. Only the Windows user running the game can open the pipe; remote clients are
  rejected.
- Bytes are the game's code page (Windows-1252). Lines end with `\n` (a `\r` before it is ignored).
- One request line in, one or more answer lines back. Lines starting with `| ` are text the game's console
  printed while the request ran, in order. The last line is `ok`, `ok <detail>` or `err <reason>`.
- Requests run on the game's main thread, at most 8 per frame, also in the main menu. During loading
  screens they wait (except `memory`, which the pipe answers itself). A request without an answer after
  60 seconds gets an `err` and is dropped, so later requests do not wait behind it (split longer waits).
- A request line may be at most 4096 bytes and must not contain control characters (tab is allowed).

| Request | Answer | Notes |
|---|---|---|
| `ping` | `ok pong <version>` | |
| `memory` | `ok vfree=<MB> vlargest=<MB> vused=<MB> vtotal=<MB> private=<MB> workingset=<MB>` | The game process's address space (free, largest free block, used, total) and committed / resident memory. Also during loading screens. |
| `state` | `ok frame=<n> world=<0\|1> cell=<hex> console=<0\|1>` | frame: game frames since the plugin loaded; world: the player is in a cell; cell: its form ID; console: the console is open |
| `wait <frames>` | `ok frame=<n>` | Answers after that many game frames (the game keeps running) |
| `waitframe <n>` | `ok frame=<n>` | Answers once game frame `n` is reached |
| `run <line>` or just `<line>` | console text, `ok` / `err the console did not take it` | Any console line, as typed in the console, except `scof` (the bridge uses the console output file itself) |
| `eval <expression>` | console text, `ok <value>` / `err ...` | Value: a number, `form <hex>`, `"<string>"`, `array` or `none`. Runs with the player as the calling reference. Needs a loaded world. |
| `on <ref id> <line>` | as `run` | Runs the line on that reference (hex form ID, `0x` optional) |
| `evalon <ref id> <expression>` | as `eval` | |
| `notify [seconds] <text>` | `ok` / `err ...` | The game's HUD message with a sound, for `seconds` (1-60, default 8; a first word outside that range is part of the text). Shown in the game's queue of HUD messages |
| `ask <text> \| <button> [\| <button> ...]` | `ok` / `err a question is open; see answer` | The game's message box with one to four buttons. One question at a time |
| `answer` | `ok pending` / `ok none` / `ok <index> <button>` | The open question's state: still open, no question asked, or the clicked button (index from 0) |

`eval` compiles `SetFunctionValue <expression>` as a user-defined function, so any xNVSE expression works
(`player.getav health`, `getstage VCG01`, `player.getitemcount 000E3778`, functions from JIP LN, JohnnyGuitar
and so on). xNVSE never frees a compiled script, so each new expression text costs a little memory for the
rest of the session: repeated expressions come from a cache, the cache is emptied when a save is loaded, and
after 1024 different expressions in one session `eval` answers `err` until the game is restarted. Expressions
with run-time form IDs (`FFxxxxxx`) are not cached.

Console text is captured by pointing the game's console output file (the mechanism behind the `scof`
command) at a temporary file for the duration of the request; a player's own `scof` setting is restored
afterwards. Whether this catches all console text is checked in the first game test.

Example (as printed by `tools/fnvtest.py`; console text, if any, appears as `| ...` lines):

```
> ping
< ok pong 0.1.0
> player.additem 000E3778 1
< ok
> eval player.getitemcount 000E3778
< ok 1
```
