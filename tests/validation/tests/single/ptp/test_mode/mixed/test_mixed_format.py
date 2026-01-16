# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation
import logging
import os

import mtl_engine.RxTxApp as rxtxapp
import pytest
from common.integrity.integrity_runner import (
    FileAudioIntegrityRunner,
    FileVideoIntegrityRunner,
)
from common.nicctl import InterfaceSetup
from mtl_engine.execute import log_fail
from mtl_engine.integrity import get_sample_size
from mtl_engine.media_files import anc_files, audio_files, parse_fps_to_pformat, yuv_files

logger = logging.getLogger(__name__)


@pytest.mark.nightly
# @pytest.mark.skip
@pytest.mark.parametrize(
    "interface_profile",
    [
        pytest.param(
            {"mode": "vf_only"},
            id="vf_only",
        ),
        pytest.param(
            {"mode": "mixed", "tx_type": "PF", "rx_type": "VF"},
            id="pf_tx_vf_rx",
        ),
    ],
)
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
    setup_interfaces: InterfaceSetup,
    test_time,
    interface_profile,
    video_format,
    test_config,
    prepare_ramdisk,
    pcap_capture,
    media_file,
):
    video_file = yuv_files[video_format]
    audio_file = audio_files["PCM24"]
    ancillary_file = anc_files["text_p50"]
    _, ramdisk_path = media_file
    if not ramdisk_path:
        raise ValueError("ramdisk was not setup correctly for media_file fixture")
    host = list(hosts.values())[0]
    if interface_profile["mode"] == "vf_only":
        interfaces_list = setup_interfaces.get_interfaces_list_single("VF")
    else:
        tx_index = test_config.get("tx_interface_index", 0)
        rx_index = test_config.get("rx_interface_index", 1)
        interfaces_list = setup_interfaces.get_mixed_interfaces_list_single(
            tx_interface_type=interface_profile["tx_type"],
            rx_interface_type=interface_profile["rx_type"],
            tx_index=tx_index,
            rx_index=rx_index,
        )

    video_out_url = os.path.join(ramdisk_path, f"{video_file['filename']}.out")
    audio_out_url = os.path.join(ramdisk_path, f"{audio_file['filename']}.out")

    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_st20p_sessions(
        config=config,
        nic_port_list=interfaces_list,
        test_mode="multicast",
        width=video_file["width"],
        height=video_file["height"],
        fps=parse_fps_to_pformat(video_file["fps"]),
        transport_format=video_file["format"],
        output_format=video_file["file_format"],
        st20p_url=os.path.join(media, video_file["filename"]),
        input_format=video_file["file_format"],
        out_url=video_out_url,
    )
    config = rxtxapp.add_st30p_sessions(
        config=config,
        nic_port_list=interfaces_list,
        test_mode="multicast",
        audio_format="PCM24",
        audio_channel=["U02"],
        audio_sampling="48kHz",
        audio_ptime="1",
        filename=os.path.join(media, audio_file["filename"]),
        out_url=audio_out_url,
    )
    config = rxtxapp.add_ancillary_sessions(
        config=config,
        nic_port_list=interfaces_list,
        test_mode="multicast",
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
    )

    if test_config.get("integrity_check", True):
        logger.info("Running audio integrity check...")
        audio_integrity = FileAudioIntegrityRunner(
            host=host,
            test_repo_path=build,
            src_url=os.path.join(media, audio_file["filename"]),
            out_name=os.path.basename(audio_out_url),
            sample_size=get_sample_size("PCM24"),
            out_path=ramdisk_path,
        )
        if not audio_integrity.run():
            log_fail("Audio integrity check failed")

        logger.info("Running video integrity check...")
        resolution = f"{video_file['width']}x{video_file['height']}"
        video_integrity = FileVideoIntegrityRunner(
            host=host,
            test_repo_path=build,
            src_url=os.path.join(media, video_file["filename"]),
            out_name=os.path.basename(video_out_url),
            resolution=resolution,
            file_format=video_file["file_format"],
            out_path=ramdisk_path,
            integrity_path=os.path.join(
                build, "tests", "validation", "common", "integrity"
            ),
        )
        if not video_integrity.run():
            log_fail("Video integrity check failed")
