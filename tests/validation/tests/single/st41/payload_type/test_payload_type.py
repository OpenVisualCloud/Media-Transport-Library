# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation
import mtl_engine.RxTxApp as rxtxapp
import pytest
from common.nicctl import InterfaceSetup
from mtl_engine.media_files import st41_files

payload_type_mapping = {
    "pt115": "115",
    "pt120": "120",
}

dit_mapping = {
    "dit0": "3648364",
    "dit1": "1234567",
}

k_bit_mapping = {
    "k0": "0",
    "k1": "1",
}


@pytest.mark.nightly
@pytest.mark.parametrize(
    "media_file",
    [st41_files["st41_p29_long_file"]],
    indirect=["media_file"],
    ids=["st41_p29_long_file"],
)
@pytest.mark.parametrize("payload_type", ["pt115", "pt120"])
@pytest.mark.parametrize("type_mode", ["rtp", "frame"])
def test_payload_type(
    hosts,
    build,
    setup_interfaces: InterfaceSetup,
    test_time,
    payload_type,
    type_mode,
    test_config,
    prepare_ramdisk,
    media_file,
):
    """
    Test the functionality of different payload types payload_type (115, 120)
    in both transmission modes (RTP, frame) to ensure proper handling.
    """
    media_file_info, media_file_path = media_file
    dit = dit_mapping["dit0"]
    k_bit = k_bit_mapping["k0"]

    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )

    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_st41_sessions(
        config=config,
        no_chain=False,
        nic_port_list=interfaces_list,
        test_mode="unicast",
        payload_type=payload_type_mapping[payload_type],
        type_=type_mode,
        fastmetadata_data_item_type=dit,
        fastmetadata_k_bit=k_bit,
        fastmetadata_fps="p59",
        fastmetadata_url=media_file_path,
    )

    rxtxapp.execute_test(
        config=config,
        build=build,
        test_time=test_time,
        host=host,
    )
