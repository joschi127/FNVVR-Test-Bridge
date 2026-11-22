"""Client for FNVVR-Test-Bridge: send lines to the running game, print the answers.

Each argument is one request (see docs/protocol.md):
  ping | state | run <console line> | <console line> | eval <expr> | on <refid> <line> | evalon <refid> <expr>
Console text the game prints comes back as "| ..." lines, the last line is "ok ..." or "err ...".

  python -I tools/bridge.py ping "player.additem 000E3778 1" "eval player.getitemcount 000E3778"
  python -I tools/bridge.py --pipe FNVVR-Test-Bridge-1234 state

Exit code: 0 all ok, 1 an "err" answer, 2 bridge not reachable, 3 connection lost.
Usable as a module too: with Bridge() as b: lines, ok = b.ask("state").
"""
import argparse
import sys
import time

DEFAULT_PIPE = "FNVVR-Test-Bridge"


class BridgeError(Exception):
    pass


class Bridge:
    def __init__(self, pipe=DEFAULT_PIPE, connect_timeout=3.0):
        path = r"\\.\pipe" + "\\" + pipe
        end = time.monotonic() + connect_timeout
        while True:
            try:
                self._f = open(path, "r+b", buffering=0)
                break
            except OSError as e:
                if time.monotonic() >= end:
                    raise BridgeError(f"bridge not reachable at {path} (game not running, Enabled=0, "
                                      f"or another tool connected): {e}") from e
                time.sleep(0.1)
        self._buf = b""

    def close(self):
        self._f.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def _line(self):
        while b"\n" not in self._buf:
            chunk = self._f.read(65536)
            if not chunk:
                raise BridgeError("connection lost")
            self._buf += chunk
        line, self._buf = self._buf.split(b"\n", 1)
        return line.decode("cp1252", "replace")

    def ask(self, request):
        """Returns (console_lines, final_line, ok)."""
        self._f.write(request.encode("cp1252", "replace") + b"\n")
        console = []
        while True:
            line = self._line()
            if line.startswith("| "):
                console.append(line[2:])
            else:
                return console, line, line == "ok" or line.startswith("ok ")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("requests", nargs="+")
    ap.add_argument("--pipe", default=DEFAULT_PIPE)
    ap.add_argument("--connect", type=float, default=3.0, help="seconds to wait for the pipe")
    a = ap.parse_args()
    try:
        bridge = Bridge(a.pipe, a.connect)
    except BridgeError as e:
        print(e, file=sys.stderr)
        return 2
    rc = 0
    with bridge:
        for request in a.requests:
            try:
                console, final, ok = bridge.ask(request)
            except BridgeError as e:
                print(e, file=sys.stderr)
                return 3
            print(f"> {request}")
            for line in console:
                print(f"  | {line}")
            print(f"< {final}")
            sys.stdout.flush()
            if not ok:
                rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
