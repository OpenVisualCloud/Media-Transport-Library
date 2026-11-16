# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation
"""IPMX sender locked to a PTP grandmaster (TR-10-1 8.7, TP-1 13.2 and 13.3).

With the MTL built-in PTP client (RxTxApp ``--ptp``, ``enable_ptp``)
following a grandmaster, the SDP and the Sender Report Info Block carry
``ts-refclk:ptp=IEEE1588-2008:<grandmaster>:<domain>`` instead of the
``localmac`` form, and the VSF scripts run with the PTP stream profile
(``PTP=true``). The case skips when no grandmaster answers on the network.
"""

import pytest
from tests.single.ipmx import ipmx_vsf


@pytest.mark.ptp
@pytest.mark.nightly
def test_ipmx_ptp(
    hosts,
    mtl_path,
    test_time,
    test_config,
    setup_interfaces,
    app_factory,
    ipmx_tools,
    ipmx_capture,
    ipmx_media,
):
    """A PTP-locked IPMX sender must publish its grandmaster in the SDP and the SRs."""
    host = list(hosts.values())[0]
    interfaces = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )
    width, height = 1920, 1080
    frame_bytes = width * height * 3
    media = ipmx_media(f"{width}x{height}_RGB8.raw", frame_bytes)
    app = ipmx_vsf.create_st20p(
        app_factory, interfaces, media, width, height, "p60", "RGB8", "RGB_8bit",
        test_time, enable_ptp=True,
    )  # fmt: skip
    _, sdp, _ = ipmx_vsf.run_sender(
        app, host, mtl_path, test_time, ipmx_tools, ipmx_capture, "st20p", "raw",
        frame_bytes, width, height, "60", "RGB", 8, ptp=True,
    )  # fmt: skip
    assert "a=ts-refclk:ptp=IEEE1588-2008:" in sdp, sdp
