# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

"""The perf FPS verdict has to be read off the steady-state stat dumps only.

MTL prints one stat line per session every 10 s, so a dump is a census of the
sessions alive at that moment and the FPS in it is the average over the period
since the previous dump. Session create and free happen *inside* a period, and
teardown adds a per-second dump per freed session, so the dumps at either end of
a run report a fraction of the target rate even at full speed.

Run 35233378303 ended its measurement window a fixed cooldown before the last
MTL-stamped log line -- which lands 11-40 s deep in teardown -- and so reported
`test_tx[multi_core]` as 2/32 for a run in which all 32 sessions held 59.9-60.0
fps for twelve consecutive dumps at 83.4 Gb/s. Every TX row of that performance
report was understated, one of them by 2x.

The fixtures are the per-dump FPS matrices of seven probes of that run, one row
per dump, each collapsed to its census and the rate the named sessions shared
(within that run they agree to 0.2 fps, and the collapse was checked not to move
any verdict). Long runs of identical single-session teardown dumps are trimmed;
the near-full teardown dumps that broke the old window are kept verbatim. Three
of the seven must pass and four must keep failing -- they are real ceilings.
"""

import sys
import unittest
from pathlib import Path

sys.path.insert(
    0, str(Path(__file__).resolve().parents[2] / "tests/acceptance/mtl_engine")
)

import performance_monitoring  # noqa: E402

# What tests/dual/performance/test_vf_perf_dualhost.py passes for the 59fps id.
FPS = 59
MAX_DROP_PCT = 0.10

ALL = None  # census naming every session


def render(rows, num_sessions, direction):
    """Expand (timestamp, census, fps) rows into RxTxApp stat lines.

    The scheduler index in the line varies with the session so that the metric
    is pinned to the session index, which is what the report counts.
    """
    kind = direction.upper()
    lines = []
    for timestamp, census, fps in rows:
        for sid in range(num_sessions) if census is ALL else census:
            lines.append(
                f"MTL: 2026-09-17 {timestamp}, "
                f"{kind}_VIDEO_SESSION({sid // 16},{sid}:app_{direction}_st20p_{sid}): "
                f"fps {fps:.6f} frames 600 pkts 0:0 inflight 0:0"
            )
    return lines


def judge(rows, num_sessions, direction):
    """Run the production monitor over rendered rows → (ok, count, details)."""
    monitor = (
        performance_monitoring.monitor_tx_fps
        if direction == "tx"
        else performance_monitoring.monitor_rx_fps
    )
    return monitor(
        render(rows, num_sessions, direction),
        FPS,
        num_sessions,
        max_drop_pct=MAX_DROP_PCT,
    )


# ── Fixtures: probes of run 35233378303 ─────────────────────────────────

# test_tx[single_core] at 17 sessions. Reported failing; ran at 59.9 throughout.
TX_SC_17 = [
    ("14:45:51", [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10], 0.0),
    ("14:46:01", ALL, 49.1),
    ("14:46:11", ALL, 59.9),
    ("14:46:21", ALL, 60.0),
    ("14:46:31", ALL, 59.9),
    ("14:46:41", ALL, 59.9),
    ("14:46:51", ALL, 60.0),
    ("14:47:01", ALL, 59.9),
    ("14:47:11", ALL, 60.0),
    ("14:47:21", ALL, 59.9),
    ("14:47:31", ALL, 60.0),
    ("14:47:41", ALL, 59.9),
    ("14:47:51", ALL, 59.9),
    ("14:47:55", [0], 45.7),
    ("14:47:56", [1], 36.8),
    ("14:48:01", [6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16], 17.3),
    ("14:48:02", [7], 0.0),
    ("14:48:11", [16], 0.0),
]

# test_tx[multi_core] at 32 sessions, the 2/32 headline. 83.4 Gb/s, all full rate.
TX_MC_32 = [
    ("14:50:29", [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10], 0.0),
    ("14:50:39", ALL, 28.1),
    ("14:50:49", ALL, 60.0),
    ("14:50:59", ALL, 59.9),
    ("14:51:09", ALL, 60.0),
    ("14:51:19", ALL, 59.9),
    ("14:51:29", ALL, 59.9),
    ("14:51:39", ALL, 60.0),
    ("14:51:49", ALL, 59.9),
    ("14:51:59", ALL, 60.0),
    ("14:52:09", ALL, 59.9),
    ("14:52:19", ALL, 59.9),
    ("14:52:29", ALL, 60.0),
    ("14:52:37", [0], 53.0),
    ("14:52:38", [1], 47.6),
    ("14:52:39", list(range(2, 32)), 44.5),
    ("14:52:40", [3], 0.0),
    ("14:52:49", list(range(12, 32)), 0.0),
    ("14:52:59", list(range(22, 32)), 0.0),
    ("14:53:08", [31], 0.0),
]

