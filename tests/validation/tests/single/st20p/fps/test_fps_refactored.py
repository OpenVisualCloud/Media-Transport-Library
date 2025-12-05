# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

import pytest
from mtl_engine.app_refactored import Application
from mtl_engine.media_files import yuv_files_422rfc10


# Define a custom fixture to provide test configuration without external config file dependency
@pytest.fixture(scope="session")
def default_test_config():
    """Provide default test configuration to avoid config file dependency issues."""
    return {
        "build": "/root/awilczyn/Media-Transport-Library/build",
        "mtl_path": "/root/awilczyn/Media-Transport-Library/build", 
        "media_path": "/mnt/media",
        "test_time": 5,
        "delay_between_tests": 1,
        "capture_cfg": {
            "enable": False,
            "test_name": "test_name",
            "pcap_dir": "/tmp/pcap",
            "capture_time": 5,
            "interface": None
        },
        "ramdisk": {
            "media": {
                "mountpoint": "/tmp/media",
                "size_gib": 1
            },
            "pcap": {
                "mountpoint": "/tmp/pcap", 
                "size_gib": 1
            }
        },
        "compliance": False
    }


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
    default_test_config,  # Use our custom fixture instead of the problematic test_config
):
    """
    Test different frame rates using Application class refactored interface
    """
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]

    # Use the default test configuration from our custom fixture
    capture_cfg = dict(default_test_config.get("capture_cfg", {}))
    capture_cfg["test_name"] = f"test_fps_refactored_{media_file_info['filename']}_{fps}"
    capture_cfg["enabled"] = False  # Disable tcpdump capture to avoid complexity

    # Create Application instance for RxTxApp
    app = Application("RxTxApp", f"{build}/tests/tools/RxTxApp/build")
    
    # Configure application using universal parameters
    # Match the original test configuration exactly - use multicast mode for proper TX/RX setup
    config_params = {
        "session_type": "st20p",
        "nic_port_list": host.vfs,
        "test_mode": "multicast",  # Use multicast like the working original test
        "destination_ip": "239.168.48.9",  # Multicast destination IP
        "port": 20000,
        "width": media_file_info["width"],
        "height": media_file_info["height"],
        "framerate": fps,
        "pixel_format": media_file_info["file_format"],
        "transport_format": media_file_info["format"],
        "pixel_format_rx": media_file_info["file_format"],
        "input_file": media_file_path,
    }
    
    # Add performance optimizations for frame rates that need more stability
    if fps in ["p30", "p50", "p59", "p60"]:
        config_params.update({
            "pacing": "gap",  # Use gap pacing for better stability
            "tx_no_chain": True,  # Optimize for performance
        })
    elif fps in ["p100", "p119", "p120"]:
        config_params.update({
            "pacing": "linear",  # Better pacing for high frame rates
            "tx_no_chain": True,  # Optimize for performance
        })

    app.create_command(**config_params)

    # Execute test using Application class
    # Use longer test time for accurate FPS measurement and stability
    actual_test_time = test_time
    if fps in ["p30", "p50", "p59", "p60"]:
        actual_test_time = max(test_time, 15)  # Minimum 15 seconds for stability
    elif fps in ["p100", "p119", "p120"]:
        actual_test_time = max(test_time, 10)  # Minimum 10 seconds for high FPS accuracy

    app.execute_test(
        build=build,
        test_time=actual_test_time,
        host=host,
        capture_cfg=capture_cfg,
    )
