# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2026 Intel Corporation

"""Cross-application interoperability at one baseline stream shape."""

import pytest
from mtl_engine.media_files import yuv_files_422p10le

pytestmark = [pytest.mark.verified, pytest.mark.nightly]


@pytest.mark.parametrize(
    "tx_application, rx_application, media_file",
    [
        ("rxtxapp", "ffmpeg", yuv_files_422p10le["Penguin_1080p"]),
        ("ffmpeg", "rxtxapp", yuv_files_422p10le["Penguin_1080p"]),
    ],
    ids=["rxtxapp_to_ffmpeg", "ffmpeg_to_rxtxapp"],
    indirect=["media_file"],
)
@pytest.mark.parametrize(
    "application",
    [
        "ffmpeg",
        pytest.param(
            "rxtxapp",
            marks=pytest.mark.skip(
                reason="Cross-app orchestration requires the FFmpeg adapter"
            ),
            id="rxtxapp_baseline",
        ),
        pytest.param(
            "rxtxapp",
            marks=pytest.mark.skip(
                reason="Cross-app multi-session orchestration requires the FFmpeg adapter"
            ),
            id="rxtxapp_multisession",
        ),
    ],
)
def test_cross_app(
    application,
    tx_application,
    rx_application,
    app_factory,
    hosts,
    test_time,
    mtl_path,
    setup_interfaces,
    test_config,
    media_file,
):
    """One TX app streaming ST2110-20 to a different RX app."""
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )
    fps_num, _, fps_den = media_file_info["fps"].partition("/")
    config_params = dict(
        session_type="st20p",
        nic_port_list=interfaces_list,
        width=media_file_info["width"],
        height=media_file_info["height"],
        framerate=f"p{int(float(fps_num) / float(fps_den or 1))}",
        pixel_format=media_file_info["file_format"],
        transport_format=media_file_info["format"],
        input_file=media_file_path,
        tx_application=tx_application,
        rx_application=rx_application,
        test_time=test_time,
    )

    app = app_factory(application)
    app.create_command(**config_params)
    assert app.execute_test(
        build=mtl_path,
        test_time=test_time,
        host=host,
    ), f"{tx_application} to {rx_application} interop failed"
