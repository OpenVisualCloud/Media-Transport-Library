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
    "payload_type,type_mode",
    [
        pytest.param("pt115", "rtp", marks=pytest.mark.smoke),
        ("pt115", "frame"),
        ("pt120", "rtp"),
        ("pt120", "frame"),
    ],
)
def test_payload_type(
    hosts,
    build,
    media,
    nic_port_list,
    test_time,
    payload_type,
    type_mode,
    test_config,
    prepare_ramdisk,
):
    """
    Test the functionality of different payload types payload_type (115, 120)
    in both transmission modes (RTP, frame) to ensure proper handling.
    """
    st41_file = st41_files["st41_p29_long_file"]["filename"]
    dit = dit_mapping["dit0"]
    k_bit = k_bit_mapping["k0"]

    # Get capture configuration from test_config.yaml
    # Collect packet capture configuration and assign test_name
    capture_cfg = dict(test_config.get("capture_cfg", {}))
    capture_cfg["test_name"] = f"test_payload_type_{payload_type}_{type_mode}"

    host = list(hosts.values())[0]

    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_st41_sessions(
        config=config,
        no_chain=False,
        nic_port_list=host.vfs,
        test_mode="unicast",
        payload_type=payload_type_mapping[payload_type],
        type_=type_mode,
        fastmetadata_data_item_type=dit,
        fastmetadata_k_bit=k_bit,
        fastmetadata_fps="p59",
        fastmetadata_url=os.path.join(media, st41_file),
    )

    rxtxapp.execute_test(
        config=config,
        build=build,
        test_time=test_time,
        host=host,
        capture_cfg=capture_cfg,
    )
