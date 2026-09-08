# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

import ast
import logging
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import Mock

ROOT = Path(__file__).resolve().parents[2]
CONFTEST = ROOT / "tests/acceptance/conftest.py"


def _load(name, scope):
    """Exec one conftest function into *scope*, without importing conftest.

    conftest pulls in pytest and mfd_connect, neither of which the unit tier
    has, so take the definition straight from the source.
    """
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


if __name__ == "__main__":
    unittest.main()
