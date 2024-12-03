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
from tests.Engine.media_files import yuv_files


@pytest.mark.parametrize("type_mode", ["rtp", "slice"])
@pytest.mark.parametrize(
    "video_format",
    ["i720p50", "i1080p30", "i2160p60", "i4320p30", "i480i59", "i576i50", "i1080i59"],
)
def test_type_mode(
    build,
    media,
    nic_port_list,
    test_time,
    video_format,
    type_mode,
):
    video_file = yuv_files[video_format]

    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_video_sessions(
        config=config,
        nic_port_list=nic_port_list,
        test_mode="multicast",
        type_=type_mode,
        video_format=video_format,
        pg_format=video_file["format"],
        video_url=os.path.join(media, video_file["filename"]),
    )

    rxtxapp.execute_test(config=config, build=build, test_time=test_time)
