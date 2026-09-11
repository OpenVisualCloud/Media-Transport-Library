# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

import sys
import types
import unittest
from pathlib import Path
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/acceptance"))

# The only dependency of these modules outside the stdlib is log_fail, and its
# module pulls in pytest, which the unit tier does not have. Stand it in so the
# modules under test import as themselves, recording the failures they report.
_reported = []
_stub = types.ModuleType("mtl_engine.execute")
_stub.log_fail = _reported.append
sys.modules["mtl_engine.execute"] = _stub

from mtl_engine import integrity, integrity_session  # noqa: E402

# Both hold their own reference to log_fail now, so drop the stand-in again:
# left in place it would also answer the import in every other test module of
# this tier, which discovery imports into the same interpreter.
sys.modules.pop("mtl_engine.execute", None)


class AudioGeometryTests(unittest.TestCase):
    """What the audio integrity checker is handed. Passing the per-PACKET sample
    count is what made it compare 12-byte chunks of a 960-byte frame."""

    def _runner(self, sampling="48kHz", ptime="0.12", test_time=60):
        host = SimpleNamespace(
            name="rx", connection=SimpleNamespace(path=lambda *parts: "/".join(parts))
        )
        intent = integrity_session.IntegrityIntent(
            host=host,
            test_repo_path="/repo",
            src_url="/mnt/media/src.pcm",
            out_url="/mnt/ramdisk/out.pcm",
            kind="audio",
            audio_format="PCM16",
            audio_channels=["U02"],
            audio_sampling=sampling,
            audio_ptime=ptime,
            test_time=test_time,
        )
        return integrity_session.IntegritySession()._build_runner(intent)

    def test_checker_gets_frame_samples_not_packet_samples(self):
        # 0.12 ms is 6 samples on the wire at 48 kHz, but 480 in a frame.
        self.assertEqual(self._runner().sample_num, 480)
        self.assertEqual(self._runner(sampling="96kHz").sample_num, 960)
        # A frame holds whole packets only: at 4 ms it is 2 packets of 192, not
        # the 480 samples that fit in 10 ms.
        self.assertEqual(self._runner(ptime="4").sample_num, 384)

    def test_checker_gets_a_floor_a_truncated_capture_misses(self):
        # A capture cut short is bit-exact, so only its length gives it away; on
        # the ffmpeg leg nothing else fails. 100 frames make a second at 48 kHz.
        floor = self._runner().min_frames
        self.assertLess(floor, 100 * (60 - 8.5), "the worst healthy shortfall seen")
        self.assertGreater(floor, 100 * 20, "ffmpeg stopped 20 s into a 60 s run")

    def test_unknown_combination_reports_and_returns_zero(self):
        _reported.clear()
        self.assertEqual(integrity.get_frame_sample_number("44kHz", "0.12"), 0)
        self.assertTrue(_reported)


if __name__ == "__main__":
    unittest.main()
