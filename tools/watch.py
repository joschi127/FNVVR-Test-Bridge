"""Watches a session with a person in the game: waits until a world is loaded, then logs whether a menu is
open and which interface element the cursor or laser is on (JIP's GetActiveUIComponentName), one line per
change, until the game closes. Commands written to <log>.cmd, one per line, are sent through the same
connection (the bridge takes one client at a time), for example notifications while the person plays.

Usage: python -I tools/watch.py <log file> [wait minutes] [--wait-only]
  --wait-only  exits as soon as a world is loaded (prints "IN GAME")

FNVVR-Test-Bridge. Copyright (c) 2026 joschi127. zlib licence, see LICENSE.
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fnvtest import Session  # noqa: E402


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if not args:
        print(__doc__)
        return 2
    log_path = args[0]
    cmd_path = log_path + ".cmd"
    deadline = time.time() + 60 * float(args[1] if len(args) > 1 else 30)
    wait_only = "--wait-only" in sys.argv
    announced = False

    with open(log_path, "a", encoding="utf-8", buffering=1) as out:
        def note(text):
            out.write(f"{time.strftime('%H:%M:%S')} {text}\n")

        while time.time() < deadline:
            try:
                with Session(game=True, vr=False, connect_timeout=5) as s:
                    last = None
                    while True:
                        state = s.run("state").final
                        if not announced:
                            if "world=1" not in state:
                                time.sleep(3)
                                continue
                            print("IN GAME", time.strftime("%H:%M:%S"), state, flush=True)
                            note(f"IN GAME {state}")
                            announced = True
                            if wait_only:
                                return 0
                        try:
                            with open(cmd_path, "r+", encoding="utf-8") as f:
                                commands = [c.strip() for c in f.read().splitlines() if c.strip()]
                                f.seek(0)
                                f.truncate()
                        except FileNotFoundError:
                            commands = []
                        for command in commands:
                            note(f"sent {command} -> {s.run(command).final}")
                        menu = s.eval("MenuMode").detail.strip()
                        hover = s.eval("GetActiveUIComponentName")
                        now = (menu, hover.detail if hover.ok else "?")
                        if now != last:
                            note(f"menu={now[0]} hover={now[1]}")
                            last = now
                        time.sleep(0.25)
            except Exception as e:  # not running yet, or closed
                if announced:
                    note(f"game closed ({type(e).__name__})")
                    print("GAME CLOSED", time.strftime("%H:%M:%S"), flush=True)
                    return 0
                time.sleep(5)
    print("wait ran out", flush=True)
    return 1


if __name__ == "__main__":
    sys.exit(main())
