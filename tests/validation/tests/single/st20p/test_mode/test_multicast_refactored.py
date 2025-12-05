# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

import pytest
from mtl_engine.app_refactored import Application
from mtl_engine.media_files import yuv_files_422rfc10


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
def test_multicast_refactored(
    hosts,
    build,
    media,
    nic_port_list,
    test_time,
    test_config,
    prepare_ramdisk,
    media_file,
):
    """
    Test multicast transmission mode using Application class refactored interface
    """
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]

    # Get capture configuration from test_config.yaml
    # This controls whether tcpdump capture is enabled, where to store the pcap, etc.
    capture_cfg = dict(test_config.get("capture_cfg", {}))
    capture_cfg["test_name"] = (
        f"test_multicast_refactored_{media_file_info['filename']}"  # Set a unique pcap file name
    )

    # Create Application instance for RxTxApp
    app = Application("RxTxApp", f"{build}/tests/tools/RxTxApp/build")
    
    # Configure application using universal parameters
    config_params = {
        "session_type": "st20p",
        "nic_port": host.vfs[0] if host.vfs else "0000:31:01.0",
        "nic_port_list": host.vfs,
        "destination_ip": "239.168.48.9",  # Multicast destination
        "port": 20000,
        "width": media_file_info["width"],
        "height": media_file_info["height"],
        "framerate": f"p{media_file_info['fps']}",
        "pixel_format": media_file_info["file_format"],
        "transport_format": media_file_info["format"],
        "pixel_format_rx": media_file_info["file_format"],
        "input_file": media_file_path,
        "test_mode": "multicast",
    }
    
    # Add multicast-specific optimizations based on resolution
    height = media_file_info.get("height", 0)
    
    if height >= 2160:  # 4K content
        config_params.update({
            "pacing": "linear",     # Linear pacing for 4K multicast
            "packing": "GPM_SL",    # Single line packing for efficiency
            "tx_no_chain": True,    # Optimize for 4K multicast performance
        })
    elif height >= 1080:  # Full HD content
        config_params.update({
            "pacing": "wide",       # Wide pacing for 1080p multicast
            "packing": "GPM",       # Standard GPM packing
            "tx_no_chain": False,   # Keep chain for 1080p multicast
        })
    else:  # HD 720p and below
        config_params.update({
            "pacing": "narrow",     # Narrow pacing for lower resolutions
            "packing": "GPM",       # Standard GPM packing
            "tx_no_chain": False,   # Keep chain for lower resolutions
        })

    app.create_command(**config_params)

    # Execute test using Application class
    # Use adaptive test time based on resolution for accurate multicast measurement
    actual_test_time = test_time
    if height >= 2160:  # 4K content
        actual_test_time = max(test_time, 15)  # Minimum 15 seconds for 4K multicast accuracy
    elif height >= 1080:  # Full HD content
        actual_test_time = max(test_time, 10)  # Minimum 10 seconds for 1080p multicast accuracy
    else:  # HD 720p and below
        actual_test_time = max(test_time, 8)   # Minimum 8 seconds for HD multicast accuracy

    app.execute_test(
        build=build,
        test_time=actual_test_time,
        host=host,
        capture_cfg=capture_cfg,
    )
