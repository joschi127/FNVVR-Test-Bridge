"""Working with a person in the headset: notifications, questions, the done signal, opening the Pip-Boy.

notify() and ask() need the game channel (the bridge); the signal and the Pip-Boy need FNVVR's VR channel
(d3d9.vrTestChannel). See the README, "Working with a person in the headset".

FNVVR-Test-Bridge. Copyright (c) 2026 joschi127. zlib licence, see LICENSE.
"""
import math
import time

from . import steamvr

from .channels import ChannelError

NOTIFY_SECONDS = 8
REPEAT_AFTER = 30


def notify(session, text, seconds=NOTIFY_SECONDS):
    """Shows text as the game's HUD message with a sound, for seconds (1-60). Keep it short."""
    session.require(game=True)
    answer = session.game.ask(f"notify {int(seconds)} {text}")
    if not answer.ok:
        raise ChannelError(f"notify: {answer.final}")
    return answer


def remind(session, text, done, every=REPEAT_AFTER, seconds=NOTIFY_SECONDS, timeout=None, poll=1.0):
    """Notifies, and again every `every` seconds until done() returns true (or timeout). Returns done()."""
    start = time.monotonic()
    last = None
    while True:
        if done():
            return True
        now = time.monotonic()
        if timeout is not None and now - start > timeout:
            return False
        if last is None or now - last >= every:
            notify(session, text, seconds)
            last = now
        time.sleep(poll)


MAX_BUTTONS = 4


def check_buttons(buttons, extra=0):
    """Raises ValueError unless there are 1 to MAX_BUTTONS buttons (counting `extra` added by a helper), without
    "|" in their text: checked before anything is shown, so a script fails at once instead of mid-session."""
    buttons = list(buttons)
    if not 1 <= len(buttons) + extra <= MAX_BUTTONS:
        raise ValueError(f"{len(buttons)} button(s) given, 1 to {MAX_BUTTONS - extra} allowed here")
    if any("|" in b for b in buttons):
        raise ValueError("a button's text may not contain '|'")


class on_error:
    """Context manager for a session script: if the script fails, the person in the headset is told so (they see
    nothing else of it) before the error is raised again.

        with headset.on_error(s):
            ...
    """

    TEXT = "Session script error - please quit the game"

    def __init__(self, session, text=TEXT):
        self.session = session
        self.text = text

    def __enter__(self):
        return self

    def __exit__(self, kind, error, trace):
        if kind is not None and not issubclass(kind, KeyboardInterrupt):
            try:
                notify(self.session, self.text, 30)
            except Exception:
                pass
        return False


TOO_FAST = 2.0


def ask(session, text, buttons, timeout=None, poll=0.5, too_fast=TOO_FAST, retries=3):
    """Asks with the game's message box (one to four buttons); returns the chosen button's text, or None
    when the timeout ran out. Only one question at a time.

    An answer within too_fast seconds of the box appearing is taken for a stray press (a button the person
    meant for the game while the box popped up): it is thrown away and the question asked again, at most
    `retries` times."""
    session.require(game=True)
    check_buttons(buttons)
    for attempt in range(retries + 1):
        answer = session.game.ask("ask " + " | ".join([text] + list(buttons)))
        if not answer.ok:
            raise ChannelError(f"ask: {answer.final}")
        start = time.monotonic()
        reply = None
        while timeout is None or time.monotonic() - start < timeout:
            reply = session.game.ask("answer").detail.strip()
            if reply == "none":
                return None
            if reply != "pending":
                break
            time.sleep(poll)
        else:
            return None
        chosen = reply.split(" ", 1)[1] if " " in reply else reply
        if time.monotonic() - start >= too_fast or attempt == retries:
            return chosen
        notify(session, "Please answer again", 4)
        time.sleep(1.0)
    return None


RECHECK = "Let me re-check"
IGNORE_SIGNAL = 3.0


def look(session, hint, max_seconds=60, remind_after=30, poll=0.5):
    """A viewing pause before a question: shows what to look at, then lets the game run until the person raises
    both hands (the done signal must be on, see start) or max_seconds pass. Returns True when they signalled.

    A question's message box stops or covers the game and hides FNVVR's panels, so the person looks first.
    A signal in the first IGNORE_SIGNAL seconds does not count."""
    text = hint + " - raise both hands when ready"
    notify(session, text)
    begin = time.monotonic()
    # A signal in the first seconds is a leftover of the step before (hands still up), not this step's
    time.sleep(IGNORE_SIGNAL)
    base = signal_count(session) or 0
    reminded = False
    while time.monotonic() - begin < max_seconds:
        if (signal_count(session) or 0) > base:
            return True
        if not reminded and time.monotonic() - begin > remind_after:
            notify(session, text)
            reminded = True
        time.sleep(poll)
    return False


def ask_after_look(session, hint, text, buttons, tries=5, max_seconds=60):
    """A viewing pause (look), then the question with a "Let me re-check" button that repeats both; returns the
    chosen button's text (never the re-check), or None after too many re-checks. At most three buttons: the
    re-check is the fourth."""
    check_buttons(buttons, extra=1)
    for _ in range(tries):
        look(session, hint, max_seconds)
        answer = ask(session, text, list(buttons) + [RECHECK], timeout=300)
        if answer != RECHECK:
            return answer
    return None


