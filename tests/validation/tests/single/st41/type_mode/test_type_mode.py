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


@pytest.mark.parametrize("test_mode", ["unicast", "multicast"])
@pytest.mark.parametrize("type_mode", ["rtp", "frame"])
def test_type_mode(
    hosts,
    build,
    media,
    nic_port_list,
    test_time,
    test_mode,
    type_mode,
    test_config,
    prepare_ramdisk,
):
    """
    Test the functionality of different transmission modes (unicast, multicast)
    and data types (RTP, frame) to ensure proper handling of long files and frame splitting.
    """
    st41_file = st41_files["st41_p29_long_file"]["filename"]
    payload_type = payload_type_mapping["pt115"]
    k_bit = k_bit_mapping["k0"]
    dit = dit_mapping["dit0"]

    # Get capture configuration from test_config.yaml
    # Collect packet capture configuration and assign test_name
    capture_cfg = dict(test_config.get("capture_cfg", {}))
    capture_cfg["test_name"] = f"test_type_mode_{test_mode}_{type_mode}"

    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_st41_sessions(
        config=config,
        no_chain=False,
        nic_port_list=nic_port_list,
        test_mode=test_mode,
        payload_type=payload_type,
        type_=type_mode,
        fastmetadata_data_item_type=dit,
        fastmetadata_k_bit=k_bit,
        fastmetadata_fps="p59",
        fastmetadata_url=os.path.join(media, st41_file),
    )
    host = list(hosts.values())[0]

    rxtxapp.execute_test(
        config=config,
        build=build,
        test_time=test_time,
        host=host,
        capture_cfg=capture_cfg,
    )
