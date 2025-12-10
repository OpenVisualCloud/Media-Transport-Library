# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

import pytest
from mtl_engine.media_files import yuv_files_422rfc10
from mtl_engine.rxtxapp import RxTxApp


@pytest.mark.nightly
@pytest.mark.parametrize(
    "media_file",
    list(yuv_files_422rfc10.values()),
    indirect=["media_file"],
    ids=list(yuv_files_422rfc10.keys()),
)
def test_resolutions_refactored(
    hosts,
    build,
    media,
    nic_port_list,
    test_time,
    prepare_ramdisk,
    media_file,
):
    """Test different video resolutions"""
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]

    app = RxTxApp(f"{build}/tests/tools/RxTxApp/build")

    config_params = {
        "session_type": "st20p",
        "nic_port": host.vfs[0] if host.vfs else "0000:31:01.0",
        "nic_port_list": host.vfs,
        "destination_ip": "239.168.48.9",
        "port": 20000,
        "width": media_file_info["width"],
        "height": media_file_info["height"],
        "framerate": f"p{media_file_info['fps']}",
        "pixel_format": media_file_info["file_format"],
        "transport_format": media_file_info["format"],
        "input_file": media_file_path,
        "test_mode": "multicast",
        "test_time": test_time,
    }

    height = media_file_info.get("height", 0)

    if height >= 2160:
        config_params.update(
            {"pacing": "linear", "packing": "GPM_SL", "tx_no_chain": True}
        )
    elif height >= 1080:
        config_params.update({"pacing": "wide", "packing": "GPM", "tx_no_chain": False})
    else:
        config_params.update(
            {"pacing": "narrow", "packing": "GPM", "tx_no_chain": False}
        )

    app.create_command(**config_params)

    actual_test_time = test_time
    if height >= 2160:
        actual_test_time = max(test_time, 15)
    elif height >= 1080:
        actual_test_time = max(test_time, 10)
    else:
        actual_test_time = max(test_time, 8)

    app.execute_test(build=build, test_time=actual_test_time, host=host)
