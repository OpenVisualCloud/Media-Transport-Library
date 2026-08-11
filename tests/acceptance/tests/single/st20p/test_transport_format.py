# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation
"""Supported ST 2110-20 RFC 4175 pixel groups at a pinned stream shape."""

import pytest
from mtl_engine.media_files import yuv_files_input_formats

pytestmark = [pytest.mark.verified, pytest.mark.nightly]

TRANSPORT_MEDIA = list(
    {
        media["format"]: media
        for media in yuv_files_input_formats.values()
        if media["format"] != "YUV_420_8bit"
    }.values()
)


@pytest.mark.parametrize(
    "media_file",
    TRANSPORT_MEDIA,
    indirect=["media_file"],
    ids=[media["format"] for media in TRANSPORT_MEDIA],
)
def test_st20p_transport_format(
    app_factory,
    hosts,
    test_time,
    mtl_path,
    setup_interfaces,
    test_config,
    media_file,
    pcap_capture,
    output_files,
    media_integrity,
):
    """Each pixel group must packetize compliantly and arrive intact."""
    media_info, media_path = media_file
    host = list(hosts.values())[0]
    config_params = dict(
        session_type="st20p",
        nic_port_list=setup_interfaces.get_interfaces_list_single(
            test_config.get("interface_type", "VF")
        ),
        test_mode="multicast",
        packing="GPM_SL",
        pacing="narrow",
        width=media_info["width"],
        height=media_info["height"],
        framerate=f"p{media_info['fps']}",
        pixel_format=media_info["file_format"],
        transport_format=media_info["format"],
        input_file=media_path,
        output_file=output_files.register(
            f"{media_path}_{media_info['format']}_GPM_SL.out"
        ),
        test_time=test_time,
    )

    app = app_factory("rxtxapp")
    app.create_command(**config_params)
    app.execute_test(
        build=mtl_path,
        test_time=test_time,
        host=host,
        compliance=pcap_capture,
        integrity=media_integrity,
    )


_PACKING_FORMAT_CASES = [
    (packing, yuv_files_input_formats["yuv422p10le"])
    for packing in ("BPM", "GPM", "GPM_SL")
] + [("GPM", media) for media in TRANSPORT_MEDIA if media["format"] != "YUV_422_10bit"]


@pytest.mark.parametrize(
    "packing, media_file",
    _PACKING_FORMAT_CASES,
    indirect=["media_file"],
    ids=[f"{packing}-{media['format']}" for packing, media in _PACKING_FORMAT_CASES],
)
def test_st20p_packing_transport_format(
    app_factory,
    hosts,
    test_time,
    mtl_path,
    setup_interfaces,
    packing,
    test_config,
    media_file,
    pcap_capture,
    output_files,
    media_integrity,
):
    """Cover packetizer interaction with each pixel-group geometry."""
    media_info, media_path = media_file
    host = list(hosts.values())[0]
    config_params = dict(
        session_type="st20p",
        nic_port_list=setup_interfaces.get_interfaces_list_single(
            test_config.get("interface_type", "VF")
        ),
        test_mode="multicast",
        packing=packing,
        pacing="narrow",
        width=media_info["width"],
        height=media_info["height"],
        framerate=f"p{media_info['fps']}",
        pixel_format=media_info["file_format"],
        transport_format=media_info["format"],
        input_file=media_path,
        output_file=output_files.register(
            f"{media_path}_{media_info['format']}_{packing}.out"
        ),
        test_time=test_time,
    )

    app = app_factory("rxtxapp")
    app.create_command(**config_params)
    app.execute_test(
        build=mtl_path,
        test_time=test_time,
        host=host,
        compliance=pcap_capture,
        integrity=media_integrity,
    )
