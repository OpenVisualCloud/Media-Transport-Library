# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

import pytest
from mtl_engine.app_refactored import Application
from mtl_engine.media_files import yuv_files_422rfc10


@pytest.mark.nightly
@pytest.mark.parametrize("pacing", ["narrow", "wide", "linear"])
@pytest.mark.parametrize(
    "media_file",
    [
        yuv_files_422rfc10["Crosswalk_720p"],
        yuv_files_422rfc10["ParkJoy_1080p"],
        yuv_files_422rfc10["Pedestrian_4K"],
    ],
    indirect=["media_file"],
    ids=["Crosswalk_720p", "ParkJoy_1080p", "Pedestrian_4K"],
)
def test_pacing_refactored(
    hosts,
    build,
    media,
    nic_port_list,
    test_time,
    pacing,
    test_config,
    prepare_ramdisk,
    media_file,
):
    """
    Test different pacing modes (narrow, wide, linear) using Application class refactored interface
    """
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]

    # Get capture configuration from test_config.yaml
    # This controls whether tcpdump capture is enabled, where to store the pcap, etc.
    capture_cfg = dict(test_config.get("capture_cfg", {}))
    capture_cfg["test_name"] = (
        f"test_pacing_refactored_{media_file_info['filename']}_{pacing}"  # Set a unique pcap file name
    )

    # Create Application instance for RxTxApp
    app = Application("RxTxApp", f"{build}/tests/tools/RxTxApp/build")
    
    # Configure application using universal parameters
    config_params = {
        "session_type": "st20p",
        "nic_port": host.vfs[0] if host.vfs else "0000:31:01.0",
        "nic_port_list": host.vfs,
        "source_ip": "192.168.17.101",     # TX interface IP
        "destination_ip": "192.168.17.102",  # RX interface IP for unicast loopback
        "port": 20000,
        "width": media_file_info["width"],
        "height": media_file_info["height"],
        "framerate": f"p{media_file_info['fps']}",
        "pixel_format": media_file_info["file_format"],
        "transport_format": media_file_info["format"],
        "pixel_format_rx": media_file_info["file_format"],
        "input_file": media_file_path,
        "test_mode": "unicast",
        "pacing": pacing,  # Specify the pacing mode to test
    }
    
    # Add pacing-specific optimizations based on resolution and pacing mode
    if media_file_info.get("height", 0) >= 2160:  # 4K content
        if pacing == "linear":
            config_params["tx_no_chain"] = True  # Better for 4K linear pacing
        else:
            config_params["tx_no_chain"] = False  # Keep chain for narrow/wide pacing
    elif pacing == "narrow":
        # Narrow pacing benefits from tighter timing control
        config_params["tx_no_chain"] = False

    app.create_command(**config_params)

    # Execute test using Application class
    # Use longer test time for 4K content and accurate pacing measurement
    actual_test_time = test_time
    if media_file_info.get("height", 0) >= 2160:  # 4K content
        actual_test_time = max(test_time, 12)  # Minimum 12 seconds for 4K pacing accuracy
    else:
        actual_test_time = max(test_time, 8)   # Minimum 8 seconds for HD pacing accuracy

    app.execute_test(
        build=build,
        test_time=actual_test_time,
        host=host,
        capture_cfg=capture_cfg,
    )
