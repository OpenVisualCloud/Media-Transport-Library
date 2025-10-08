# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

import pytest
from mtl_engine.app_refactored import Application
from mtl_engine.media_files import yuv_files_422rfc10


@pytest.mark.nightly
@pytest.mark.parametrize("packing", ["GPM_SL", "GPM"])
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
def test_packing_refactored(
    hosts,
    build,
    media,
    nic_port_list,
    test_time,
    packing,
    test_config,
    prepare_ramdisk,
    media_file,
):
    """
    Test different packing modes (GPM_SL, GPM) using Application class refactored interface
    """
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]

    # Get capture configuration from test_config.yaml
    # This controls whether tcpdump capture is enabled, where to store the pcap, etc.
    capture_cfg = dict(test_config.get("capture_cfg", {}))
    capture_cfg["test_name"] = (
        f"test_packing_refactored_{media_file_info['filename']}_{packing}"  # Set a unique pcap file name
    )

    # Create Application instance for RxTxApp
    app = Application("RxTxApp", f"{build}/tests/tools/RxTxApp/build")
    
    # Configure application using universal parameters
    config_params = {
        "session_type": "st20p",
        "nic_port": host.vfs[0] if host.vfs else "0000:31:01.0",
        "nic_port_list": host.vfs,
        # Explicit dual interface loopback IPs for unicast mode
        "source_ip": "192.168.17.101",      # TX interface IP
        "destination_ip": "192.168.17.102", # RX interface IP (unicast destination)
        "port": 20000,
        "width": media_file_info["width"],
        "height": media_file_info["height"],
        "framerate": f"p{media_file_info['fps']}",
        "pixel_format": media_file_info["file_format"],
        "transport_format": media_file_info["format"],
        "pixel_format_rx": media_file_info["file_format"],
        "input_file": media_file_path,
        "test_mode": "unicast",
        "packing": packing,  # Specify the packing mode to test
    }
    
    # Add packing-specific optimizations based on resolution and packing mode
    if media_file_info.get("height", 0) >= 2160:  # 4K content
        if packing == "GPM_SL":
            # GPM_SL (Single Line) mode is more efficient for 4K
            config_params.update({
                "tx_no_chain": True,  # Better performance for GPM_SL
                "pacing": "linear",   # Linear pacing works well with GPM_SL
            })
        else:  # GPM mode
            config_params.update({
                "tx_no_chain": False,  # Keep chain for regular GPM
                "pacing": "wide",      # Wide pacing for GPM with 4K
            })
    else:  # HD content
        if packing == "GPM_SL":
            config_params["pacing"] = "linear"
        else:
            config_params["pacing"] = "narrow"  # Narrow pacing for HD GPM

    app.create_command(**config_params)

    # Execute test using Application class
    # Use longer test time for 4K content and accurate packing validation
    actual_test_time = test_time
    if media_file_info.get("height", 0) >= 2160:  # 4K content
        actual_test_time = max(test_time, 12)  # Minimum 12 seconds for 4K packing accuracy
    else:
        actual_test_time = max(test_time, 8)   # Minimum 8 seconds for HD packing accuracy

    app.execute_test(
        build=build,
        test_time=actual_test_time,
        host=host,
        capture_cfg=capture_cfg,
    )