# test_tx[single_core] at 32 sessions: a real single-core TX ceiling, ~55 fps
# uniformly for eleven dumps. Must stay failing.
TX_SC_32 = [
    ("14:28:03", [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10], 0.0),
    ("14:28:13", ALL, 25.9),
    ("14:28:23", ALL, 55.6),
    ("14:28:33", ALL, 55.2),
    ("14:28:43", ALL, 55.0),
    ("14:28:53", ALL, 55.1),
    ("14:29:03", ALL, 55.1),
    ("14:29:13", ALL, 55.0),
    ("14:29:23", ALL, 55.1),
    ("14:29:33", ALL, 55.0),
    ("14:29:43", ALL, 55.0),
    ("14:29:53", ALL, 55.1),
    ("14:30:03", ALL, 55.0),
    ("14:30:12", [0], 48.9),
    ("14:30:13", list(range(1, 32)), 42.8),
    ("14:30:23", list(range(11, 32)), 0.0),
    ("14:30:43", [31], 0.0),
]

# test_rx[single_core] at 22 sessions: a real receive ceiling, ~17.5 fps. Note
# the dumps naming only 19-21 of the 22 sessions in mid-run -- stat lines lost
# in transport, not sessions leaving, so the window has to span them.
RX_SC_22 = [
    ("15:28:38", ALL, 0.0),
    ("15:28:48", ALL, 13.9),
    ("15:28:58", [0, 1] + list(range(3, 22)), 17.5),
    ("15:29:08", ALL, 17.4),
    ("15:29:18", list(range(17)) + [18, 20], 17.0),
    ("15:29:28", list(range(17)) + [18, 19, 20, 21], 17.1),
    ("15:29:38", list(range(15)) + [16, 17, 18, 19, 20, 21], 17.7),
    ("15:29:48", ALL, 17.2),
    ("15:29:58", list(range(20)) + [21], 17.7),
    ("15:30:08", [0] + list(range(2, 22)), 17.5),
    ("15:30:18", ALL, 17.7),
    ("15:30:28", ALL, 17.8),
    ("15:30:31", ALL, 17.1),
]

# test_rx[single_core-dma] at 36 sessions: 93.8 Gb/s asked of the wire, and the
# run collapses to ~0 fps two thirds of the way in and never recovers. The
# collapse is the answer, so the window must not end before it.
RX_SC_DMA_36 = [
    ("15:45:21", ALL, 0.0),
    ("15:45:31", ALL, 0.0),
    ("15:45:41", ALL, 14.4),
    ("15:45:51", ALL, 59.9),
    ("15:46:01", ALL, 59.9),
    ("15:46:11", ALL, 59.6),
    ("15:46:21", ALL, 59.9),
    ("15:46:31", ALL, 2.9),
    ("15:46:41", ALL, 0.2),
    ("15:46:51", ALL, 0.1),
    ("15:47:01", ALL, 0.1),
    ("15:47:11", ALL, 0.1),
    ("15:47:13", ALL, 0.1),
]

# test_rx[multi_core] at 34 sessions: two ~20 s stalls in which every session
# loses packets together. Must keep failing.
RX_MC_34 = [
    ("16:08:16", ALL, 0.0),
    ("16:08:26", ALL, 27.6),
    ("16:08:36", ALL, 5.1),
    ("16:08:46", ALL, 0.0),
    ("16:08:56", ALL, 56.0),
    ("16:09:06", ALL, 59.9),
    ("16:09:16", ALL, 60.0),
    ("16:09:26", ALL, 39.0),
    ("16:09:36", ALL, 0.0),
    ("16:09:46", ALL, 21.7),
    ("16:09:56", ALL, 59.9),
    ("16:10:06", ALL, 60.0),
    ("16:10:09", ALL, 57.8),
]

