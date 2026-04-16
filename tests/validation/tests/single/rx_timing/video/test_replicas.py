# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation
import os

import mtl_engine.RxTxApp as rxtxapp
from common.nicctl import InterfaceSetup
from mtl_engine.media_files import yuv_files


def test_rx_timing_video_replicas(
    hosts,
    mtl_path,
    media,
    setup_interfaces: InterfaceSetup,
    test_time,
    test_config,
    prepare_ramdisk,
):
    video_file = yuv_files["i1080p60"]
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )

    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_st20p_sessions(
        config=config,
        nic_port_list=interfaces_list,
        test_mode="multicast",
        width=video_file["width"],
        height=video_file["height"],
        fps=f"p{video_file['fps']}",
        input_format=video_file["file_format"],
        transport_format=video_file["format"],
        output_format=video_file["file_format"],
        st20p_url=os.path.join(media, video_file["filename"]),
    )
    config = rxtxapp.change_replicas(config=config, session_type="st20p", replicas=2)

    rxtxapp.execute_test(
        config=config,
        build=mtl_path,
        test_time=test_time,
        rx_timing_parser=True,
        host=host,
    )
