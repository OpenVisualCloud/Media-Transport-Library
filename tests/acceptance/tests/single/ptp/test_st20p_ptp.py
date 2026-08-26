# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation
"""ST 2110-21 pacing driven by MTL's built-in PTP client, for every application.

PTP is a libmtl device option, not a per-application feature: RxTxApp's
``--ptp``, the FFmpeg plugin's ``-ptp_enable 1`` and the GStreamer elements'
``enable-ptp=true`` all switch on the same PHC inside the library. One
parametrized test therefore covers all three, instead of one test file per
framework asserting the same thing.

``@pytest.mark.ptp`` makes the harness stop phc2sys first, so the library's own
clock -- not the system clock it would otherwise follow -- is what paces the
stream. The oracles are the usual pair:
  1. Compliance -- the EBU LIST verdict on the captured wire traffic, which is
     what actually proves the PTP-derived pacing is narrow.
  2. Integrity -- MD5 of the RX output against the source file, so a
     compliant-but-empty stream cannot pass.
"""

import logging

import pytest
from mtl_engine.media_files import yuv_files_input_formats

pytestmark = [pytest.mark.verified, pytest.mark.nightly, pytest.mark.ptp]

logger = logging.getLogger(__name__)

# 1080p25 planar 10-bit: the one asset every application can read directly, so
# the PTP dimension is what varies between cases and nothing else.
_MEDIA_FILE = yuv_files_input_formats["yuv422p10le"]


@pytest.mark.parametrize("application", ["rxtxapp", "ffmpeg", "gstreamer"])
@pytest.mark.parametrize(
    "media_file", [_MEDIA_FILE], indirect=["media_file"], ids=["yuv422p10le_1080p25"]
)
@pytest.mark.tx_and_rx
def test_st20p_ptp(
    application,
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
    """One st20p stream paced by the internal PTP clock must stay compliant."""
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
    rx_output = output_files.register(f"{media_file_path}_ptp.out")
    app.create_command(
        session_type="st20p",
        nic_port_list=interfaces_list,
        test_mode="multicast",
        width=media_file_info["width"],
        height=media_file_info["height"],
        framerate=f"p{media_file_info['fps']}",
        pixel_format=media_file_info["file_format"],
        transport_format=media_file_info["format"],
        input_file=media_file_path,
        output_file=rx_output,
        enable_ptp=True,
        test_time=test_time,
    )

    app.execute_test(
        build=mtl_path,
        test_time=test_time,
        host=host,
        compliance=pcap_capture,
        integrity=media_integrity,
    )