# test_rx[multi_core] at 32 sessions: the one clean multi_core receive probe of
# the run, 83.3 Gb/s. Passed before and must still pass.
RX_MC_32 = [
    ("15:55:31", ALL, 0.0),
    ("15:55:41", ALL, 35.1),
    ("15:55:51", ALL, 59.9),
    ("15:56:01", ALL, 60.0),
    ("15:56:11", ALL, 59.9),
    ("15:56:21", ALL, 59.9),
    ("15:56:31", ALL, 60.0),
    ("15:56:41", ALL, 59.9),
    ("15:56:51", ALL, 60.0),
    ("15:57:01", ALL, 59.9),
    ("15:57:11", ALL, 59.9),
    ("15:57:21", ALL, 60.0),
    ("15:57:23", ALL, 58.4),
]


class SteadyWindowVerdictTests(unittest.TestCase):
    """Each probe of run 35233378303 has to get the verdict its data supports."""

    def test_tx_single_core_17_sessions_passes(self):
        ok, count, _ = judge(TX_SC_17, 17, "tx")
        self.assertEqual((ok, count), (True, 17))

    def test_tx_multi_core_32_sessions_passes(self):
        # The report said 2/32. Twelve dumps of 32 sessions at 59.9-60.0 say 32.
        ok, count, _ = judge(TX_MC_32, 32, "tx")
        self.assertEqual((ok, count), (True, 32))

    def test_rx_multi_core_32_sessions_passes(self):
        ok, count, _ = judge(RX_MC_32, 32, "rx")
        self.assertEqual((ok, count), (True, 32))

    def test_tx_single_core_32_sessions_fails_on_a_real_ceiling(self):
        # 55 fps is 92% of the target and it is what the host can pace.
        ok, count, _ = judge(TX_SC_32, 32, "tx")
        self.assertEqual((ok, count), (False, 0))

    def test_rx_single_core_22_sessions_fails_across_lost_stat_lines(self):
        ok, count, details = judge(RX_SC_22, 22, "rx")
        self.assertEqual((ok, count), (False, 0))
        # The short-census dumps must be inside the window, not a boundary: a
        # window that stopped at the first of them would hold too few samples
        # to judge and the failure would be reported for the wrong reason.
        self.assertEqual(details["window_reject_reason"], "")
        self.assertGreaterEqual(len(details["window"]), 8)
        # And no session may be judged on a single retained dump.
        for sid, history in details["session_fps_history"].items():
            self.assertGreaterEqual(len(history), 6, f"session {sid}")

    def test_rx_single_core_dma_36_sessions_fails_on_a_late_collapse(self):
        # The last five dumps are ~0 fps. Trimming may drop one of them; the
        # verdict must not survive the rest.
        ok, count, _ = judge(RX_SC_DMA_36, 36, "rx")
        self.assertEqual((ok, count), (False, 0))

    def test_rx_multi_core_34_sessions_fails_on_mid_run_stalls(self):
        ok, count, _ = judge(RX_MC_34, 34, "rx")
        self.assertEqual((ok, count), (False, 0))


class WindowBoundaryTests(unittest.TestCase):
    """What the window may and may not be moved by."""

    def test_a_later_unrelated_mtl_line_cannot_move_the_end(self):
        # This is the defect: the end of the window was derived from the last
        # MTL-stamped line of the log, and MTL keeps logging through teardown.
        lines = render(TX_MC_32, 32, "tx")
        lines.append(
            "MTL: 2026-09-17 14:53:08, mtl_sch_unregister_tasklet(0), "
            "tasklet tx_video_sessions_mgr(0) unregistered"
        )
        ok, count, _ = performance_monitoring.monitor_tx_fps(
            lines, FPS, 32, max_drop_pct=MAX_DROP_PCT
        )
        self.assertEqual((ok, count), (True, 32))

    def test_teardown_dumps_are_outside_the_window(self):
        _, _, details = judge(TX_MC_32, 32, "tx")
        # Teardown begins at 14:52:37, the first dump after the last full census.
        self.assertTrue(all(ts < "2026-09-17 14:52:37" for ts in details["window"]))

    def test_the_ramp_dump_is_outside_the_window(self):
        # 14:50:39 names all 32 sessions but covers the last one's create.
        _, _, details = judge(TX_MC_32, 32, "tx")
        self.assertNotIn("2026-09-17 14:50:39", details["window"])

    def test_the_first_dump_with_traffic_is_outside_the_window(self):
        # An RX session reports 0 fps until the sender on the other host starts,
        # so the dump in which arrival begins covers only part of its period.
        _, _, details = judge(RX_MC_32, 32, "rx")
        self.assertNotIn("2026-09-17 15:55:41", details["window"])
        self.assertIn("2026-09-17 15:55:51", details["window"])


