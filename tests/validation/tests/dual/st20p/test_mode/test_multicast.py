# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation
import os

import mtl_engine.RxTxApp as rxtxapp
import pytest
from mtl_engine.media_files import yuv_files_422rfc10


@pytest.mark.dual
@pytest.mark.parametrize("file", ["Crosswalk_720p", "ParkJoy_1080p", "Pedestrian_4K"])
def test_multicast_dual(
    hosts,
    build,
    media,
    nic_port_list,
    test_time,
    file,
):
    st20p_file = yuv_files_422rfc10[file]

    # Get TX and RX hosts
    host_list = list(hosts.values())
    if len(host_list) < 2:
        pytest.skip("Dual tests require at least 2 hosts")

    tx_host = host_list[0]
    rx_host = host_list[1]

    config = rxtxapp.create_empty_dual_config()
    config = rxtxapp.add_st20p_dual_sessions(
        config=config,
        tx_nic_port_list=tx_host.vfs,
        rx_nic_port_list=rx_host.vfs,
        test_mode="multicast",
        width=st20p_file["width"],
        height=st20p_file["height"],
        fps=f"p{st20p_file['fps']}",
        input_format=st20p_file["file_format"],
        transport_format=st20p_file["format"],
        output_format=st20p_file["file_format"],
        st20p_url=os.path.join(media, st20p_file["filename"]),
    )

    rxtxapp.execute_dual_test(
        config=config,
        build=build,
        test_time=test_time,
        tx_host=tx_host,
        rx_host=rx_host,
    )
