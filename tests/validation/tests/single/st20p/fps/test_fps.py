# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation
import os

import mtl_engine.RxTxApp as rxtxapp
import pytest
from mtl_engine.media_files import yuv_files_422rfc10


@pytest.mark.parametrize(
    "fps",
    ["p23", "p24", "p25", pytest.param("p29", marks=pytest.mark.smoke), "p30", "p50", "p59", "p60", "p100", "p119", "p120"],
)
@pytest.mark.parametrize("file", ["ParkJoy_1080p"])
def test_fps(
    hosts,
    build,
    media,
    nic_port_list,
    test_time,
    file,
    fps,
    test_config,
    prepare_ramdisk,
):
    st20p_file = yuv_files_422rfc10[file]
    host = list(hosts.values())[0]

    # Get capture configuration from test_config.yaml
    # This controls whether tcpdump capture is enabled, where to store the pcap, etc.
    capture_cfg = dict(test_config.get("capture_cfg", {}))
    capture_cfg["test_name"] = f"test_fps_{file}_{fps}"  # Set a unique pcap file name

    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_st20p_sessions(
        config=config,
        nic_port_list=host.vfs,
        test_mode="unicast",
        width=st20p_file["width"],
        height=st20p_file["height"],
        fps=fps,
        input_format=st20p_file["file_format"],
        transport_format=st20p_file["format"],
        output_format=st20p_file["file_format"],
        st20p_url=os.path.join(media, st20p_file["filename"]),
    )

    rxtxapp.execute_test(
        config=config,
        build=build,
        test_time=test_time,
        host=host,
        capture_cfg=capture_cfg,
    )
