# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation
"""What the FFmpeg adapter transmits for a multisession st20p run.

``tests/single/st20p/test_multisession.py``'s ffmpeg variant builds an RX
command with two mtl_st20p inputs, on udp_port 20000 and 20002, and used to
build a single TX output on 20000. Nothing ever reached the second stream's
port, so RX input 1 could only time out. TX has to stay ONE process with two
outputs: the topology gives it a single VF (``nic_port_list[0]``, RX owns [1])
and a second DPDK process cannot claim it.

The acceptance tree imports packages only its venv installs. Stand them in,
since none is reached by the command builder, and import the real adapter.
"""

import re
import sys
import types
import unittest
from pathlib import Path
from unittest.mock import Mock

sys.path.append(str(Path(__file__).resolve().parents[1] / "acceptance"))
for _name in (
    "mfd_common_libs",
    "mfd_common_libs.log_levels",
    "mfd_connect",
    "mfd_connect.exceptions",
    "pytest",
    "pytest_check",
    "requests",
):
    sys.modules.setdefault(_name, types.ModuleType(_name))
# The only attributes reached at import time.
sys.modules["mfd_common_libs.log_levels"].TEST_FAIL = 41
sys.modules["mfd_connect"].SSHConnection = Mock
sys.modules["mfd_connect.exceptions"].ConnectionCalledProcessError = OSError
sys.modules["pytest_check"].check = Mock()

from mtl_engine import ip_pools  # noqa: E402
from mtl_engine.ffmpeg import FFmpeg  # noqa: E402

# The pools are filled by the session fixture at run time; any session id does.
ip_pools.init(1)

TX_PORT = "0000:af:01.0"
RX_PORT = "0000:af:01.1"


class _Adapter(FFmpeg):
    """The adapter with bare state instead of a live host connection.

    Parameters mirror what test_multisession passes: a video_format only, with
    no explicit width/height/framerate/pixel_format.
    """

    def __init__(self, *, multiple):
        self.params = {
            "video_format": "i1080p25",
            "video_url": "/mnt/media/ParkJoy_1080p25.yuv",
            "nic_port_list": [TX_PORT, RX_PORT],
        }
        self._user_provided_params = set(self.params)
        self._ff_params = {"multiple_sessions": multiple}
        self._tx_commands = []
        self._rx_frame_spec = None

    def build(self):
        """``(rx command, tx commands)`` for this configuration."""
        rx_cmd, _ = self._build_yuv_h264_cmds(self.params["nic_port_list"])
        return rx_cmd, self._tx_commands


def _streams(cmd, ip_flag):
    """``(dst ip, udp port, payload type)`` per stream, in command order."""
    return re.findall(rf"-{ip_flag} (\S+) -udp_port (\S+) -payload_type (\S+)", cmd)


class MultisessionTxTests(unittest.TestCase):
    def setUp(self):
        self.rx, self.tx = _Adapter(multiple=True).build()

    def test_tx_is_one_process_with_an_output_per_rx_input(self):
        self.assertEqual(self.rx.count("-f mtl_st20p -i"), 2, "RX asks for two")
        self.assertEqual(len(self.tx), 1, "a second DPDK process cannot claim the VF")
        self.assertEqual(re.findall(r"-p_port (\S+)", self.tx[0]), [TX_PORT] * 2)
        # Per-output -p_sip, so the harness does not depend on the muxer
        # inheriting the address from the already-initialised MTL instance.
        self.assertEqual(self.tx[0].count("-p_sip "), 2)

    def test_tx_streams_match_the_rx_inputs(self):
        self.assertEqual(_streams(self.tx[0], "p_tx_ip"), _streams(self.rx, "p_rx_ip"))
        self.assertEqual(len(_streams(self.rx, "p_rx_ip")), 2)


class SingleSessionTxTests(unittest.TestCase):
    def test_tx_transmits_exactly_the_one_rx_input(self):
        rx, tx = _Adapter(multiple=False).build()
        self.assertEqual(len(tx), 1)
        self.assertEqual(_streams(tx[0], "p_tx_ip"), _streams(rx, "p_rx_ip"))
        self.assertEqual(
            _streams(tx[0], "p_tx_ip"), [(ip_pools.rx_multicast[0], "20000", "112")]
        )


if __name__ == "__main__":
    unittest.main()
