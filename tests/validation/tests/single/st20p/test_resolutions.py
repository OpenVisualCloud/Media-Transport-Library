# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

import os

import mtl_engine.media_creator as media_create
import pytest
from common.nicctl import InterfaceSetup
from mtl_engine import ip_pools
from mtl_engine.media_files import yuv_files_422rfc10
from tests.xfail import SDBQ1971_conversion_v210_720p_error

pytestmark = pytest.mark.verified

# GStreamer reads raw planar media (I422_10LE / v210), not the RFC4175-packed
# files RxTxApp/FFmpeg consume, so it self-provides a synthetic clip of the same
# resolution. Generating raw 4K/8K clips is impractical, so GStreamer only runs
# the <=1080p resolutions; RxTxApp/FFmpeg still sweep the full range.
_GST_MAX_HEIGHT = 1080


def _prepare_gstreamer_media(host, info, media_file_path, request):
    """Generate a synthetic planar clip for GStreamer; return (input, output, gst_format).

    The st20 plugin only emits v210 when the width is a multiple of six (pixel
    groups are six pixels wide); otherwise it falls back to I422_10LE. The clip
    is written next to the fixture-provided media file so it lands in the
    managed media/ramdisk directory rather than a hardcoded path.
    """
    width, height, fps = info["width"], info["height"], info["fps"]
    gst_format = "v210" if width % 6 == 0 else "I422_10LE"
    if gst_format == "v210":
        SDBQ1971_conversion_v210_720p_error(
            video_format=gst_format, resolution_height=height, request=request
        )

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
    output_file = os.path.join(media_dir, "output_video.yuv")
    return input_file, output_file, gst_format


@pytest.mark.nightly
@pytest.mark.parametrize(
    "application",
    [
        "rxtxapp",
        "ffmpeg",
        "gstreamer",
    ],
)
@pytest.mark.parametrize(
    "media_file",
    [
        pytest.param(v, marks=pytest.mark.smoke) if k == "Penguin_1080p" else v
        for k, v in yuv_files_422rfc10.items()
    ],
    indirect=["media_file"],
    ids=list(yuv_files_422rfc10.keys()),
)
def test_st20p_resolutions(
    application,
    app_factory,
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_config,
    test_time,
    pcap_capture,
    media_file,
    request,
):
    """Test different video resolutions across all framework adapters."""
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )
    height = media_file_info.get("height", 0)

    config_params = {
        "session_type": "st20p",
        "nic_port_list": interfaces_list,
        "destination_ip": ip_pools.rx_multicast[0],
        "port": 20000,
        "width": media_file_info["width"],
        "height": height,
        "framerate": f"p{media_file_info['fps']}",
        "pixel_format": media_file_info["file_format"],
        "transport_format": media_file_info["format"],
        "test_mode": "multicast",
        "test_time": test_time,
    }

    gst_cleanup = []
    if application == "gstreamer":
        if height > _GST_MAX_HEIGHT:
            pytest.skip(
                f"GStreamer st20p resolution sweep limited to <={_GST_MAX_HEIGHT}p "
                "(raw planar clip generation)"
            )
        input_file, output_file, gst_format = _prepare_gstreamer_media(
            host, media_file_info, media_file_path, request
        )
        config_params.update(
            input_file=input_file, output_file=output_file, gst_format=gst_format
        )
        gst_cleanup = [input_file, output_file]
    else:
        config_params["input_file"] = media_file_path
        if height >= 2160:
            config_params.update(
                {"pacing": "linear", "packing": "GPM_SL", "tx_no_chain": True}
            )
        elif height >= 1080:
            config_params.update(
                {"pacing": "wide", "packing": "GPM", "tx_no_chain": False}
            )
        else:
            config_params.update(
                {"pacing": "narrow", "packing": "GPM", "tx_no_chain": False}
            )

    actual_test_time = test_time
    if height >= 2160:
        actual_test_time = max(test_time, 15)
    elif height >= 1080:
        actual_test_time = max(test_time, 10)
    else:
        actual_test_time = max(test_time, 8)

    app = app_factory(application)
    app.create_command(**config_params)

    try:
        app.execute_test(
            build=mtl_path,
            test_time=actual_test_time,
            host=host,
            netsniff=pcap_capture,
        )
    finally:
        for path in gst_cleanup:
            media_create.remove_file(path, host=host)
