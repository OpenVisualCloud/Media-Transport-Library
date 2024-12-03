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
from tests.Engine.media_files import (
    yuv_files,
    yuv_files_422rfc10,
)


@pytest.mark.parametrize("standard_mode", ["st20p", "st22p"])
@pytest.mark.parametrize("test_mode", ["multicast"])
@pytest.mark.parametrize("video_format", ["i1080p59"])
@pytest.mark.parametrize("replicas", [1, 2])
def test_xdp_standard(
    build, media, test_time, test_mode, video_format, replicas, standard_mode
):
    video_file = yuv_files[video_format]

    st22p_file = yuv_files_422rfc10["Crosswalk_1080p"]

    config = rxtxapp.create_empty_config()
    if standard_mode == "st20p":
        config = rxtxapp.add_st20p_sessions(
            config=config,
            nic_port_list=["native_af_xdp:eth2", "native_af_xdp:eth3"],
            test_mode=test_mode,
            type_="frame",
            video_format=video_format,
            pg_format=video_file["format"],
            video_url=os.path.join(media, video_file["filename"]),
        )
        config = rxtxapp.change_replicas(
            config=config, session_type="st20p", replicas=replicas
        )
    elif standard_mode == "st22p":
        config = rxtxapp.add_st22p_sessions(
            config=config,
            nic_port_list=["native_af_xdp:eth2", "native_af_xdp:eth3"],
            test_mode=test_mode,
            width=st22p_file["width"],
            height=st22p_file["height"],
            fps="p30",
            codec="JPEG-XS",
            quality="speed",
            pack_type="codestream",
            input_format=st22p_file["file_format"],
            output_format=st22p_file["file_format"],
            codec_thread_count=2,
            st22p_url=os.path.join(media, st22p_file["filename"]),
        )
        config = rxtxapp.change_replicas(
            config=config, session_type="st22p", replicas=replicas
        )
    rxtxapp.execute_test(config=config, build=build, test_time=test_time)
