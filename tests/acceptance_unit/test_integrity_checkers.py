# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

import logging
import random
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

INTEGRITY = Path(__file__).resolve().parents[2] / "tests/acceptance/common/integrity"
CHECKER = INTEGRITY / "audio_integrity.py"
sys.path.insert(0, str(INTEGRITY))

import audio_integrity as ai  # noqa: E402

SAMPLE_SIZE, SAMPLE_NUM, CHANNELS = 2, 4, 2
FRAME = SAMPLE_SIZE * SAMPLE_NUM * CHANNELS
GROUP = SAMPLE_SIZE * CHANNELS
_rand = random.Random(0)
F = [bytes(_rand.randrange(256) for _ in range(FRAME)) for _ in range(24)]
SRC = b"".join(F[:20]) + bytes(5)  # ends mid-group, as a real source file may
FOREIGN = b"".join(F[20:]) * 20
FRAME_LOOP, GROUP_LOOP = SRC[: 20 * FRAME], SRC[: len(SRC) // GROUP * GROUP]


def window(loop, skip_bytes, frames):
    """A capture of `frames` frames that joined `skip_bytes` into `loop`."""
    return (loop[skip_bytes:] + loop * 3)[: frames * FRAME]


def frames(*indexes):
    return b"".join(F[i] for i in indexes)


class AudioLoopWindowTests(unittest.TestCase):
    """A correct capture is a window onto the looped source starting wherever the
    RX joined. Every case below is a shape a real st30p run produces."""

    def check(self, out, src=SRC, min_frames=0, stream=False):
        """Check one capture, or a list of them as consecutive stream segments."""
        records = []
        handler = logging.Handler()
        handler.emit = records.append
        logger = logging.getLogger(self.id())
        logger.handlers, logger.propagate = [handler], False
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, "src.pcm").write_bytes(src)
            for index, part in enumerate(out if stream else [out]):
                name = f"out.pcm.{index:03d}" if stream else "out.pcm"
                Path(tmp, name).write_bytes(part)
            checker = (ai.AudioStreamIntegritor if stream else ai.AudioIntegritor)(
                logger,
                f"{tmp}/src.pcm",
                "out.pcm",
                SAMPLE_SIZE,
                SAMPLE_NUM,
                CHANNELS,
                tmp,
                False,
                min_frames,
            )
            ok = (
                checker.check_stream_integrity()
                if stream
                else checker.check_integrity_file(f"{tmp}/out.pcm")
            )
        return ok, [r.getMessage() for r in records]

    def test_join_on_a_packet_boundary_passes(self):
        # The RX anchors frame 0 on its first accepted packet, so a healthy capture
        # starts a part-frame in -- FRAME // 4 here is exactly one sample group.
        ok, msgs = self.check(window(FRAME_LOOP, 9 * FRAME + FRAME // 4, 40))
        self.assertTrue(ok, msgs)

    def test_group_loop_period_passes(self):
        # The ffmpeg leg drops the source's trailing partial sample group, so its
        # loop period is the whole-group prefix, not the whole-frame one.
        ok, msgs = self.check(window(GROUP_LOOP, 5 * GROUP, 40))
        self.assertTrue(ok, msgs)

    def test_unrelated_capture_fails(self):
        ok, msgs = self.check(FOREIGN)
        self.assertFalse(ok)
        self.assertTrue(any("No position in" in m for m in msgs), msgs)

    def test_a_capture_that_could_start_anywhere_is_inconclusive(self):
        # Silence matches at every offset, so comparing it anyway would pass a
        # receiver that wrote nothing but zeroed frame buffers.
        silence = bytes(20 * FRAME)
        ok, msgs = self.check(silence[: 10 * FRAME], src=silence)
        self.assertFalse(ok)
        self.assertTrue(any("cannot be told" in m for m in msgs), msgs)

    def test_truncated_capture_fails_only_against_the_floor(self):
        short = window(FRAME_LOOP, 0, 5)
        self.assertTrue(self.check(short)[0], "bit-exact frames, no floor asked")
        self.assertFalse(self.check(short, min_frames=40)[0])

    def test_only_a_small_gap_at_the_join_is_tolerated(self):
        src = frames(*range(20))
        joined = self.check(frames(0, *range(2, 20)), src=src)  # 1 frame missing
        self.assertTrue(joined[0], joined[1])
        self.assertTrue(any("missing 1 frame(s)" in m for m in joined[1]))
        for name, out in (
            ("stale repeat", frames(0, 0, *range(2, 20))),
            ("wider than the allowance", frames(0, *range(4, 20))),
            (
                "past the join window",
                frames(*range(ai.JOIN_FRAMES + 3), *range(ai.JOIN_FRAMES + 4, 20)),
            ),
        ):
            with self.subTest(name):
                rejected, why = self.check(out, src=src)
                self.assertFalse(rejected)
                self.assertTrue(any("frames differ" in m for m in why), why)

    def test_only_the_first_stream_segment_may_hold_a_gap(self):
        # Only the first segment can hold the join. A later one was cut from a
        # stream the RX had already joined, so the same gap there is lost audio.
        src = frames(*range(20))
        joined, later = frames(0, *range(2, 20)), frames(10, *range(12, 20))
        self.assertTrue(self.check([joined], src=src, stream=True)[0])
        ok, msgs = self.check([joined, later], src=src, stream=True)
        self.assertFalse(ok, msgs)
        self.assertTrue(any("out.pcm.001" in m for m in msgs), msgs)

    def test_audio_lost_across_a_segment_boundary_fails(self):
        # The segments are one capture. Checked one by one, the second re-anchors
        # on the source past the hole and the loss reads as correct.
        src = frames(*range(20))
        whole = [frames(*range(10)), frames(*range(10, 20))]
        holed = [frames(*range(10)), frames(*range(11, 20))]  # frame 10 lost
        self.assertTrue(self.check(whole, src=src, stream=True)[0])
        ok, msgs = self.check(holed, src=src, stream=True)
        self.assertFalse(ok, msgs)

    def test_per_frame_report_is_bounded(self):
        # The failure that hung the nightly: 411k report lines past the ~2 MiB SSH
        # channel window, which the caller drains only after the checker exits.
        ok, msgs = self.check(FRAME_LOOP[: 3 * FRAME] + FOREIGN)
        self.assertFalse(ok)
        bad = [m for m in msgs if m.startswith("Bad audio frame")]
        self.assertTrue(bad, "a failing check must still name some frames")
        self.assertLessEqual(len(bad), ai.MAX_BAD_FRAME_REPORTS)
        self.assertTrue(any("Suppressing" in m for m in msgs))

    def test_cli_reports_the_verdict_in_its_exit_code(self):
        # integrity_runner.py reads nothing but the exit code, so a failing check
        # that exits 0 passes the acceptance test silently.
        for name, out, expected in (("good", FRAME_LOOP, 0), ("bad", FOREIGN, 1)):
            with self.subTest(name), tempfile.TemporaryDirectory() as tmp:
                Path(tmp, "src.pcm").write_bytes(SRC)
                Path(tmp, "out.pcm").write_bytes(out)
                flags = (
                    f"--sample_size {SAMPLE_SIZE} --sample_num {SAMPLE_NUM}"
                    f" --channel_num {CHANNELS} --min_frames 5 --no_delete_file"
                ).split()
                argv = [sys.executable, str(CHECKER), "file", f"{tmp}/src.pcm"]
                argv += ["out.pcm", *flags, "--output_path", tmp]
                done = subprocess.run(argv, capture_output=True)
                self.assertEqual(done.returncode, expected, done.stderr.decode())


if __name__ == "__main__":
    unittest.main()
