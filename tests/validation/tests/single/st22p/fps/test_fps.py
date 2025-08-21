# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation


import mtl_engine.RxTxApp as rxtxapp
import pytest
from mtl_engine.media_files import yuv_files_422p10le


@pytest.mark.parametrize(
    "media_file",
    [yuv_files_422p10le["Penguin_1080p"]],
    indirect=["media_file"],
    ids=["Penguin_1080p"],
)
@pytest.mark.parametrize(
    "fps",
    [
        "p23",
        "p24",
        pytest.param("p25", marks=pytest.mark.nightly),
        "p29",
        "p30",
        "p50",
        "p59",
        "p60",
        "p100",
        "p119",
        "p120",
    ],
)
@pytest.mark.parametrize("codec", ["JPEG-XS", "H264_CBR"])
def test_fps(
    hosts,
    build,
    media,
    nic_port_list,
    test_time,
    fps,
    codec,
    test_config,
    prepare_ramdisk,
    media_file,
):
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]

    # Get capture configuration from test_config.yaml
    # This controls whether tcpdump capture is enabled, where to store the pcap, etc.
    capture_cfg = dict(test_config.get("capture_cfg", {}))
    capture_cfg["test_name"] = f"test_fps_{codec}_{media_file_info['filename']}_{fps}"

    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_st22p_sessions(
        config=config,
        nic_port_list=host.vfs,
        test_mode="multicast",
        width=media_file_info["width"],
        height=media_file_info["height"],
        fps=fps,
        codec=codec,
        quality="speed",
        input_format=media_file_info["file_format"],
        output_format=media_file_info["file_format"],
        st22p_url=media_file_path,
        codec_thread_count=16,
    )

    rxtxapp.execute_test(
        config=config,
        build=build,
        test_time=test_time,
        host=host,
        capture_cfg=capture_cfg,
    )
