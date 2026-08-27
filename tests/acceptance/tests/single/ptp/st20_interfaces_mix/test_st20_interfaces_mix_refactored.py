# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation
"""PTP + interface-mix ST20P test using unified app_factory pattern."""

import pytest
from common.nicctl import InterfaceSetup
from mtl_engine.media_files import yuv_files_422rfc10


@pytest.mark.xfail
@pytest.mark.ptp
@pytest.mark.parametrize(
    "interface_profile",
    [
        pytest.param({"mode": "vf_only"}, id="vf_only"),
        pytest.param(
            {"mode": "mixed", "tx_type": "PF", "rx_type": "VF"},
            id="pf_tx_vf_rx",
        ),
    ],
)
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
@pytest.mark.refactored
def test_st20_interfaces_mix_refactored(
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_time,
    interface_profile,
    test_config,
    pcap_capture,
    media_file,
    output_files,
    app_factory,
    media_integrity,
):
    """Test st20p streaming with PTP across different interface profiles."""
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]
    # PTP sync (+10s in RxTxApp), pcap capture, and large RX file caps
    # collectively need more headroom than the 60s default.
    test_time = max(test_time, 90)

    if interface_profile["mode"] == "vf_only":
        interfaces_list = setup_interfaces.get_interfaces_list_single("VF")
    else:
        tx_index = test_config.get("tx_interface_index", 0)
        rx_index = test_config.get("rx_interface_index", 1)
        interfaces_list = setup_interfaces.get_mixed_interfaces_list_single(
            tx_interface_type=interface_profile["tx_type"],
            rx_interface_type=interface_profile["rx_type"],
            tx_index=tx_index,
            rx_index=rx_index,
        )

    video_out_url = output_files.register(
        str(
            host.connection.path(media_file_path).parent
            / f"{media_file_info['filename']}.out"
        )
    )

    app = app_factory("rxtxapp")
    app.create_command(
        session_type="st20p",
        nic_port_list=interfaces_list,
        test_mode="multicast",
        width=media_file_info["width"],
        height=media_file_info["height"],
        framerate=f"p{media_file_info['fps']}",
        pixel_format=media_file_info["file_format"],
        transport_format=media_file_info["format"],
        input_file=media_file_path,
        output_file=video_out_url,
        enable_ptp=True,
        rx_max_file_size=5 * 1024 * 1024 * 1024,  # 5 GB cap
        test_time=test_time,
    )

    app.execute_test(
        build=mtl_path,
        test_time=test_time,
        host=host,
        compliance=pcap_capture,
        integrity=media_integrity,
    )
