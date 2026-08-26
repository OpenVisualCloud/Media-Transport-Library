# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation
"""Sweep of MTL st20p input pixel formats, driven through RxTxApp, FFmpeg and
GStreamer.

A framework's own pixel-format option is not a separate feature to validate in
isolation: the mtl_st20p FFmpeg plugin
(ecosystem/ffmpeg_plugin/mtl_st20p_{tx,rx}.c) maps each AVPixelFormat straight
onto an MTL ``input_fmt``/``transport_fmt`` pair, and the GStreamer plugin
(ecosystem/gstreamer_plugin/gst_mtl_st20p_{tx,rx}.c) does the same for the
formats it converts -- the same knobs RxTxApp exposes as
``pixel_format``/``transport_format``. One input-format table therefore drives
every app instead of duplicating the sweep per app. Formats a plugin does not
implement are skipped by the adapter's own ``unsupported_reason()``, so no case
reports a pass without having run.

Each pix_fmt has its own pre-generated media asset on the NFS share (see
``yuv_files_input_formats`` in mtl_engine/media_files.py) -- tests must never
transcode at runtime.

Every case asserts both:
  1. Compliance -- ``pcap_capture`` records the wire traffic and the
     ``conftest.py`` teardown uploads it to the EBU LIST server.
  2. Integrity -- ``media_integrity`` MD5-compares the RX output against
     the source media file.
"""

import logging

import pytest
from mtl_engine.media_files import yuv_files_input_formats

pytestmark = [pytest.mark.verified, pytest.mark.nightly]

logger = logging.getLogger(__name__)


@pytest.mark.parametrize("application", ["rxtxapp", "ffmpeg", "gstreamer"])
@pytest.mark.parametrize(
    "pix_fmt, media_file",
    list(yuv_files_input_formats.items()),
    indirect=["media_file"],
    ids=list(yuv_files_input_formats.keys()),
)
def test_st20p_input_format(
    application,
    app_factory,
    hosts,
    test_time,
    mtl_path,
    setup_interfaces,
    pix_fmt,
    test_config,
    media_file,
    pcap_capture,
    output_files,
    media_integrity,
):
    """TX->wire->RX for one MTL input format must be compliant and correct."""
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )

    app = app_factory(
        application,
        session_type="st20p",
        pixel_format=media_file_info["file_format"],
        transport_format=media_file_info["format"],
    )
    rx_output = output_files.register(f"{media_file_path}_{pix_fmt}.out")
    app.create_command(
        session_type="st20p",
        nic_port_list=interfaces_list,
        test_mode="multicast",
        # BPM matches the FFmpeg plugin's fixed packing mode (see
        # mtl_st20p_tx.c) -- both apps must produce the same wire format so a
        # single capture-sizing calculation covers both.
        packing="BPM",
        width=media_file_info["width"],
        height=media_file_info["height"],
        framerate=f"p{media_file_info['fps']}",
        pixel_format=media_file_info["file_format"],
        transport_format=media_file_info["format"],
        input_file=media_file_path,
        output_file=rx_output,
        test_time=test_time,
    )

    app.execute_test(
        build=mtl_path,
        test_time=test_time,
        host=host,
        compliance=pcap_capture,
        integrity=media_integrity,
    )
