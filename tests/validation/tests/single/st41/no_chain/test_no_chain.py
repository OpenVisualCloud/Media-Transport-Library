# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation
import os

import pytest
import tests.Engine.RxTxApp as rxtxapp
from tests.Engine.media_files import st41_files

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


@pytest.mark.parametrize("type_mode", ["rtp", "frame"])
def test_no_chain(
    build,
    media,
    nic_port_list,
    test_time,
    type_mode,
):
    """
    Test the functionality with the tx_no_chain configuration set to True
    to ensure proper handling of unchained sessions for type modes rtp and frame.
    """
    payload_type = payload_type_mapping["pt115"]
    k_bit = k_bit_mapping["k0"]
    dit = dit_mapping["dit0"]
    st41_file = st41_files["st41_p29_long_file"]["filename"]

    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_st41_sessions(
        config=config,
        no_chain=True,
        nic_port_list=nic_port_list,
        test_mode="unicast",
        payload_type=payload_type,
        type_=type_mode,
        fastmetadata_data_item_type=dit,
        fastmetadata_k_bit=k_bit,
        fastmetadata_fps="p59",
        fastmetadata_url=os.path.join(media, st41_file),
    )

    rxtxapp.execute_test(config=config, build=build, test_time=test_time)
