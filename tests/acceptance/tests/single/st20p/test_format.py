# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

import pytest
from mtl_engine.media_files import yuv_files_422p10le, yuv_files_422p10le_4k

pytestmark = [pytest.mark.verified, pytest.mark.nightly]


# FFmpeg is the only backend here (rxtxapp is skipped below), and both ends of
# its pipeline name a frame buffer with an AVPixelFormat: -pix_fmt on the
# rawvideo reader and on the mtl_st20p muxer. RFC4175 is an ST 2110-20 wire
# packing, so no AVPixelFormat can name it -- the sources must be planar. The
# framerate is the parametrized one, not the file's: FFmpeg loops the input
# (-stream_loop -1) and -re throttles it to the rate asked for.
FORMAT_CASES = [
    ("i1080p25", "p25", yuv_files_422p10le["Penguin_1080p"]),
    ("i1080p30", "p30", yuv_files_422p10le["Penguin_1080p"]),
    ("i1080p60", "p60", yuv_files_422p10le["Penguin_1080p"]),
    ("i2160p30", "p30", yuv_files_422p10le_4k["Penguin_4K"]),
    ("i2160p60", "p60", yuv_files_422p10le_4k["Penguin_4K"]),
]


@pytest.mark.parametrize("application", ["ffmpeg", "rxtxapp"])
@pytest.mark.parametrize("output_format", ["yuv", "h264"])
@pytest.mark.parametrize(
    "video_format, fps, media_file",
    FORMAT_CASES,
    ids=[c[0] for c in FORMAT_CASES],
    indirect=["media_file"],
)
def test_format(
    application,
    app_factory,
    hosts,
    mtl_path,
    setup_interfaces,
    test_time,
    test_config,
    media_file,
    video_format,
    fps,
    output_format,
    output_files,
):
    if application == "rxtxapp":
        pytest.skip("RxTxApp does not support output_format parameter")
    media_file_info, media_file_path = media_file
    # A raw 4:2:2 10-bit 4K frame is 33 MB, so a 60s recording is 50 GB at p30 and
    # 100 GB at p60, more than the 32 GiB media ramdisk the RX records to. The
    # h264 leg still covers 4K RX and yuv still runs at 1080p.
    if output_format == "yuv" and media_file_info["height"] >= 2160:
        pytest.skip("raw 4K recording does not fit the media ramdisk")
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )
    app = app_factory(application)
    if output_format == "h264":
        app.require_encoder(host, "libopenh264")
    # On ext4, closing the O_TRUNC'd output flushes it and holds the VF past the
    # stop; tmpfs does not. 1080p60 yuv fills the ramdisk above a ~66s test_time.
    rx_output = output_files.register(f"{media_file_path}_out.{output_format}")
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
        output_format=output_format,
        output_file=rx_output,
        test_time=test_time,
    )
    result = app.execute_test(
        build=mtl_path,
        test_time=test_time,
        host=host,
    )
    assert result, f"Format test failed for {video_format} ({output_format})"
