# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

import os

import pytest
import tests.Engine.RxTxApp as rxtxapp
from tests.Engine.execute import log_info
from tests.Engine.integrity import check_st20p_integrity
from tests.Engine.logging import LOG_FOLDER
from tests.Engine.media_files import yuv_files_422rfc10


@pytest.mark.parametrize("file", ["Crosswalk_720p", "ParkJoy_1080p"])
@pytest.mark.parametrize("fps", ["p30", "p60"])
def test_integrity(build, media, nic_port_list, test_time, file, fps):
    st20p_file = yuv_files_422rfc10[file]
    st20p_file_url = os.path.join(media, st20p_file["filename"])

    out_file_url = os.path.join(os.getcwd(), LOG_FOLDER, "latest", "out.yuv")

    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_st20p_sessions(
        config=config,
        nic_port_list=nic_port_list,
        test_mode="unicast",
        height=st20p_file["height"],
        width=st20p_file["width"],
        fps=fps,
        input_format=st20p_file["file_format"],
        transport_format=st20p_file["format"],
        output_format=st20p_file["file_format"],
        st20p_url=st20p_file_url,
        out_url=out_file_url,
    )

    rxtxapp.execute_test(config=config, build=build, test_time=test_time)

    size = f"{st20p_file['width']}x{st20p_file['height']}"
    result = check_st20p_integrity(
        src_url=st20p_file_url, out_url=out_file_url, size=size
    )

    if result:
        log_info("INTEGRITY PASS")
    else:
        log_info("INTEGRITY FAIL")
