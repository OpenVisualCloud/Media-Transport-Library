# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

import pytest
from common.nicctl import InterfaceSetup
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


@pytest.mark.nightly
@pytest.mark.parametrize(
    "media_file",
    [st41_files["st41_p29_long_file"]],
    indirect=["media_file"],
    ids=["st41_p29_long_file"],
)
@pytest.mark.refactored
@pytest.mark.parametrize("dit", ["dit0", "dit1"])
def test_dit_refactored(
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_time,
    dit,
    test_config,
    prepare_ramdisk,
    media_file,
    rxtxapp,
):
    """
    Test the Data Item Type (DIT) fastmetadata_data_item_type
    functionality to ensure it is not hardcoded and can handle different values.
    """
    media_file_info, media_file_path = media_file
    payload_type = payload_type_mapping["pt115"]
    k_bit = k_bit_mapping["k0"]

    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )

    rxtxapp.create_command(
        session_type="fastmetadata",
        nic_port_list=interfaces_list,
        test_mode="unicast",
        type_mode="rtp",
        payload_type=payload_type,
        fastmetadata_data_item_type=dit_mapping[dit],
        fastmetadata_k_bit=k_bit,
        fastmetadata_fps="p59",
        input_file=media_file_path,
        test_time=test_time,
    )

    rxtxapp.execute_test(
        build=mtl_path,
        test_time=test_time,
        host=host,
    )
