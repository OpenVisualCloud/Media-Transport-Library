# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation
"""IPMX ST 2110-20 sender checks of VSF TR-10 TP-1 section 13, through RxTxApp.

RxTxApp sends one st20p IPMX stream (JSON key ``"ipmx": true``) to an IPMX RX
session and writes the library SDP (``--ipmx_sdp_dir``). The wire is captured
from before the start, then the VSF scripts check it as TP-1 13.6 does:
``ipmx_raw_validate_pcap.py --cmax --cfg --sdp`` (13.1, 13.3, 13.6.1, and the
CMAX and TR-10-9 11.2 parts of 13.5) and ``sdp_validation_script.py`` (13.2,
13.6.2). The RX side must report its Sender Report line OK. The matrix is the
IPMX ST 2110-20 MVP scope: 1080p50/59.94/60 and 2160p59.94/60, each in YUV
4:2:2 10-bit, RGB 8-bit and RGB 10-bit.
"""

import pytest
from tests.single.ipmx import ipmx_vsf

# One point per case: (width, height, RxTxApp fps, exactframerate, file
# format, transport format, VSF sampling, depth, bytes per pixel of the file).
# The files hold pgroup formats, so TX sends them without a conversion. The
# TP-1 example pair (1080p59.94 YUV 4:2:2 10-bit, 1080p60 RGB 8-bit) is smoke.
_ST20P_RATES = [
    (1920, 1080, "p50", "50", "1080p50"),
    (1920, 1080, "p59", "60000/1001", "1080p59"),
    (1920, 1080, "p60", "60", "1080p60"),
    (3840, 2160, "p59", "60000/1001", "2160p59"),
    (3840, 2160, "p60", "60", "2160p60"),
]
_ST20P_FORMATS = [
    ("YUV422RFC4175PG2BE10", "YUV_422_10bit", "YCbCr-4:2:2", 10, 2.5, "yuv422_10"),
    ("RGB8", "RGB_8bit", "RGB", 8, 3, "rgb8"),
    ("RGBRFC4175PG4BE10", "RGB_10bit", "RGB", 10, 3.75, "rgb10"),
]
_ST20P_SMOKE = {"1080p59_yuv422_10", "1080p60_rgb8"}
ST20P_POINTS = [
    pytest.param(
        w, h, fps, rate, ffmt, tfmt, sampling, depth, bpp,
        id=f"{rid}_{fid}",
        marks=[pytest.mark.smoke] if f"{rid}_{fid}" in _ST20P_SMOKE else [],
    )
    for (w, h, fps, rate, rid) in _ST20P_RATES
    for (ffmt, tfmt, sampling, depth, bpp, fid) in _ST20P_FORMATS
]  # fmt: skip


@pytest.mark.nightly
@pytest.mark.parametrize(
    "width, height, fps, rate, file_fmt, transport_fmt, sampling, depth, bpp",
    ST20P_POINTS,
)
def test_ipmx_st20p_sender(
    hosts,
    mtl_path,
    test_time,
    test_config,
    setup_interfaces,
    app_factory,
    ipmx_tools,
    ipmx_capture,
    ipmx_media,
    width,
    height,
    fps,
    rate,
    file_fmt,
    transport_fmt,
    sampling,
    depth,
    bpp,
):
    """An IPMX st20p stream must pass the VSF pcap and SDP scripts."""
    host = list(hosts.values())[0]
    interfaces = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )
    frame_bytes = int(width * height * bpp)
    media = ipmx_media(f"{width}x{height}_{file_fmt}.raw", frame_bytes)

    app = ipmx_vsf.create_st20p(
        app_factory, interfaces, media, width, height, fps, file_fmt, transport_fmt,
        test_time,
    )  # fmt: skip
    ipmx_vsf.run_sender(
        app, host, mtl_path, test_time, ipmx_tools, ipmx_capture,
        "st20p", "raw", frame_bytes, width, height, rate, sampling, depth,
    )  # fmt: skip
