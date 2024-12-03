# INTEL CONFIDENTIAL
# Copyright 2024-2024 Intel Corporation.
#
# This software and the related documents are Intel copyrighted materials, and your use of them is governed
# by the express license under which they were provided to you ("License"). Unless the License provides otherwise,
# you may not use, modify, copy, publish, distribute, disclose or transmit this software or the related documents
# without Intel's prior written permission.
#
# This software and the related documents are provided as is, with no express or implied warranties,
# other than those that are expressly stated in the License.
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

@pytest.mark.parametrize("payload_type", ["pt115", "pt120"])
@pytest.mark.parametrize("type_mode", ["rtp", "frame"])
def test_payload_type(build, media, nic_port_list, test_time, payload_type, type_mode):
    """
    Test the functionality of different payload types payload_type (115, 120) 
    in both transmission modes (RTP, frame) to ensure proper handling.
    """
    st41_file = st41_files["st41_p29_long_file"]["filename"]
    dit = dit_mapping["dit0"]
    k_bit = k_bit_mapping["k0"]

    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_st41_sessions(
        config=config,
        no_chain=False,
        nic_port_list=nic_port_list,
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
        test_time=test_time
    )

