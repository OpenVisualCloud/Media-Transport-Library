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


@pytest.mark.parametrize("test_mode", ["multicast"])
@pytest.mark.parametrize("file", ["Penguin_1080p"])
@pytest.mark.parametrize("replicas", [1, 4])
def test_kernello_st20p_video_format(
    build,
    media,
    test_time,
    test_mode,
    file,
    replicas
):
    st20p_file = yuv_files_422p10le[file]

    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_st20p_sessions(
        config=config,
        nic_port_list=["kernel:lo", "kernel:lo"],
        test_mode=test_mode,
        width=st20p_file["width"],
        height=st20p_file["height"],
        fps="p30",
        input_format=st20p_file["file_format"],
        transport_format=st20p_file["format"],
        output_format=st20p_file["file_format"],
        st20p_url=os.path.join(media, st20p_file["filename"]),
    )
    config = rxtxapp.change_replicas(config=config, session_type="st20p", replicas=replicas)
    rxtxapp.execute_test(config=config, build=build, test_time=test_time*replicas)
