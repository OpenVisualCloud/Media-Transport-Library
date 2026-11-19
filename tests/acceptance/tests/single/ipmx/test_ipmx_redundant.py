# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation
"""IPMX ST 2022-7 sender: both legs carry valid Sender Reports (TP-1 13).

RxTxApp sends one redundant st20p IPMX session on two TX ports to an IPMX RX
session on two other ports. The SDP holds both legs (``a=group:DUP``) and must
pass the SDP script, as must each leg's own SDP; each leg is captured on its
RX port and must pass every SHALL and SHOULD of the raw validator
(``--dst-ip`` picks the leg); the RX side must report the Sender Report line
OK on both ports. Port layout: each leg leaves one PF and arrives at the
other, so both legs cross the cable, where the capture sees them.
"""

import pytest
from mtl_engine import ip_pools
from tests.single.ipmx import ipmx_vsf


@pytest.mark.nightly
def test_ipmx_redundant(
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
    """Both legs of an IPMX 2022-7 stream must pass the VSF scripts."""
    host = list(hosts.values())[0]
    # Two VFs of each PF, PF by PF: TX P and RX R on the first PF, TX R and
    # RX P on the second.
    if test_config.get("interface_type", "VF").upper() != "VF":
        pytest.skip("ST 2022-7 case needs four VFs")
    vfs = setup_interfaces.get_interfaces_list_single("VF", count=4)
    interfaces = [vfs[0], vfs[2], vfs[3], vfs[1]]
    width, height = 1920, 1080
    frame_bytes = width * height * 3
    media = ipmx_media(f"{width}x{height}_RGB8.raw", frame_bytes)
    app = ipmx_vsf.create_st20p(
        app_factory, interfaces, media, width, height, "p60", "RGB8", "RGB_8bit",
        test_time, redundant=True, source_ip=ip_pools.tx[0],
        destination_ip=ip_pools.rx_multicast[0], source_ip_r=ip_pools.tx_r[0],
        destination_ip_r=ip_pools.rx_multicast[1], port=20000,
    )  # fmt: skip
    _, sdp, _ = ipmx_vsf.run_sender(
        app, host, mtl_path, test_time, ipmx_tools, ipmx_capture, "st20p", "raw",
        frame_bytes, width, height, "60", "RGB", 8,
    )  # fmt: skip
    assert "a=group:DUP" in sdp, sdp
