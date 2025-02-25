# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation
import os

import pytest
import tests.Engine.RxTxApp as rxtxapp
from tests.Engine.media_files import yuv_files_422rfc10


@pytest.mark.parametrize("pacing", ["narrow", "wide", "linear"])
@pytest.mark.parametrize("file", ["Crosswalk_720p", "ParkJoy_1080p", "Pedestrian_4K"])
def test_pacing(build, media, nic_port_list, test_time, file, pacing):
    st20p_file = yuv_files_422rfc10[file]

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
        pacing=pacing,
    )

    rxtxapp.execute_test(config=config, build=build, test_time=test_time)
