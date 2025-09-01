# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation
import os

import mtl_engine.RxTxApp as rxtxapp
import pytest
from mtl_engine.media_files import anc_files, audio_files, yuv_files


@pytest.mark.parametrize("test_mode", ["unicast", "multicast"])
@pytest.mark.parametrize(
    "video_format",
    [
        pytest.param("i1080p30", marks=pytest.mark.nightly),
        pytest.param("i1080p50", marks=pytest.mark.nightly),
        "i1080p59",
        pytest.param("i2160p30", marks=pytest.mark.nightly),
        pytest.param("i2160p50", marks=pytest.mark.nightly),
        "i2160p59",
    ],
)
def test_ptp_mixed_format(
    hosts,
    build,
    media,
    nic_port_list,
    test_time,
    test_mode,
    video_format,
    test_config,
    prepare_ramdisk,
):
    video_file = yuv_files[video_format]
    audio_file = audio_files["PCM24"]
    ancillary_file = anc_files["text_p50"]
    host = list(hosts.values())[0]

    # Get capture configuration from test_config.yaml
    # This controls whether tcpdump capture is enabled, where to store the pcap, etc.
    capture_cfg = dict(test_config.get("capture_cfg", {}))
    capture_cfg["test_name"] = f"test_ptp_mixed_format_{test_mode}_{video_format}"

    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_st20p_sessions(
        config=config,
        nic_port_list=host.vfs,
        test_mode=test_mode,
        width=video_file["width"],
        height=video_file["height"],
        fps=f"p{video_file['fps']}",
        transport_format=video_file["format"],
        output_format=video_file["file_format"],
        st20p_url=os.path.join(media, video_file["filename"]),
        input_format=video_file["file_format"],
    )
    config = rxtxapp.add_st30p_sessions(
        config=config,
        nic_port_list=host.vfs,
        test_mode=test_mode,
        audio_format="PCM24",
        audio_channel=["U02"],
        audio_sampling="48kHz",
        audio_ptime="1",
        filename=os.path.join(media, audio_file["filename"]),
        out_url=os.path.join(media, audio_file["filename"]),
    )
    config = rxtxapp.add_ancillary_sessions(
        config=config,
        nic_port_list=host.vfs,
        test_mode=test_mode,
        type_="frame",
        ancillary_format="closed_caption",
        ancillary_fps=f"{ancillary_file['fps']}",
        ancillary_url=os.path.join(media, ancillary_file["filename"]),
    )

    rxtxapp.execute_test(
        config=config,
        build=build,
        test_time=test_time,
        ptp=True,
        host=host,
        capture_cfg=capture_cfg,
    )
