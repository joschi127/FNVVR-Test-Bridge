"""Command line for the test channels.

Each argument is one request. Requests go to the game channel (FNVVR-Test-Bridge) unless prefixed
with "vr:" (FNVVR's VR test channel); "sync" measures the offset between their frame counters.

  python -I tools/fnvtest.py ping state "player.additem 000E3778 1" "eval player.getitemcount 000E3778"
  python -I tools/fnvtest.py vr:state "vr:aim right menu 0.5 0.9" "vr:press menuselect" sync

Exit code: 0 all ok, 1 an "err" answer, 2 a needed channel is not reachable, 3 connection lost.

FNVVR-Test-Bridge. Copyright (c) 2026 joschi127. zlib licence, see LICENSE.
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from fnvtest import ChannelError, Session, GAME_PIPE, VR_PIPE  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("requests", nargs="+")
    ap.add_argument("--game-pipe", default=GAME_PIPE)
    ap.add_argument("--vr-pipe", default=VR_PIPE)
    ap.add_argument("--connect", type=float, default=3.0, help="seconds to wait for a pipe")
    a = ap.parse_args()

    need_vr = any(r.startswith("vr:") or r == "sync" for r in a.requests)
    need_game = any(not r.startswith("vr:") for r in a.requests)
    try:
        session = Session(game=need_game, vr=need_vr, game_pipe=a.game_pipe, vr_pipe=a.vr_pipe,
                          connect_timeout=a.connect)
    except ChannelError as e:
        print(e, file=sys.stderr)
        return 2

    rc = 0
    with session:
        for request in a.requests:
            try:
                if request == "sync":
                    print(f"> sync\n< offset vr-game = {session.sync()} frames")
                    continue
                if request.startswith("vr:"):
                    answer = session.vr_do(request[3:].strip())
                else:
                    answer = session.run(request)
            except ChannelError as e:
                print(e, file=sys.stderr)
                return 3
            print(f"> {request}")
            for line in answer.console:
                print(f"  | {line}")
            print(f"< {answer.final}")
            sys.stdout.flush()
            if not answer.ok:
                rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
