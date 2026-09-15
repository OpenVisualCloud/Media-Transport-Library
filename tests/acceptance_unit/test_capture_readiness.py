# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation
"""What has to hold before a compliance capture may be armed.

conftest owns all of it and imports pytest and mfd_connect, neither of which
this tier has, so each function is read out of the source rather than imported.
"""

import ast
import logging
import re
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import Mock

ROOT = Path(__file__).resolve().parents[2]
CONFTEST = ROOT / "tests/acceptance/conftest.py"


def _load(name, scope):
    """Exec one conftest function into *scope*, without importing conftest."""
    node = next(
        n
        for n in ast.parse(CONFTEST.read_text()).body
        if isinstance(n, ast.FunctionDef) and n.name == name
    )
    exec(
        compile(ast.Module(body=[node], type_ignores=[]), str(CONFTEST), "exec"), scope
    )
    return scope[name]


class CaptureReadinessTests(unittest.TestCase):
    def setUp(self):
        self.proc = Mock(running=True)
        self.host = SimpleNamespace(name="capture", connection=Mock())
        self.host.connection.start_process.return_value = self.proc
        self.scope = {
            "logger": logging.getLogger(__name__),
            "time": Mock(),
            "_reap_ptp_daemons": Mock(),
            "_host_tai_utc_offset": Mock(return_value=37),
            "_wait_phc_sync_converged": Mock(return_value=True),
            "_PHC_SYNC_TIMEOUT_SEC": 30,
        }
        _load("_start_capture_phc_sync", self.scope)

    def start(self):
        return self.scope["_start_capture_phc_sync"](self.host, "ice1")

    def test_host_offset_reaches_phc2sys(self):
        self.start()
        self.assertIn("-O 37", self.host.connection.start_process.call_args[0][0])

    def test_zero_offset_still_arms_capture(self):
        # A host reporting 0 puts the sender's CLOCK_TAI and the capture PHC both
        # on UTC, which is self-consistent; refusing to capture, or imposing an
        # offset of our own, would only step the sender's media clock instead.
        self.scope["_host_tai_utc_offset"].return_value = 0
        self.assertIs(self.start(), self.proc)
        self.assertIn("-O 0", self.host.connection.start_process.call_args[0][0])

    def test_process_start_failure_blocks_capture(self):
        self.host.connection.start_process.side_effect = RuntimeError("start failed")
        with self.assertRaisesRegex(RuntimeError, "phc2sys"):
            self.start()
        self.assertEqual(self.scope["_reap_ptp_daemons"].call_count, 2)

    def test_early_exit_blocks_capture_and_reaps_daemon(self):
        self.proc.running = False
        with self.assertRaisesRegex(RuntimeError, "phc2sys"):
            self.start()
        self.assertEqual(self.scope["_reap_ptp_daemons"].call_count, 2)

    def test_nonconvergence_blocks_capture_and_reaps_daemon(self):
        self.scope["_wait_phc_sync_converged"].return_value = False
        with self.assertRaisesRegex(RuntimeError, "hold sync"):
            self.start()
        self.assertEqual(self.scope["_reap_ptp_daemons"].call_count, 2)

    def test_exit_during_convergence_blocks_capture(self):
        def converge(*args):
            self.proc.running = False
            return True

        self.scope["_wait_phc_sync_converged"].side_effect = converge
        with self.assertRaisesRegex(RuntimeError, "phc2sys"):
            self.start()
        self.assertEqual(self.scope["_reap_ptp_daemons"].call_count, 2)


class HostOffsetReadTests(unittest.TestCase):
    def setUp(self):
        self.read = _load("_host_tai_utc_offset", {})
        self.host = SimpleNamespace(name="capture", connection=Mock())

    def reads(self, stdout):
        self.host.connection.execute_command.return_value = SimpleNamespace(
            stdout=stdout
        )
        return self.read(self.host)

    def test_the_reported_offset_is_returned_verbatim(self):
        self.assertEqual(self.reads("37\n"), 37)
        self.assertEqual(self.reads("0\n"), 0)

    def test_an_unreadable_offset_names_the_host(self):
        for stdout in ("", "  \n", None):
            with self.assertRaisesRegex(RuntimeError, "capture"):
                self.reads(stdout)


class _Clock:
    """``time`` with no wall clock: only the polling loop's sleeps move it."""

    def __init__(self):
        self.now = 0.0
        self.polls = 0

    def monotonic(self):
        return self.now

    def sleep(self, seconds):
        self.now += seconds
        self.polls += 1


def _phc2sys_line(offset, servo_state):
    """A phc2sys ``-m`` line as it is really printed, verbatim from the run."""
    return (
        f"phc2sys[1165377.102]: ice1 sys offset {offset} "
        f"s{servo_state} freq -7560 delay 0\n"
    )


class PhcSyncGateTests(unittest.TestCase):
    """Only ``s2`` is a locked servo; the offset printed beside ``s0`` is luck.

    Every capture in the st20p E835 nightly of 2026-09-14 was armed off an
    ``s0`` line, so its PHC was still free-running at its own crystal error --
    and ST 2110-21 VRX/Cinst are computed from packet timestamps.
    """

    def setUp(self):
        self.clock = _Clock()
        self.converged = _load(
            "_wait_phc_sync_converged",
            {
                "re": re,
                "time": self.clock,
                "logger": logging.getLogger(__name__),
                "_PHC_SYNC_THRESHOLD_NS": 2000,
                "_PHC_SYNC_TIMEOUT_SEC": 30,
            },
        )

    def poll(self, *lines):
        """Converge against a host whose log tail yields *lines*, then repeats."""
        lines = list(lines)

        def tail(*args, **kwargs):
            return SimpleNamespace(stdout=lines[0] if len(lines) == 1 else lines.pop(0))

        host = SimpleNamespace(connection=SimpleNamespace(execute_command=tail))
        return self.converged(host, "/tmp/phc.log")

    def test_an_unlocked_servo_is_not_sync(self):
        # The exact line that armed the nightly's captures: in tolerance, s0.
        self.assertFalse(self.poll(_phc2sys_line(71, 0)))

    def test_a_locked_servo_in_tolerance_is_sync(self):
        self.assertTrue(self.poll(_phc2sys_line(83, 2)))

    def test_it_waits_out_the_unlocked_lines_and_takes_the_later_lock(self):
        # Locking takes a few seconds and waiting for it is the whole job:
        # anything that gives up on the s0 lines fails every capture instead.
        lines = [_phc2sys_line(o, s) for o, s in ((71, 0), (-88, 0), (-12, 1), (19, 2))]
        self.assertTrue(self.poll(*lines))
        self.assertEqual(self.clock.polls, len(lines))


if __name__ == "__main__":
    unittest.main()
