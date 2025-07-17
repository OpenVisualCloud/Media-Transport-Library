# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

import os

import pytest
import mtl_engine.RxTxApp as rxtxapp
from mtl_engine.media_files import yuv_files_422p10le


@pytest.mark.parametrize("codec", ["JPEG-XS", "H264_CBR"])
def test_codec(hosts, build, media, nic_port_list, test_time, codec, test_config, prepare_ramdisk):
    st22p_file = yuv_files_422p10le["Penguin_1080p"]
    host = list(hosts.values())[0]

    # Get capture configuration from test_config.yaml
    # This controls whether tcpdump capture is enabled, where to store the pcap, etc.
    capture_cfg = dict(test_config.get("capture_cfg", {}))
    capture_cfg["test_name"] = f"test_codec_{codec}_Penguin_1080p"

    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_st22p_sessions(
        config=config,
        nic_port_list=host.vfs,
        test_mode="multicast",
        width=st22p_file["width"],
        height=st22p_file["height"],
        fps=f"p{st22p_file['fps']}",
        codec=codec,
        quality="speed",
        input_format=st22p_file["file_format"],
        output_format=st22p_file["file_format"],
        st22p_url=os.path.join(media, st22p_file["filename"]),
        codec_thread_count=2,
    )

    rxtxapp.execute_test(config=config, build=build, test_time=test_time, host=host, capture_cfg=capture_cfg)
