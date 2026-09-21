# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

"""NetsniffRecorder.capture() has to put the capture port back when it returns.

netsniff-ng is sized to one frame of packets, so it exits a fraction of a
second into a test that then runs for another minute or more. Whatever capture()
leaves behind, the port carries for that whole remainder.
"""

import sys
import types
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import Mock, patch

sys.path.append(str(Path(__file__).resolve().parents[1] / "acceptance"))
# Only the acceptance venv installs mfd_connect. Stand it in, since none of it
# is reached by the code under test, and import the real module: a stub of the
# module itself would not notice a rename or a changed signature. All four names
# have to be real exception classes -- netsniff catches every one of them.
for _name in ("mfd_connect", "mfd_connect.exceptions"):
    sys.modules.setdefault(_name, types.ModuleType(_name))
for _exc in (
    "ConnectionCalledProcessError",
    "RemoteProcessInvalidState",
    "RemoteProcessTimeoutExpired",
    "SSHRemoteProcessEndException",
):
    if not hasattr(sys.modules["mfd_connect.exceptions"], _exc):
        setattr(sys.modules["mfd_connect.exceptions"], _exc, type(_exc, (OSError,), {}))

from create_pcap_file import netsniff  # noqa: E402


def _result(stdout="", return_code=0):
    return SimpleNamespace(return_code=return_code, stdout=stdout, stderr="")


class CaptureReleaseTests(unittest.TestCase):
    def setUp(self):
        # _reap() waits between SIGTERM and SIGKILL; nothing here is real.
        patcher = patch.object(netsniff, "sleep")
        self.sleep = patcher.start()
        self.addCleanup(patcher.stop)

        self.commands = []

        def execute_command(cmd, **_kwargs):
            self.commands.append(cmd)
            if cmd.startswith("ip -o link show"):
                # UP but not PROMISC, so the recorder turns promisc on itself
                # and owns restoring it.
                return _result("2: ice1: <BROADCAST,MULTICAST,UP,LOWER_UP>")
            return _result("host")

        self.proc = Mock(running=True, pid=4242, stdout_text="")
        # netsniff-ng exits of its own accord once it has --num packets.
        self.proc.wait.side_effect = lambda **_kw: setattr(self.proc, "running", False)

        connection = Mock()
        connection.execute_command.side_effect = execute_command
        connection.start_process.return_value = self.proc
        self.host = SimpleNamespace(name="sut", connection=connection)

    def _recorder(self, **kwargs):
        return netsniff.NetsniffRecorder(
            self.host,
            test_name="rx_timing",
            pcap_dir="/tmp",
            interface="ice1",
            **kwargs,
        )

    def _promisc(self, state):
        return [c for c in self.commands if f"promisc {state}" in c]

    def _pkills(self):
        return [c for c in self.commands if "pkill" in c]

    def test_packet_count_capture_restores_promisc(self):
        recorder = self._recorder(packets_capture=10)
        recorder.capture(capture_time=90)

        self.proc.wait.assert_called_once()
        self.assertEqual(len(self._promisc("on")), 1)
        self.assertEqual(
            len(self._promisc("off")),
            1,
            "capture() returned with the port still promiscuous; it stays that "
            "way until fixture teardown, i.e. for the rest of the test",
        )

    def test_capture_interrupted_mid_wait_restores_promisc(self):
        # Only the first sleep is the capture's own; _reap()'s SIGTERM grace
        # still has to run inside the release path.
        self.sleep.side_effect = [KeyboardInterrupt, None, None]
        recorder = self._recorder(capture_time=1)

        with self.assertRaises(KeyboardInterrupt):
            recorder.capture()

        self.assertEqual(
            len(self._promisc("off")),
            1,
            "an interrupted timed capture left the port promiscuous",
        )

    def test_second_stop_is_a_no_op(self):
        # The fixture calls stop() at teardown as well. _reap() ends in a
        # host-global pkill, so that second call must not reach it -- by then
        # the name can belong to a following test's capture.
        recorder = self._recorder(packets_capture=10)
        recorder.capture(capture_time=90)
        pkills_after_capture = len(self._pkills())
        recorder.stop()

        self.assertEqual(len(self._pkills()), pkills_after_capture)
        self.assertEqual(len(self._promisc("off")), 1)


if __name__ == "__main__":
    unittest.main()
