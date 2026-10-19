# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

import pytest
from common.nicctl import InterfaceSetup
from mtl_engine.media_files import yuv_files_422p10le, yuv_files_st22p_input_formats


@pytest.mark.nightly
@pytest.mark.parametrize(
    "application",
    [
        "rxtxapp",
        "ffmpeg",
        pytest.param(
            "gstreamer",
            marks=pytest.mark.skip(reason="GStreamer does not support st22p pipeline"),
        ),
    ],
)
@pytest.mark.parametrize(
    "media_file",
    list(yuv_files_422p10le.values()),
    indirect=["media_file"],
    ids=list(yuv_files_422p10le.keys()),
)
def test_st22p_format(
    application,
    app_factory,
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_time,
    test_config,
    output_files,
    media_file,
):
    """Test st22p with different media formats/resolutions."""
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )
    # JPEG-XS plugin init adds 3-10s on top of MTL init.
    test_time = max(test_time, 90)

    app = app_factory(application)
    app.require_encoder(host, "libsvt_jpegxs", use_mtl_plugin=True)
    app.create_command(
        session_type="st22p",
        test_mode="multicast",
        nic_port_list=interfaces_list,
        width=media_file_info["width"],
        height=media_file_info["height"],
        framerate=f"p{media_file_info['fps']}",
        codec="JPEG-XS",
        quality="speed",
        pixel_format=media_file_info["file_format"],
        input_file=media_file_path,
        output_file=output_files.register(f"{media_file_path}.out"),
        codec_threads=2,
        test_time=test_time,
    )

    app.execute_test(build=mtl_path, test_time=test_time, host=host)


# Explicit ids spell ``application`` the way pytest_mfd_logging does: the
# combined report reads the app from that token.
@pytest.mark.nightly
@pytest.mark.parametrize(
    "application, media_file",
    [
        pytest.param("rxtxapp", info, id=f"{fmt}-|application = rxtxapp|")
        for fmt, info in yuv_files_st22p_input_formats.items()
    ]
    + [
        pytest.param(
            "ffmpeg",
            yuv_files_st22p_input_formats["yuv422p12le"],
            id="yuv422p12le-|application = ffmpeg|",
            marks=pytest.mark.skip(
                reason="FFmpeg mtl_st22p receives only yuv422p10le and rgb24"
            ),
        ),
        pytest.param(
            "gstreamer",
            yuv_files_st22p_input_formats["yuv422p12le"],
            id="yuv422p12le-|application = gstreamer|",
            marks=pytest.mark.skip(reason="GStreamer does not support st22p pipeline"),
        ),
    ],
    indirect=["media_file"],
)
def test_st22p_input_format(
    application,
    app_factory,
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_time,
    test_config,
    media_file,
):
    """Test st22p with the input-format footage the JPEG XS encoder takes."""
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )
    # JPEG-XS plugin init adds 3-10s on top of MTL init.
    test_time = max(test_time, 90)
    # The encoder rejects 4:2:0 in speed mode, which sets no vertical decomposition.
    is_420 = media_file_info["file_format"] == "YUV420PLANAR8"

    app = app_factory(application)
    app.require_encoder(host, "libsvt_jpegxs", use_mtl_plugin=True)
    app.create_command(
        session_type="st22p",
        test_mode="multicast",
        nic_port_list=interfaces_list,
        width=media_file_info["width"],
        height=media_file_info["height"],
        framerate=f"p{media_file_info['fps']}",
        codec="JPEG-XS",
        quality="quality" if is_420 else "speed",
        pixel_format=media_file_info["file_format"],
        input_file=media_file_path,
        codec_threads=2,
        test_time=test_time,
    )

    app.execute_test(build=mtl_path, test_time=test_time, host=host)
