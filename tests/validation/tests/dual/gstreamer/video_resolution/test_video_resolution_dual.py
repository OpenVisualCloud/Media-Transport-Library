# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

import os

import mtl_engine.media_creator as media_create
import pytest
from mtl_engine import GstreamerApp
from mtl_engine.media_files import yuv_files
from tests.xfail import SDBQ1971_conversion_v210_720p_error


@pytest.mark.parametrize(
    "file",
    [
        pytest.param(f, marks=pytest.mark.smoke) if f == "i1080p59" else f
        for f in yuv_files.keys()
    ],
)
def test_video_resolutions_dual(
    hosts,
    build,
    media,
    nic_port_list,
    file,
    request,
    test_time,
    test_config,
    prepare_ramdisk,
):
    """Test GStreamer ST20P video resolution in dual host configuration."""
    video_file = yuv_files[file]
    video_file["format"] = "v210"

    # Get TX and RX hosts
    host_list = list(hosts.values())
    if len(host_list) < 2:
        pytest.skip("Dual tests require at least 2 hosts")

    tx_host = host_list[0]
    rx_host = host_list[1]

    SDBQ1971_conversion_v210_720p_error(
        video_format=video_file["format"],
        resolution_width=video_file["height"],
        request=request,
    )

    # Create input file on TX host
    input_file_path = media_create.create_video_file(
        width=video_file["width"],
        height=video_file["height"],
        framerate=video_file["fps"],
        format=GstreamerApp.video_format_change(video_file["format"]),
        media_path=media,
        host=tx_host,
    )

    capture_cfg = dict(test_config.get("capture_cfg", {})) if test_config else {}
    capture_cfg["test_name"] = f"test_video_resolutions_dual_{file}"

    try:
        result = GstreamerApp.execute_dual_st20p_test(
            build=build,
            tx_nic_port=tx_host.vfs[0],
            rx_nic_port=rx_host.vfs[0],
            input_path=input_file_path,
            width=video_file["width"],
            height=video_file["height"],
            framerate=video_file["fps"],
            format=GstreamerApp.video_format_change(video_file["format"]),
            payload_type=112,
            queues=4,
            test_time=test_time,
            tx_host=tx_host,
            rx_host=rx_host,
            capture_cfg=capture_cfg,
        )

        assert result, f"GStreamer dual video resolution test failed for resolution {file}"

    finally:
        # Remove the input file on TX host
        media_create.remove_file(input_file_path, host=tx_host)
