# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

import pytest
from common.nicctl import InterfaceSetup
from mtl_engine import ip_pools
from mtl_engine.media_files import yuv_files_interlace

pytestmark = pytest.mark.verified


@pytest.mark.nightly
@pytest.mark.parametrize(
    "application",
    [
        "rxtxapp",
        pytest.param(
            "ffmpeg",
            marks=pytest.mark.skip(
                reason="FFmpeg does not support interlaced mode"
            ),
        ),
    ],
)
@pytest.mark.parametrize(
    "media_file",
    list(yuv_files_interlace.values()),
    indirect=["media_file"],
    ids=list(yuv_files_interlace.keys()),
)
def test_st20p_interlace(
    application,
    app_factory,
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_config,
    test_time,
    pcap_capture,
    media_file,
):
    """Test interlaced video transmission."""
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )

    config_params = {
        "session_type": "st20p",
        "nic_port_list": interfaces_list,
        "source_ip": ip_pools.tx[0],
        "destination_ip": ip_pools.rx[0],
        "port": 20000,
        "width": media_file_info["width"],
        "height": media_file_info["height"],
        "framerate": f"p{media_file_info['fps']}",
        "pixel_format": media_file_info["file_format"],
        "transport_format": media_file_info["format"],
        "input_file": media_file_path,
        "test_mode": "unicast",
        "interlaced": True,
        "pacing": "linear",
        "tx_no_chain": False,
        "test_time": test_time,
    }

    app = app_factory(application)
    app.create_command(**config_params)
    actual_test_time = max(test_time, 10)
    app.execute_test(
        build=mtl_path, test_time=actual_test_time, host=host, netsniff=pcap_capture
    )
