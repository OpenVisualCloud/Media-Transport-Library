# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation
import mtl_engine.RxTxApp as rxtxapp
import pytest
from mtl_engine.media_files import yuv_files


@pytest.mark.parametrize(
    "media_file, replicas",
    [
        (yuv_files["i1080p60"], 1),
        (yuv_files["i1080p60"], 3),
        (yuv_files["i1080p60"], 30),
        (yuv_files["i2160p60"], 1),
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
    build,
    media,
    nic_port_list,
    test_time,
    replicas,
    test_config,
    prepare_ramdisk,
    media_file,
):
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]

    # Get capture configuration from test_config.yaml
    # This controls whether tcpdump capture is enabled, where to store the pcap, etc.
    capture_cfg = dict(test_config.get("capture_cfg", {}))
    capture_cfg["test_name"] = (
        f"test_virtio_user_multicast_{media_file_info['filename']}_replicas{replicas}"
    )

    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_st20p_sessions(
        config=config,
        nic_port_list=host.vfs,
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
        build=build,
        test_time=test_time,
        virtio_user=True,
        host=host,
        capture_cfg=capture_cfg,
    )
