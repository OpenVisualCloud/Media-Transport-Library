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
from tests.xfail import SDBQ1002_pg_format_error_check

pg_formats = [
    "YUV_422_8bit",
    "YUV_422_10bit",
    "YUV_422_12bit",
    "YUV_422_16bit",
    "YUV_420_8bit",
    "YUV_420_10bit",
    "YUV_420_12bit",
    "YUV_420_16bit",
    "YUV_444_8bit",
    "YUV_444_10bit",
    "YUV_444_12bit",
    "YUV_444_16bit",
    "RGB_8bit",
    "RGB_10bit",
    "RGB_12bit",
    "RGB_16bit",
    "YUV_422_PLANAR10LE",
    "V210",
]


@pytest.mark.parametrize("pg_format", pg_formats)
@pytest.mark.parametrize("video_format", ["i720p50", "i1080p30", "i2160p60"])
def test_pg_format(build, media, nic_port_list, test_time, video_format, pg_format, request):
    SDBQ1002_pg_format_error_check(video_format, pg_format, request)

    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_video_sessions(
        config=config,
        nic_port_list=nic_port_list,
        test_mode="unicast",
        type_="frame",
        video_format=video_format,
        pg_format=pg_format,
        video_url=os.path.join(media, "test_8k.yuv"),
    )
    config = rxtxapp.change_packing_video(content=config, packing="GPM")

    rxtxapp.execute_test(config=config, build=build, test_time=test_time)
