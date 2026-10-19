# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

import pytest
from mtl_engine.config.universal_params import UNIVERSAL_PARAMS
from mtl_engine.media_files import yuv_files_422p10le

pytestmark = [pytest.mark.verified, pytest.mark.nightly]

MULTISESSION_MEDIA = [
    ("i1080p25", "p25", yuv_files_422p10le["Penguin_1080p"]),
    ("i1080p30", "p30", yuv_files_422p10le["Penguin_1080p"]),
]


@pytest.mark.parametrize(
    "application",
    [
        "ffmpeg",
        "rxtxapp",
        pytest.param(
            "gstreamer",
            marks=pytest.mark.skip(reason="GStreamer runs one session per pipeline"),
        ),
    ],
)
@pytest.mark.parametrize(
    "output_format",
    [
        "yuv",
        pytest.param(
            "h264", marks=pytest.mark.skip(reason="h264 multi-session not validated")
        ),
    ],
)
@pytest.mark.parametrize(
    "framerate, media_file",
    [media[1:] for media in MULTISESSION_MEDIA],
    ids=[media[0] for media in MULTISESSION_MEDIA],
    indirect=["media_file"],
)
def test_multisession(
    application,
    app_factory,
    hosts,
    mtl_path,
    setup_interfaces,
    test_time,
    test_config,
    output_files,
    media_integrity,
    media_file,
    framerate,
    output_format,
):
    """Test multiple simultaneous ST2110-20 sessions.

    Validates that the framework can handle two concurrent TX/RX sessions
    on the same interface pair, and that each session's recording matches
    the source.
    """
    if application == "rxtxapp":
        pytest.skip("RxTxApp does not support output_format parameter")
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )
    app = app_factory(application)
    app.create_command(
        session_type="st20p",
        nic_port_list=interfaces_list,
        width=media_file_info["width"],
        height=media_file_info["height"],
        framerate=framerate,
        pixel_format=media_file_info["file_format"],
        input_file=media_file_path,
        output_file=f"{media_file_path}.out",
        output_format=output_format,
        multiple_sessions=True,
        test_time=test_time * 3,
        # Each session records its own file; split the default cap between them.
        rx_max_file_size=UNIVERSAL_PARAMS["rx_max_file_size"] // 2,
    )
    # Track the files the RX writes: <output_file>_<k> for session k.
    for path in app.rx_output_files():
        output_files.register(path)
    result = app.execute_test(
        build=mtl_path,
        test_time=test_time * 3,
        host=host,
        integrity=media_integrity,
    )
    assert result, f"Multi-session test failed for {framerate} ({application})"
