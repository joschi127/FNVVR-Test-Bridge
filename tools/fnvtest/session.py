"""A test session over whichever channels are there.

Each channel is optional: a test says what it needs (Session(game=True, vr=False) ...), and a missing
required channel fails with a clear message. Nothing here needs the co-op mod.

Frame numbers: the game channel counts game frames, the VR channel VR frames, from different start
moments. sync() measures the offset (to about one frame) so both can be addressed with one number,
the VR channel's when it is there, else the game channel's. The offset is measured again on every sync()
because loading screens can make the counters drift apart.

FNVVR-Test-Bridge. Copyright (c) 2026 joschi127. zlib licence, see LICENSE.
"""
from .channels import ChannelError, GameChannel, VrChannel, GAME_PIPE, VR_PIPE


class Session:
    def __init__(self, game="auto", vr="auto", game_pipe=GAME_PIPE, vr_pipe=VR_PIPE, connect_timeout=3.0):
        """game / vr: True (required), False (not used) or "auto" (used if reachable)."""
        self.game = self._open(GameChannel, game, game_pipe, connect_timeout)
        self.vr = self._open(VrChannel, vr, vr_pipe, connect_timeout)
        self.offset = None  # vr frame - game frame

    @staticmethod
    def _open(cls, wanted, pipe, timeout):
        if wanted is False:
            return None
        try:
            return cls(pipe, timeout)
        except ChannelError:
            if wanted is True:
                raise
            return None

    def close(self):
        for channel in (self.game, self.vr):
            if channel:
                channel.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def require(self, game=False, vr=False):
        missing = [name for name, need, ch in (("game", game, self.game), ("vr", vr, self.vr)) if need and not ch]
        if missing:
            raise ChannelError("this test needs the " + " and ".join(missing) + " channel(s): "
                               "game = FNVVR-Test-Bridge with Enabled=1, vr = d3d9.vrTestChannel = True")

    # Game side
    def run(self, line):
        self.require(game=True)
        return self.game.ask(line)

    def eval(self, expression):
        self.require(game=True)
        return self.game.ask("eval " + expression)

    # VR side
    def vr_do(self, request):
        self.require(vr=True)
        return self.vr.ask(request)

    # Frames
    def sync(self):
        """Measures the offset between the two frame counters; returns it (None with one channel)."""
        if not (self.game and self.vr):
            self.offset = None
            return None
        before = self.game.frame()
        vr = self.vr.frame()
        after = self.game.frame()
        self.offset = vr - round((before + after) / 2)
        return self.offset

    def frame(self):
        """The current frame in the common numbering."""
        if self.vr:
            return self.vr.frame()
        self.require(game=True)
        return self.game.frame()

    def wait_frames(self, frames):
        channel = self.vr or self.game
        self.require(game=not self.vr)
        return channel.ask(f"wait {int(frames)}")

    def wait_until(self, frame):
        """Waits for a frame in the common numbering (VR frames when the VR channel is there)."""
        if self.vr:
            return self.vr.ask(f"waitframe {int(frame)}")
        self.require(game=True)
        return self.game.ask(f"waitframe {int(frame)}")

    def game_frame(self, common_frame):
        """The game channel's number for a common frame number (after sync())."""
        if self.vr is None:
            return common_frame
        if self.offset is None:
            self.sync()
        return common_frame - self.offset
