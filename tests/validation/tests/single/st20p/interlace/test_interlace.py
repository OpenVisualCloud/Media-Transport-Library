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
from tests.Engine.media_files import yuv_files_interlace


@pytest.mark.parametrize("file", yuv_files_interlace.keys())
def test_interlace(build, media, nic_port_list, test_time, file):
    st20p_file = yuv_files_interlace[file]

    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_st20p_sessions(
        config=config,
        nic_port_list=nic_port_list,
        test_mode="unicast",
        width=st20p_file["width"],
        height=st20p_file["height"],
        fps="p30",
        input_format=st20p_file["file_format"],
        transport_format=st20p_file["format"],
        output_format=st20p_file["file_format"],
        st20p_url=os.path.join(media, st20p_file["filename"]),
        interlaced=True,
    )

    rxtxapp.execute_test(config=config, build=build, test_time=test_time)
