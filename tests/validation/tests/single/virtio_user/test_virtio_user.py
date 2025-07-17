# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation
import os

import pytest
import mtl_engine.RxTxApp as rxtxapp
from mtl_engine.media_files import yuv_files


@pytest.mark.parametrize(
    "video_format, replicas",
    [
        ("i1080p60", 1),
        ("i1080p60", 3),
        ("i1080p60", 30),
        ("i2160p60", 1),
        ("i2160p60", 3),
        ("i2160p60", 9),
    ],
)
def test_virtio_user(hosts, build, media, nic_port_list, test_time, video_format, replicas, test_config, prepare_ramdisk):
    video_file = yuv_files[video_format]
    host = list(hosts.values())[0]

    # Get capture configuration from test_config.yaml
    # This controls whether tcpdump capture is enabled, where to store the pcap, etc.
    capture_cfg = dict(test_config.get("capture_cfg", {}))
    capture_cfg["test_name"] = f"test_virtio_user_multicast_{video_format}_replicas{replicas}"

    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_st20p_sessions(
        config=config,
        nic_port_list=host.vfs,
        test_mode="multicast",
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
        config=config, build=build, test_time=test_time, virtio_user=True, host=host, capture_cfg=capture_cfg
    )
