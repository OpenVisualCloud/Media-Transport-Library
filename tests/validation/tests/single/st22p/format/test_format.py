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
from tests.Engine.media_files import yuv_files_422p10le


@pytest.mark.parametrize("format", yuv_files_422p10le.keys())
def test_format(build, media, nic_port_list, test_time, format):
    st22p_file = yuv_files_422p10le[format]

    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_st22p_sessions(
        config=config,
        nic_port_list=nic_port_list,
        test_mode="multicast",
        width=st22p_file["width"],
        height=st22p_file["height"],
        fps="p25",
        codec="JPEG-XS",
        quality="speed",
        input_format=st22p_file["file_format"],
        output_format=st22p_file["file_format"],
        st22p_url=os.path.join(media, st22p_file["filename"]),
        codec_thread_count=2,
    )

    rxtxapp.execute_test(config=config, build=build, test_time=test_time)