class RefusesToJudgeTests(unittest.TestCase):
    """Cases with no honest window must be reported as such, not scored."""

    def test_a_run_too_short_to_sample_is_refused(self):
        rows = [
            ("15:00:01", ALL, 30.0),
            ("15:00:11", ALL, 59.9),
            ("15:00:21", ALL, 59.9),
            ("15:00:31", ALL, 59.9),
            ("15:00:34", ALL, 20.0),
        ]
        ok, count, details = judge(rows, 4, "rx")
        self.assertEqual((ok, count), (False, 0))
        self.assertEqual(details["window"], [])
        self.assertIn("only 2 dump(s)", details["window_reject_reason"])
        # The reason has to name the requirement: the lever is test_time, and a
        # bare sample count does not tell the reader which way to move it.
        self.assertIn(
            str(performance_monitoring.FPS_MIN_STEADY_SAMPLES),
            details["window_reject_reason"],
        )

    def test_a_run_with_no_interior_between_its_census_dumps_is_refused(self):
        # Both bounds are discarded as partial, so two *adjacent* full censuses
        # leave nothing between them. A ~30 s run looks like this.
        rows = [
            ("15:00:01", [0, 1, 2], 59.9),
            ("15:00:11", ALL, 59.9),
            ("15:00:21", ALL, 59.9),
            ("15:00:31", [0, 1, 2], 59.9),
        ]
        ok, count, details = judge(rows, 4, "rx")
        self.assertEqual((ok, count), (False, 0))
        self.assertIn("naming all 4 sessions", details["window_reject_reason"])

    def test_a_run_that_never_carried_traffic_is_refused(self):
        rows = [(f"15:0{n}:00", ALL, 0.0) for n in range(8)]
        ok, count, details = judge(rows, 4, "rx")
        self.assertEqual((ok, count), (False, 0))
        self.assertEqual(
            details["window_reject_reason"], "no dump had every session live"
        )

    def test_a_session_that_never_started_is_refused(self):
        # Session 3 is configured but absent from every dump, so no dump is a
        # complete census and there is no window to score the other three on.
        rows = [(f"15:0{n}:00", [0, 1, 2], 59.9) for n in range(8)]
        ok, count, details = judge(rows, 4, "rx")
        self.assertEqual((ok, count), (False, 0))
        self.assertIn("naming all 4 sessions", details["window_reject_reason"])

    def test_no_log_at_all_is_refused(self):
        ok, count, details = performance_monitoring.monitor_rx_fps([], FPS, 4)
        self.assertEqual((ok, count), (False, 0))
        self.assertNotEqual(details["window_reject_reason"], "")


class NotAnAggregateTests(unittest.TestCase):
    """The verdict is per session, so one lagging session fails the case."""

    def test_one_slow_session_among_many_fails_the_case(self):
        rows = [("15:00:00", ALL, 0.0)] + [
            (f"15:0{n}:00", ALL, 59.9) for n in range(1, 9)
        ]
        lines = render(rows, 8, "rx")
        # Re-render session 7 at half rate for every dump it appears in.
        lines = [ln for ln in lines if "st20p_7)" not in ln]
        lines += render([(ts, [7], 30.0) for ts, _, _ in rows], 8, "rx")
        ok, count, _ = performance_monitoring.monitor_rx_fps(
            lines, FPS, 8, max_drop_pct=MAX_DROP_PCT
        )
        self.assertEqual((ok, count), (False, 7))

    def test_a_single_bad_dump_is_forgiven_but_two_are_not(self):
        # MAX_DROP_PCT trims the worst 10% of a session's samples, which is one
        # dump out of ten. A second one has to sink the case.
        good = [(f"15:{n:02d}:00", ALL, 59.9) for n in range(13)]
        one_bad = list(good)
        one_bad[6] = ("15:06:00", ALL, 0.0)
        ok, count, details = judge(one_bad, 4, "rx")
        self.assertEqual(len(details["window"]), 10)
        self.assertEqual((ok, count), (True, 4))

        two_bad = list(one_bad)
        two_bad[7] = ("15:07:00", ALL, 0.0)
        ok, count, _ = judge(two_bad, 4, "rx")
        self.assertEqual((ok, count), (False, 0))


if __name__ == "__main__":
    unittest.main()
