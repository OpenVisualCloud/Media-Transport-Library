# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation
"""ST 2110-21 shaping profiles across increasing bandwidth at fixed FPS."""

import pytest
from common.nicctl import InterfaceSetup
from mtl_engine import ip_pools
from mtl_engine.media_files import parse_fps_to_pformat, yuv_files

pytestmark = pytest.mark.verified

CORE = ["i1080p59", "i2160p59", "i2160p119"]


@pytest.mark.nightly
@pytest.mark.parametrize(
    "application",
    [
        "rxtxapp",
        pytest.param(
            "ffmpeg",
            marks=pytest.mark.skip(
                reason="FFmpeg does not support pacing mode selection"
            ),
        ),
    ],
)
@pytest.mark.tx_side
@pytest.mark.parametrize("pacing", ["narrow", "wide", "linear"])
@pytest.mark.parametrize(
    "media_file",
    [yuv_files[k] for k in CORE],
    indirect=["media_file"],
    ids=CORE,
)
def test_st20p_pacing(
    application,
    app_factory,
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_config,
    test_time,
    pacing,
    pcap_capture,
    output_files,
    media_integrity,
    media_file,
):
    """Test different pacing modes (narrow, wide, linear)."""
    media_file_info, media_file_path = media_file
    rx_output = output_files.register(f"{media_file_path}.out")
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )

    config_params = {
        "session_type": "st20p",
        "nic_port_list": interfaces_list,
        "source_ip": ip_pools.tx[0],
        "destination_ip": ip_pools.rx[0],
        "port": 20000,
        "width": media_file_info["width"],
        "height": media_file_info["height"],
        "framerate": parse_fps_to_pformat(media_file_info["fps"]),
        "pixel_format": media_file_info["file_format"],
        "transport_format": media_file_info["format"],
        "input_file": media_file_path,
        "output_file": rx_output,
        "test_mode": "multicast",
        "pacing": pacing,
        "test_time": test_time,
    }

    height = media_file_info.get("height", 0)
    if height >= 2160:
        actual_test_time = max(test_time, 12)
    else:
        actual_test_time = max(test_time, 8)

    # EBU LIST 2.2.2 cannot name 119.88 fps: its detector takes two inter-frame
    # RTP timestamp deltas and returns Rate(180000, d1 + d2), so it can only
    # express a rate whose 180000/fps is a whole number of ticks. 119.88 needs
    # 1501.5 and comes out as 90000/751, and every measure anchored to the frame
    # grid is then judged against that -- the inter-frame delta limit collapses
    # to {751, 751} against a correct 750/751 alternation, and the VRX/TRO grid
    # drifts 2.78us per frame. Cinst, which does not use the grid, reads narrow
    # on the same capture, so the pacing itself is what the test asked for.
    # Upstream rate_calculator.cpp hard-codes this escape for 23.976 fps, whose
    # arithmetic fails the same way, but not for 119.88.
    if parse_fps_to_pformat(media_file_info["fps"]) == "p119":
        pcap_capture.skip("EBU LIST 2.2.2 mis-detects 119.88 fps as 90000/751")

    app = app_factory(application)
    app.create_command(**config_params)
    app.execute_test(
        build=mtl_path,
        test_time=actual_test_time,
        host=host,
        compliance=pcap_capture,
        integrity=media_integrity,
    )
