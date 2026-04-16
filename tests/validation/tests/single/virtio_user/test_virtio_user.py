# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation
import mtl_engine.RxTxApp as rxtxapp
import pytest
from common.nicctl import InterfaceSetup
from mtl_engine.media_files import yuv_files


@pytest.mark.nightly
@pytest.mark.parametrize(
    "media_file, replicas",
    [
        pytest.param(yuv_files["i1080p60"], 1, marks=pytest.mark.nightly),
        (yuv_files["i1080p60"], 3),
        (yuv_files["i1080p60"], 30),
        pytest.param(yuv_files["i2160p60"], 1, marks=pytest.mark.nightly),
        (yuv_files["i2160p60"], 3),
        (yuv_files["i2160p60"], 9),
    ],
    indirect=["media_file"],
    ids=[
        "i1080p60_1",
        "i1080p60_3",
        "i1080p60_10",
        "i2160p60_1",
        "i2160p60_3",
        "i2160p60_10",
    ],
)
def test_virtio_user(
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_time,
    replicas,
    test_config,
    prepare_ramdisk,
    media_file,
):
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )

    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_st20p_sessions(
        config=config,
        nic_port_list=interfaces_list,
        test_mode="multicast",
        width=media_file_info["width"],
        height=media_file_info["height"],
        fps=f"p{media_file_info['fps']}",
        transport_format=media_file_info["format"],
        output_format=media_file_info["file_format"],
        st20p_url=media_file_path,
        input_format=media_file_info["file_format"],
    )
    config = rxtxapp.change_replicas(
        config=config, session_type="st20p", replicas=replicas
    )

    rxtxapp.execute_test(
        config=config,
        build=mtl_path,
        test_time=test_time,
        virtio_user=True,
        host=host,
    )
