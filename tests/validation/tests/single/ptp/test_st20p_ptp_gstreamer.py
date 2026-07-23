# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation
"""GStreamer ST20P onboard-PTP validation.

Exercises the GStreamer plugin ``enable-ptp`` property (onboard PTP client) on
a single-host TX+RX ST20P pipeline. GStreamer drives synthetic planar media, so
this validates that the stream transports and md5-matches end to end with PTP
enabled; it is GStreamer-only because the ``enable-ptp`` knob lives in the
plugin (the RxTxApp PTP coverage lives under the integrity-based ptp tests).
"""

import os

import mtl_engine.media_creator as media_create
import pytest
from common.nicctl import InterfaceSetup
from mtl_engine import ip_pools
from mtl_engine.media_files import yuv_files_422rfc10


@pytest.mark.nightly
@pytest.mark.ptp
@pytest.mark.parametrize("application", ["gstreamer"])
@pytest.mark.parametrize(
    "media_file",
    [
        yuv_files_422rfc10["Crosswalk_720p"],
        yuv_files_422rfc10["ParkJoy_1080p"],
    ],
    indirect=["media_file"],
    ids=["Crosswalk_720p", "ParkJoy_1080p"],
)
def test_st20p_ptp_gstreamer(
    application,
    app_factory,
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_config,
    test_time,
    pcap_capture,
    media_file,
):
    """Stream ST20P over GStreamer with onboard PTP enabled."""
    media_file_info, media_file_path = media_file
    width = media_file_info["width"]
    height = media_file_info["height"]
    fps = media_file_info["fps"]
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )

    # v210 pixel groups are six pixels wide; fall back to planar otherwise.
    gst_format = "v210" if width % 6 == 0 else "I422_10LE"
    media_dir = str(host.connection.path(media_file_path).parent)
    input_file = media_create.create_video_file(
        width=width,
        height=height,
        framerate=fps,
        format=gst_format,
        media_path=media_dir,
        duration=3,
        host=host,
    )
    output_file = os.path.join(media_dir, "output_ptp_video.yuv")

    # PTP sync adds startup latency before the data window opens.
    ptp_test_time = max(test_time, 30)

    app = app_factory(application)
    app.create_command(
        session_type="st20p",
        nic_port_list=interfaces_list,
        destination_ip=ip_pools.rx_multicast[0],
        port=20000,
        width=width,
        height=height,
        framerate=f"p{fps}",
        gst_format=gst_format,
        input_file=input_file,
        output_file=output_file,
        enable_ptp=True,
        test_mode="multicast",
        test_time=ptp_test_time,
    )

    try:
        app.execute_test(
            build=mtl_path,
            test_time=ptp_test_time,
            host=host,
            netsniff=pcap_capture,
        )
    finally:
        media_create.remove_file(input_file, host=host)
        media_create.remove_file(output_file, host=host)
