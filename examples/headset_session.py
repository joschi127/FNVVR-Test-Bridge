"""A guided headset session, as in docs/headset-sessions.md: greet, give short numbered steps, ask in between,
take the hands-up signal for "done", thank. Several topics in one session.

Needs the game channel (this plugin, Enabled=1) and FNVVR's VR channel (d3d9.vrTestChannel = True).
Usage (from the repository root): python -I examples/headset_session.py [log file]

FNVVR-Test-Bridge. Copyright (c) 2026 joschi127. zlib licence, see LICENSE.
"""
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
from fnvtest import Session, headset  # noqa: E402

LOG = open(sys.argv[1], "a", encoding="utf-8", buffering=1) if len(sys.argv) > 1 else None


def say(text):
    line = f"{time.strftime('%H:%M:%S')} {text}"
    print(line, flush=True)
    if LOG:
        LOG.write(line + "\n")


def wait_for_world():
    """Waits until the person has loaded a save; they need not tell you."""
    while True:
        try:
            with Session(game=True, vr=True, connect_timeout=5) as s:
                while "world=1" not in s.run("state").final:
                    time.sleep(3)
                return
        except Exception:
            time.sleep(5)


def step(s, text, done, every=headset.REPEAT_AFTER, timeout=600):
    """Shows a step and repeats it until done() is true or the person signals; returns "done", "signal" or
    "timeout". The signal counter is read through the VR channel."""
    start, shown = time.monotonic(), None
    signals = headset.signal_count(s) or 0
    while time.monotonic() - start < timeout:
        if done():
            return "done"
        if (headset.signal_count(s) or 0) > signals:
            return "signal"
        if shown is None or time.monotonic() - shown >= every:
            headset.notify(s, text)
            say(f"notify: {text}")
            shown = time.monotonic()
        time.sleep(0.3)
    return "timeout"


def ask(s, text, buttons):
    say(f"ask: {text} {buttons}")
    reply = headset.ask(s, text, buttons, timeout=300)
    say(f"answer: {reply}")
    return reply


def main():
    say("waiting for a loaded world")
    wait_for_world()
    with Session(game=True, vr=True, connect_timeout=10) as s:
        # Greeting: turns the done signal on and tells how to give it
        headset.start(s, "Hi! Raise both hands to signal me")
        say("notify: Hi! Raise both hands to signal me")
        time.sleep(6)

        # Topic 1: something to check by looking, then a question
        r = step(s, "1/3: look at your left wrist", lambda: headset.pipboy(s)["state"] == "open")
        if r == "signal":
            # No "done" was agreed for this step: ask what the signal was for instead of guessing
            ask(s, "You signalled - what for?", ["It won't open", "Problem", "Pause"])
        ask(s, "Does the Pip-Boy panel look right?", ["Yes", "No", "Not sure"])

        # Topic 2: something to do, with the signal as "done"
        r = step(s, "2/3: click STATS, then DATA. Hands up when done", lambda: False, timeout=300)
        say(f"step 2: {r}")
        ask(s, "Did DATA open with one click?", ["Yes", "No"])

        # Topic 3: feedback on the session itself
        ask(s, "Were my messages easy to read?", ["Yes", "Too short", "Too long"])

        # Thanks; the signal goes off again
        headset.notify(s, "3/3: done, thank you!")
        say("notify: done")
        s.vr.ask("signal off")


if __name__ == "__main__":
    main()
