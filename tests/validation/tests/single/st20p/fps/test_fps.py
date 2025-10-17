# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

import mtl_engine.RxTxApp as rxtxapp
import pytest
from mtl_engine.media_files import yuv_files_422rfc10


@pytest.mark.nightly
@pytest.mark.parametrize(
    "media_file",
    [yuv_files_422rfc10["ParkJoy_1080p"]],
    indirect=["media_file"],
    ids=["ParkJoy_1080p"],
)
@pytest.mark.parametrize(
    "fps",
    [
        "p23",
        "p24",
        "p25",
        pytest.param("p29", marks=pytest.mark.smoke),
        "p30",
        "p50",
        "p59",
        "p60",
        "p100",
        "p119",
        "p120",
    ],
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
    media_file,
):
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]

    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_st20p_sessions(
        config=config,
        nic_port_list=host.vfs,
        test_mode="unicast",
        width=media_file_info["width"],
        height=media_file_info["height"],
        fps=fps,
        input_format=media_file_info["file_format"],
        transport_format=media_file_info["format"],
        output_format=media_file_info["file_format"],
        st20p_url=media_file_path,
    )

    rxtxapp.execute_test(
        config=config,
        build=build,
        test_time=test_time,
        host=host,
    )
