# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

import pytest
from common.nicctl import InterfaceSetup
from mtl_engine import ip_pools
from mtl_engine.media_files import yuv_files_422p10le

pytestmark = pytest.mark.verified


@pytest.mark.nightly
@pytest.mark.parametrize("application", ["rxtxapp", "ffmpeg", "gstreamer"])
@pytest.mark.parametrize(
    "media_file",
    [yuv_files_422p10le["Penguin_1080p"]],
    indirect=["media_file"],
    ids=["Penguin_1080p"],
)
@pytest.mark.parametrize(
    "fps",
    [
        "p23",
        "p24",
        "p25",
        # 1080p at 29.97 fps is ~1.3 Gbps on the wire, the highest-rate
        # uncompressed video case that still fits a 2.5 GbE NIC.
        pytest.param("p29", marks=[pytest.mark.smoke, pytest.mark.low_bandwidth]),
        "p30",
        "p50",
        "p59",
        "p60",
        "p100",
        "p119",
        "p120",
    ],
)
def test_st20p_fps(
    application,
    app_factory,
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_time,
    test_config,
    fps,
    pcap_capture,
    output_files,
    media_integrity,
    media_file,
):
    """Test different frame rates for st20p with both applications."""
    media_file_info, media_file_path = media_file
    rx_output = output_files.register(f"{media_file_path}.out")
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )

    config_params = {
        "session_type": "st20p",
        "nic_port_list": interfaces_list,
        "test_mode": "multicast",
        "destination_ip": ip_pools.rx_multicast[0],
        "port": 20000,
        "width": media_file_info["width"],
        "height": media_file_info["height"],
        "framerate": fps,
        "pixel_format": media_file_info["file_format"],
        "transport_format": media_file_info["format"],
        "input_file": media_file_path,
        "output_file": rx_output,
        "test_time": test_time,
    }

    app = app_factory(
        application,
        session_type="st20p",
        pixel_format=media_file_info["file_format"],
        transport_format=media_file_info["format"],
    )
    app.create_command(**config_params)

    actual_test_time = test_time
    if fps in ["p30", "p50", "p59", "p60"]:
        actual_test_time = max(test_time, 15)
    elif fps in ["p100", "p119", "p120"]:
        actual_test_time = max(test_time, 10)

    app.execute_test(
        build=mtl_path,
        test_time=actual_test_time,
        host=host,
        compliance=pcap_capture,
        integrity=media_integrity,
    )
