# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

import pytest
from mtl_engine.media_files import yuv_files_422p10le, yuv_files_422rfc10

pytestmark = [pytest.mark.verified, pytest.mark.nightly]

# FFmpeg reads these assets with `-f rawvideo -pix_fmt <file_format>`, so file_format
# must name a real AVPixelFormat. RFC 4175 is a transport packing instead: its pgroup is
# 2 pixels in 5 bytes, so a planar -pix_fmt misreads it at 4 bytes per pixel.
FORMAT_CASES = [
    pytest.param("i1080p25", "p25", yuv_files_422p10le["Penguin_1080p"]),
    pytest.param("i1080p30", "p30", yuv_files_422p10le["Penguin_1080p"]),
    pytest.param("i1080p60", "p60", yuv_files_422p10le["Penguin_1080p"]),
    pytest.param(
        "i2160p60",
        "p60",
        yuv_files_422rfc10["Crosswalk_4K"],
        marks=pytest.mark.skip(
            reason="no 2160p entry in media_files.py's FFmpeg-ingestible tables"
        ),
    ),
]


# Each leg carries only the kwargs its own adapter understands: output_format names the
# RX output encoding and is FFmpeg-private, and RxTxApp always writes raw frames, so its
# leg needs no counterpart. No row names an id: pytest_mfd_logging synthesises
# `|application = <name>|`, which is the token the nightly report groups its rows by, and
# an explicit id replaces it.
APPLICATION_LEGS = [
    pytest.param("ffmpeg", {"output_format": "yuv"}),
    pytest.param("ffmpeg", {"output_format": "h264"}),
    pytest.param("rxtxapp", {}),
]


@pytest.mark.parametrize("application, leg_params", APPLICATION_LEGS)
@pytest.mark.parametrize(
    "video_format, fps, media_file",
    FORMAT_CASES,
    ids=[case.values[0] for case in FORMAT_CASES],
    indirect=["media_file"],
)
def test_format(
    application,
    leg_params,
    app_factory,
    hosts,
    mtl_path,
    setup_interfaces,
    test_time,
    test_config,
    media_file,
    video_format,
    fps,
):
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )
    app = app_factory(application)
    if leg_params.get("output_format") == "h264":
        app.require_encoder(host, "libopenh264")
    app.create_command(
        session_type="st20p",
        nic_port_list=interfaces_list,
        test_mode="multicast",
        width=media_file_info["width"],
        height=media_file_info["height"],
        framerate=fps,
        pixel_format=media_file_info["file_format"],
        transport_format=media_file_info["format"],
        input_file=media_file_path,
        test_time=test_time,
        **leg_params,
    )
    result = app.execute_test(
        build=mtl_path,
        test_time=test_time,
        host=host,
    )
    assert result, f"Format test failed: {video_format} {application} {leg_params}"
