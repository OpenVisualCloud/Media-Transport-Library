# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation
import os

import mtl_engine.RxTxApp as rxtxapp
import pytest
from mtl_engine.media_files import st41_files

payload_type_mapping = {
    "pt115": 115,
    "pt120": 120,
}

dit_mapping = {
    "dit0": 3648364,
    "dit1": 1234567,
}

k_bit_mapping = {
    "k0": 0,
    "k1": 1,
}


@pytest.mark.parametrize(
    "media_file",
    [st41_files["st41_p29_long_file"]],
    indirect=["media_file"],
    ids=["st41_p29_long_file"],
)
@pytest.mark.parametrize("dit", ["dit0", "dit1"])
def test_dit(
    hosts,
    build,
    media,
    nic_port_list,
    test_time,
    dit,
    test_config,
    prepare_ramdisk,
    media_file,
):
    """
    Test the Data Item Type (DIT) fastmetadata_data_item_type
    functionality to ensure it is not hardcoded and can handle different values.
    """
    media_file_info, media_file_path = media_file
    payload_type = payload_type_mapping["pt115"]
    type_mode = "rtp"
    k_bit = k_bit_mapping["k0"]

    # Get capture configuration from test_config.yaml
    # This controls whether tcpdump capture is enabled, where to store the pcap, etc.
    capture_cfg = dict(test_config.get("capture_cfg", {}))
    capture_cfg["test_name"] = f"test_dit_{dit}"

    host = list(hosts.values())[0]

    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_st41_sessions(
        config=config,
        no_chain=False,
        nic_port_list=host.vfs,
        test_mode="unicast",
        payload_type=payload_type,
        type_=type_mode,
        fastmetadata_data_item_type=dit_mapping[dit],
        fastmetadata_k_bit=k_bit,
        fastmetadata_fps="p59",
        fastmetadata_url=media_file_path,
    )

    rxtxapp.execute_test(
        config=config,
        build=build,
        test_time=test_time,
        host=host,
        capture_cfg=capture_cfg,
    )
