# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation
import os

import mtl_engine.RxTxApp as rxtxapp
import pytest
from common.nicctl import InterfaceSetup
from mtl_engine.media_files import yuv_files


@pytest.mark.nightly
@pytest.mark.xfail(reason="the test environment is not yet ready to run PTP tests")
@pytest.mark.parametrize("test_mode", ["unicast", "multicast"])
@pytest.mark.parametrize(
    "video_format",
    ["i1080p30", "i1080p50", "i1080p59", "i2160p30", "i2160p50", "i2160p59"],
)
@pytest.mark.parametrize("replicas", [1, 3, 7, 10, 14, 18])
def test_ptp_video_format(
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
    if "i2160" in video_format and replicas > 3:
        pytest.skip("Skipping 4k tests with more than 3 replicas")
    video_file = yuv_files[video_format]
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )

    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_st20p_sessions(
        config=config,
        nic_port_list=interfaces_list,
        test_mode=test_mode,
        width=video_file["width"],
        height=video_file["height"],
        fps=f"p{video_file['fps']}",
        transport_format=video_file["format"],
        output_format=video_file["file_format"],
        st20p_url=os.path.join(media, video_file["filename"]),
        input_format=video_file["file_format"],
    )
    config = rxtxapp.change_replicas(
        config=config, session_type="st20p", replicas=replicas
    )

    rxtxapp.execute_test(
        config=config,
        build=build,
        test_time=test_time,
        ptp=True,
        host=host,
    )
