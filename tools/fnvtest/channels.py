"""Line channels to the running game: the game side (FNVVR-Test-Bridge) and the VR side (FNVVR's
d3d9.vrTestChannel). Both speak the same framing: one request line in, answer lines back, console text
as "| ..." lines, the last line "ok ..." or "err ...".

FNVVR-Test-Bridge. Copyright (c) 2026 joschi127. zlib licence, see LICENSE.
"""
import re
import time

GAME_PIPE = "FNVVR-Test-Bridge"
VR_PIPE = "FNVVR-Test-VR"


class ChannelError(Exception):
    """The channel is not reachable or the connection broke."""


class Answer:
    """One answer: the console text lines, the final line and whether it was "ok"."""

    def __init__(self, console, final):
        self.console = console
        self.final = final
        self.ok = final == "ok" or final.startswith("ok ")
        self.detail = final[3:] if self.ok else final[4:] if final.startswith("err ") else final

    def fields(self):
        """key=value pairs of the final line (for "state", "wait" ...)."""
        return dict(re.findall(r"(\w+)=(\S+)", self.detail))

    def __repr__(self):
        return f"Answer({self.final!r}, console={len(self.console)} lines)"


class Channel:
    def __init__(self, pipe, label, connect_timeout=3.0):
        self.label = label
        path = r"\\.\pipe" + "\\" + pipe
        end = time.monotonic() + connect_timeout
        while True:
            try:
                self._f = open(path, "r+b", buffering=0)
                break
            except OSError as e:
                if time.monotonic() >= end:
                    raise ChannelError(f"{label} channel not reachable at {path}: {e}") from e
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
                raise ChannelError(f"{self.label} channel: connection lost")
            self._buf += chunk
        line, self._buf = self._buf.split(b"\n", 1)
        return line.decode("cp1252", "replace")

    def ask(self, request):
        self._f.write(request.encode("cp1252", "replace") + b"\n")
        console = []
        while True:
            line = self._line()
            if line.startswith("| "):
                console.append(line[2:])
            else:
                return Answer(console, line)

    def frame(self):
        answer = self.ask("state")
        if not answer.ok or "frame" not in answer.fields():
            raise ChannelError(f"{self.label} channel: no frame in state answer: {answer.final}")
        return int(answer.fields()["frame"])


class GameChannel(Channel):
    """Console lines and eval expressions through FNVVR-Test-Bridge (needs xNVSE, not the co-op mod)."""

    def __init__(self, pipe=GAME_PIPE, connect_timeout=3.0):
        super().__init__(pipe, "game", connect_timeout)


class VrChannel(Channel):
    """Head/hand poses, actions, aiming and captures through FNVVR's d3d9.vrTestChannel."""

    def __init__(self, pipe=VR_PIPE, connect_timeout=3.0):
        super().__init__(pipe, "vr", connect_timeout)