def start(session, text="Raise both hands to signal me"):
    """The start of a session with a person in the headset: turns the done signal on and says so.

    Warns first when SteamVR is set to its null driver (see steamvr.py)."""
    steamvr.check()
    session.require(game=True, vr=True)
    session.vr.ask("signal on")
    notify(session, text)


def signal_count(session):
    """How often the person gave the done signal since it was turned on (None while it is off)."""
    session.require(vr=True)
    value = session.vr.ask("state").fields().get("signal", "off")
    return None if value == "off" else int(value)


# The Pip-Boy

def pipboy(session):
    """state (closed, changing, open), looking (bool), and screen / facing on the left controller
    (forward, up, right in metres, or None before the game placed it)."""
    session.require(vr=True)
    words = session.vr.ask("pipboy").detail.split()
    info = {"state": "closed", "looking": False, "screen": None, "facing": None}
    for i, word in enumerate(words):
        if word.startswith("state="):
            info["state"] = word[6:]
        elif word.startswith("looking="):
            info["looking"] = word[8:] == "1"
        elif word in ("screen", "facing") and i + 3 < len(words) and words[i + 1] != "unknown":
            info[word] = tuple(float(v) for v in words[i + 1:i + 4])
    return info


def wait_pipboy(session, state, frames=120):
    """Waits up to frames VR frames for the Pip-Boy to be in state; returns whether it is."""
    for _ in range(max(1, frames // 10)):
        if pipboy(session)["state"] == state:
            return True
        session.wait_frames(10)
    return pipboy(session)["state"] == state


def open_pipboy(session):
    """Opens the Pip-Boy the way looking at the wrist does (FNVVR's VR channel)."""
    session.vr.ask("pipboy open")
    return wait_pipboy(session, "open")


def close_pipboy(session):
    session.vr.ask("pipboy close")
    return wait_pipboy(session, "closed")


def _qmul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz)


def _qaxis(axis, deg):
    s = math.sin(math.radians(deg) / 2)
    return (axis[0] * s, axis[1] * s, axis[2] * s, math.cos(math.radians(deg) / 2))


def _rot(q, v):
    x, y, z, w = q
    p = _qmul(_qmul(q, (v[0], v[1], v[2], 0.0)), (-x, -y, -z, w))
    return p[:3]


def _pose_q(yaw, pitch, roll):
    # As the VR channel's poses: yaw about +Y, then pitch about +X, then roll about +Z
    return _qmul(_qmul(_qaxis((0, 1, 0), yaw), _qaxis((1, 0, 0), pitch)), _qaxis((0, 0, 1), roll))


def gesture_pose(screen, facing, head=(0.0, 1.65, 0.0), head_pitch=-30.0, distance=0.35):
    """The left hand's pose (x, y, z, yaw, pitch, roll) that puts the Pip-Boy's screen in front of a head
    looking down by head_pitch, facing it, with a natural look at a watch (fingers to the right, level)."""
    def controller(f, u, r):  # forward = -Z, up = +Y, right = +X
        return (r, u, -f)

    offset = controller(*screen)
    normal = controller(*facing)
    look = _rot(_pose_q(0, head_pitch, 0), (0, 0, -1))
    target = tuple(head[i] + look[i] * distance for i in range(3))
    want = tuple(-look[i] for i in range(3))
    natural = (0.85, 0.0, -0.53)
    best = None
    for yaw in range(-180, 180, 5):
        for pitch in range(-90, 91, 5):
            for roll in range(-180, 180, 5):
                q = _pose_q(yaw, pitch, roll)
                n = _rot(q, normal)
                face = sum(n[i] * want[i] for i in range(3))
                if face < 0.97:
                    continue
                fingers = _rot(q, (0, 0, -1))
                score = face + sum(fingers[i] * natural[i] for i in range(3))
                if best is None or score > best[0]:
                    best = (score, yaw, pitch, roll, q)
    if best is None:
        return None
    _, yaw, pitch, roll, q = best
    o = _rot(q, offset)
    hand = tuple(target[i] - o[i] for i in range(3))
    return hand + (yaw, pitch, roll)


def open_pipboy_by_gesture(session, head_pitch=-30.0, distance=0.35, head=(0.0, 1.65, 0.0)):
    """Opens the Pip-Boy by looking at the left wrist: sets the head and the left hand through the VR
    channel. The head and hand stay set; release them with session.vr_do("release all")."""
    session.require(vr=True)
    info = pipboy(session)
    if not info["screen"] or not info["facing"]:
        raise ChannelError("the Pip-Boy's screen is not placed yet (no world loaded?)")
    pose = gesture_pose(info["screen"], info["facing"], head, head_pitch, distance)
    if pose is None:
        raise ChannelError("no hand pose faces the Pip-Boy's screen to the head")
    session.vr_do(f"head {head[0]} {head[1]} {head[2]} 0 {head_pitch}")
    session.vr_do("hand left {:.4f} {:.4f} {:.4f} {} {} {}".format(*pose))
    return wait_pipboy(session, "open")
