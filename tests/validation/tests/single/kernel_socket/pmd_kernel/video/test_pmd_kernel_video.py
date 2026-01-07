# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation
import os

import mtl_engine.RxTxApp as rxtxapp
import pytest
from common.nicctl import InterfaceSetup
from mtl_engine.media_files import parse_fps_to_pformat, yuv_files


@pytest.mark.parametrize("test_mode", ["multicast"])
@pytest.mark.parametrize("video_format", ["i1080p59", "i2160p59"])
@pytest.mark.parametrize("replicas", [1, 2])
def test_pmd_kernel_video_format(
    hosts,
    build,
    media,
    setup_interfaces: InterfaceSetup,
    test_time,
    test_mode,
    video_format,
    replicas,
    test_config,
    prepare_ramdisk,
):

    video_file = yuv_files[video_format]

    # rxtxapp.check_and_bind_interface(["0000:38:00.0","0000:38:00.1"], "pmd")
    host = list(hosts.values())[0]

    # Get hybrid interface list: one DPDK (VF/PF) and one kernel socket
    interfaces_list = setup_interfaces.get_pmd_kernel_interfaces(
        test_config.get("interface_type", "VF")
    )

    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_st20p_sessions(
        config=config,
        nic_port_list=interfaces_list,
        test_mode=test_mode,
        width=video_file["width"],
        height=video_file["height"],
        fps=parse_fps_to_pformat(video_file["fps"]),
        input_format=video_file["file_format"],
        transport_format=video_file["format"],
        output_format=video_file["file_format"],
        st20p_url=os.path.join(media, video_file["filename"]),
    )
    # rxtxapp.check_and_set_ip('eth2')
    config = rxtxapp.change_replicas(
        config=config, session_type="st20p", replicas=replicas
    )
    rxtxapp.execute_test(
        config=config,
        build=build,
        test_time=test_time * 2,
        host=host,
    )
