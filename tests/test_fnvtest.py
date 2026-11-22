"""Game-free tests of the Python test tools: python -I tests/test_fnvtest.py"""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))

from fnvtest import Answer, ChannelError, Session  # noqa: E402


class AnswerTest(unittest.TestCase):
    def test_ok_and_fields(self):
        a = Answer(["9mm Pistol added"], "ok frame=12 world=1 cell=0000FAB3")
        self.assertTrue(a.ok)
        self.assertEqual(a.fields()["frame"], "12")
        self.assertEqual(a.console, ["9mm Pistol added"])

    def test_plain_ok_and_err(self):
        self.assertTrue(Answer([], "ok").ok)
        e = Answer([], "err no world loaded")
        self.assertFalse(e.ok)
        self.assertEqual(e.detail, "no world loaded")


class RequestTest(unittest.TestCase):
    def test_newline_in_request_is_refused(self):
        from fnvtest.channels import Channel
        channel = Channel.__new__(Channel)
        channel.label = "test"
        with self.assertRaises(ValueError):
            channel.ask("ping\nstate")


class SessionTest(unittest.TestCase):
    def test_optional_channels_missing(self):
        s = Session(game="auto", vr="auto", game_pipe="FNVVR-Test-None-1", vr_pipe="FNVVR-Test-None-2",
                    connect_timeout=0.2)
        self.assertIsNone(s.game)
        self.assertIsNone(s.vr)
        with self.assertRaises(ChannelError):
            s.run("ping")
        with self.assertRaises(ChannelError):
            s.vr_do("ping")
        self.assertIsNone(s.sync())

    def test_required_channel_missing(self):
        with self.assertRaises(ChannelError):
            Session(game=True, vr=False, game_pipe="FNVVR-Test-None-3", connect_timeout=0.2)


class GestureTest(unittest.TestCase):
    def test_hand_pose_faces_the_screen_to_the_head(self):
        from fnvtest import headset
        # The screen as FNVVR reported it in a run: forward, up, right of the left controller, and its facing
        screen, facing = (-0.221, -0.038, -0.080), (0.038, -0.241, -0.970)
        pose = headset.gesture_pose(screen, facing, head=(0.0, 1.65, 0.0), head_pitch=-30.0, distance=0.35)
        self.assertIsNotNone(pose)
        x, y, z, yaw, pitch, roll = pose
        # In front of and below the head, within an arm's reach
        self.assertLess(z, 0.0)
        self.assertLess(y, 1.65)
        self.assertLess((x * x + (y - 1.65) ** 2 + z * z) ** 0.5, 0.8)


if __name__ == "__main__":
    unittest.main()
