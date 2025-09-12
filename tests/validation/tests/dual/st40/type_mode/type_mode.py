# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

import os

import mtl_engine.RxTxApp as rxtxapp
import pytest
from mtl_engine.media_files import anc_files


@pytest.mark.dual
@pytest.mark.parametrize("type_mode", ["rtp", "frame"])
@pytest.mark.parametrize("anc_keys", anc_files.keys())
def test_type_mode_dual(
    hosts,
    build,
    media,
    nic_port_list,
    test_time,
    type_mode,
    anc_keys,
):
    ancillary_file = anc_files[anc_keys]

    host_list = list(hosts.values())
    if len(host_list) < 2:
        pytest.skip("Dual tests require at least 2 hosts")

    tx_host = host_list[0]
    rx_host = host_list[1]

    config = rxtxapp.create_empty_dual_config()
    config = rxtxapp.add_st40p_dual_sessions(
        config=config,
        tx_nic_port_list=tx_host.vfs,
        rx_nic_port_list=rx_host.vfs,
        test_mode="unicast",
        type_=type_mode,
        ancillary_format="closed_caption",
        ancillary_fps=ancillary_file["fps"],
        ancillary_url=os.path.join(media, ancillary_file["filename"]),
    )

    rxtxapp.execute_dual_test(
        config=config,
        build=build,
        test_time=test_time,
        tx_host=tx_host,
        rx_host=rx_host,
    )
