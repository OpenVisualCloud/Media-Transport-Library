# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation
import os

import pytest
import mtl_engine.RxTxApp as rxtxapp
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
    "fps",
    ["p23", "p24", "p25", "p29", "p30", "p50", "p59", "p60", "p100", "p119", "p120"],
)
def test_fps(
    hosts,
    build,
    media,
    nic_port_list,
    test_time,
    fps,
    test_config,
    prepare_ramdisk,
):
    """
    Test the functionality of different frame rates (fps) fastmetadata_fps to ensure the system handles various
    frame rates correctly.
    """
    st41_file = st41_files["st41_p29_long_file"]["filename"]
    payload_type = payload_type_mapping["pt115"]
    type_mode = "rtp"
    dit = dit_mapping["dit0"]
    k_bit = k_bit_mapping["k0"]

    # Get capture configuration from test_config.yaml
    # This controls whether tcpdump capture is enabled, where to store the pcap, etc.
    capture_cfg = dict(test_config.get("capture_cfg", {}))
    capture_cfg["test_name"] = f"test_fps_st41_{fps}"

    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_st41_sessions(
        config=config,
        no_chain=False,
        nic_port_list=nic_port_list,
        test_mode="unicast",
        payload_type=payload_type,
        type_=type_mode,
        fastmetadata_data_item_type=dit,
        fastmetadata_k_bit=k_bit,
        fastmetadata_fps=fps,
        fastmetadata_url=os.path.join(media, st41_file),
    )
    host = list(hosts.values())[0]

    rxtxapp.execute_test(config=config, build=build, test_time=test_time, host=host, capture_cfg=capture_cfg)
