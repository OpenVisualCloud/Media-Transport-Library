# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation
import os

import mtl_engine.RxTxApp as rxtxapp
import pytest
from mtl_engine.media_files import yuv_files_interlace


@pytest.mark.parametrize("file", yuv_files_interlace.keys())
def test_interlace(
    hosts, build, media, nic_port_list, test_time, file, test_config, prepare_ramdisk
):
    st20p_file = yuv_files_interlace[file]
    host = list(hosts.values())[0]

    # Get capture configuration from test_config.yaml
    # This controls whether tcpdump capture is enabled, where to store the pcap, etc.
    # capture_time: 15
    capture_cfg = dict(test_config.get("capture_cfg", {}))
    capture_cfg["test_name"] = f"test_interlace_{file}"  # Set a unique pcap file name

    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_st20p_sessions(
        config=config,
        nic_port_list=host.vfs,
        test_mode="unicast",
        width=st20p_file["width"],
        height=st20p_file["height"],
        fps=f"p{st20p_file['fps']}",
        input_format=st20p_file["file_format"],
        transport_format=st20p_file["format"],
        output_format=st20p_file["file_format"],
        st20p_url=os.path.join(media, st20p_file["filename"]),
        interlaced=True,
    )

    rxtxapp.execute_test(
        config=config,
        build=build,
        test_time=test_time,
        host=host,
        capture_cfg=capture_cfg,
    )
