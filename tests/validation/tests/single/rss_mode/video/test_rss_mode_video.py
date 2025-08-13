# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation
import os

import mtl_engine.RxTxApp as rxtxapp
import pytest
from mtl_engine.media_files import yuv_files


@pytest.mark.parametrize("rss_mode", ["l3_l4", "l3", "none"])
@pytest.mark.parametrize("video_format", ["i1080p60", "i2160p60"])
def test_rss_mode_video(
    hosts,
    build,
    media,
    nic_port_list,
    test_time,
    video_format,
    rss_mode,
    test_config,
    prepare_ramdisk,
):
    video_file = yuv_files[video_format]
    host = list(hosts.values())[0]

    # Get capture configuration from test_config.yaml
    # Collect packet capture configuration and assign test_name
    capture_cfg = dict(test_config.get("capture_cfg", {}))
    capture_cfg["test_name"] = f"test_rss_mode_video_{video_format}_{rss_mode}"

    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_st20p_sessions(
        config=config,
        nic_port_list=host.vfs,
        test_mode="unicast",
        width=video_file["width"],
        height=video_file["height"],
        fps=f"p{video_file['fps']}",
        input_format=video_file["file_format"],
        transport_format=video_file["format"],
        output_format=video_file["file_format"],
        st20p_url=os.path.join(media, video_file["filename"]),
    )
    config = rxtxapp.change_rss_mode(content=config, rss_mode=rss_mode)

    rxtxapp.execute_test(
        config=config,
        build=build,
        test_time=test_time,
        host=host,
        capture_cfg=capture_cfg,
    )
