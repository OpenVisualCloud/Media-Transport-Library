# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

import pytest
from common.nicctl import InterfaceSetup
from mtl_engine import ip_pools
from mtl_engine.media_files import yuv_files_422p10le, yuv_files_422rfc10

pytestmark = pytest.mark.verified


@pytest.mark.nightly
@pytest.mark.tx_side
@pytest.mark.parametrize(
    ("application", "packing", "media_file"),
    [
        *[
            pytest.param(
                "rxtxapp",
                packing,
                media_file,
                id=f"rxtxapp-{packing}-{name}",
            )
            for packing in ("BPM", "GPM", "GPM_SL")
            for name, media_file in yuv_files_422rfc10.items()
        ],
        pytest.param(
            "ffmpeg",
            "BPM",
            yuv_files_422p10le["Penguin_1080p"],
            id="ffmpeg-BPM-Penguin_1080p",
        ),
        pytest.param(
            "gstreamer",
            "BPM",
            yuv_files_422p10le["Penguin_1080p"],
            id="gstreamer-BPM-Penguin_1080p",
        ),
    ],
    indirect=["media_file"],
)
def test_st20p_packing(
    application,
    app_factory,
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_config,
    test_time,
    packing,
    pcap_capture,
    output_files,
    media_integrity,
    media_file,
):
    """Test each packetizer across representative frame geometry."""
    media_file_info, media_file_path = media_file
    rx_output = output_files.register(f"{media_file_path}.out")
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
        "output_file": rx_output,
        "test_mode": "multicast",
        "packing": packing,
        "test_time": test_time,
    }

    height = media_file_info.get("height", 0)
    if height >= 2160:
        actual_test_time = max(test_time, 12)
    else:
        actual_test_time = max(test_time, 8)

    app = app_factory(
        application,
        session_type="st20p",
        pixel_format=media_file_info["file_format"],
        transport_format=media_file_info["format"],
        packing=packing,
    )
    app.create_command(**config_params)
    app.execute_test(
        build=mtl_path,
        test_time=actual_test_time,
        host=host,
        compliance=pcap_capture,
        integrity=media_integrity,
    )
