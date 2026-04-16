# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation
import os

import mtl_engine.RxTxApp as rxtxapp
import pytest
from common.nicctl import InterfaceSetup
from mtl_engine.media_files import anc_files, audio_files, yuv_files


@pytest.mark.nightly
@pytest.mark.parametrize("test_mode", ["unicast", "multicast"])
def test_rx_timing_mode(
    hosts,
    mtl_path,
    media,
    setup_interfaces: InterfaceSetup,
    test_time,
    test_mode,
    test_config,
    prepare_ramdisk,
):
    video_file = yuv_files["i1080p50"]
    audio_file = audio_files["PCM24"]
    ancillary_file = anc_files["text_p50"]
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
        input_format=video_file["file_format"],
        transport_format=video_file["format"],
        output_format=video_file["file_format"],
        st20p_url=os.path.join(media, video_file["filename"]),
    )
    config = rxtxapp.add_st30p_sessions(
        config=config,
        nic_port_list=interfaces_list,
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
        nic_port_list=interfaces_list,
        test_mode=test_mode,
        type_="frame",
        ancillary_format="closed_caption",
        ancillary_fps=f"{ancillary_file['fps']}",
        ancillary_url=os.path.join(media, ancillary_file["filename"]),
    )

    rxtxapp.execute_test(
        config=config,
        build=mtl_path,
        test_time=test_time,
        rx_timing_parser=True,
        host=host,
    )
