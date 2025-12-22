# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

import pytest
from mtl_engine.app_refactored import Application
from mtl_engine.media_files import yuv_files_interlace


@pytest.mark.parametrize(
    "media_file",
    list(yuv_files_interlace.values()),
    indirect=["media_file"],
    ids=list(yuv_files_interlace.keys()),
)
def test_interlace_refactored(
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
    Test interlaced video transmission using Application class refactored interface
    """
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]

    # Get capture configuration from test_config.yaml
    # This controls whether tcpdump capture is enabled, where to store the pcap, etc.
    # capture_time: 15
    capture_cfg = dict(test_config.get("capture_cfg", {}))
    capture_cfg["test_name"] = (
        f"test_interlace_refactored_{media_file_info['filename']}"  # Set a unique pcap file name
    )

    # Create Application instance for RxTxApp
    app = Application("RxTxApp", f"{build}/tests/tools/RxTxApp/build")
    
    # Configure application using universal parameters
    config_params = {
        "session_type": "st20p",
        "nic_port": host.vfs[0] if host.vfs else "0000:31:01.0",
        "nic_port_list": host.vfs,
        "source_ip": "192.168.17.101",      # TX interface IP
        "destination_ip": "192.168.17.102", # RX interface IP  
        "port": 20000,
        "width": media_file_info["width"],
        "height": media_file_info["height"],
        "framerate": f"p{media_file_info['fps']}",
        "pixel_format": media_file_info["file_format"],
        "transport_format": media_file_info["format"],
        "pixel_format_rx": media_file_info["file_format"],
        "input_file": media_file_path,
        "test_mode": "unicast",
        "interlaced": True,  # Enable interlaced mode
    }
    
    # Add interlace-specific optimizations
    config_params.update({
        "pacing": "linear",  # Better for interlaced content
        "tx_no_chain": False,  # Keep chain for better field synchronization
    })

    app.create_command(**config_params)

    # Execute test using Application class
    # Use longer test time for interlaced content to ensure proper field handling
    actual_test_time = max(test_time, 10)  # Minimum 10 seconds for interlaced accuracy

    app.execute_test(
        build=build,
        test_time=actual_test_time,
        host=host,
        capture_cfg=capture_cfg,
    )
