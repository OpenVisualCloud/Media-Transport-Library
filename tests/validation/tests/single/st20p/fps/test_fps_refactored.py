# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

import pytest
from mtl_engine.media_files import yuv_files_422rfc10
from mtl_engine.rxtxapp import RxTxApp


@pytest.mark.nightly
@pytest.mark.parametrize(
    "media_file",
    [yuv_files_422rfc10["ParkJoy_1080p"]],
    indirect=["media_file"],
    ids=["ParkJoy_1080p"],
)
@pytest.mark.parametrize(
    "fps",
    [
        "p23",
        "p24",
        "p25",
        pytest.param("p29", marks=pytest.mark.smoke),
        "p30",
        "p50",
        "p59",
        "p60",
        "p100",
        "p119",
        "p120",
    ],
)
def test_fps_refactored(
    hosts,
    build,
    media,
    nic_port_list,
    test_time,
    fps,
    prepare_ramdisk,
    media_file,
):
    """Test different frame rates"""
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]

    app = RxTxApp(f"{build}/tests/tools/RxTxApp/build")

    config_params = {
        "session_type": "st20p",
        "nic_port_list": host.vfs,
        "test_mode": "multicast",
        "destination_ip": "239.168.48.9",
        "port": 20000,
        "width": media_file_info["width"],
        "height": media_file_info["height"],
        "framerate": fps,
        "pixel_format": media_file_info["file_format"],
        "transport_format": media_file_info["format"],
        "input_file": media_file_path,
        "test_time": test_time,
    }

    if fps in ["p30", "p50", "p59", "p60"]:
        config_params.update({"pacing": "gap", "tx_no_chain": True})
    elif fps in ["p100", "p119", "p120"]:
        config_params.update({"pacing": "linear", "tx_no_chain": True})

    app.create_command(**config_params)

    actual_test_time = test_time
    if fps in ["p30", "p50", "p59", "p60"]:
        actual_test_time = max(test_time, 15)
    elif fps in ["p100", "p119", "p120"]:
        actual_test_time = max(test_time, 10)

    app.execute_test(build=build, test_time=actual_test_time, host=host)
