# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation
"""Refactored XDP standard mode tests (single session_type per parametrize variant)."""
import pytest
from mtl_engine.media_files import yuv_files, yuv_files_422rfc10


@pytest.mark.refactored
@pytest.mark.parametrize("standard_mode", ["st20p", "st22p"])
@pytest.mark.parametrize("test_mode", ["multicast"])
@pytest.mark.parametrize("video_format", ["i1080p59"])
@pytest.mark.parametrize("replicas", [1, 2])
def test_xdp_standard_refactored(
    hosts,
    mtl_path,
    media,
    test_time,
    test_mode,
    video_format,
    replicas,
    standard_mode,
    prepare_ramdisk,
    rxtxapp,
):
    host = list(hosts.values())[0]
    # native_af_xdp program load + ARP resolution is slower than VF.
    test_time = max(test_time, 90)

    if standard_mode == "st20p":
        video_file = yuv_files[video_format]
        rxtxapp.create_command(
            session_type="st20p",
            nic_port_list=["native_af_xdp:eth2", "native_af_xdp:eth3"],
            test_mode=test_mode,
            width=video_file["width"],
            height=video_file["height"],
            framerate=f"p{video_file['fps']}",
            pixel_format=video_file["file_format"],
            transport_format=video_file["format"],
            input_file=str(host.connection.path(media, video_file["filename"])),
            replicas=replicas,
            test_time=test_time,
        )
    else:  # st22p
        st22p_file = yuv_files_422rfc10["Crosswalk_1080p"]
        rxtxapp.create_command(
            session_type="st22p",
            nic_port_list=["native_af_xdp:eth2", "native_af_xdp:eth3"],
            test_mode=test_mode,
            width=st22p_file["width"],
            height=st22p_file["height"],
            framerate=f"p{st22p_file['fps']}",
            codec="JPEG-XS",
            quality="speed",
            pack_type="codestream",
            pixel_format=st22p_file["file_format"],
            codec_threads=2,
            input_file=str(host.connection.path(media, st22p_file["filename"])),
            replicas=replicas,
            test_time=test_time,
        )

    rxtxapp.execute_test(
        build=mtl_path,
        test_time=test_time,
        host=host,
    )
