# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation
"""Sweep of MTL st20p input pixel formats, driven through both RxTxApp and
FFmpeg.

FFmpeg's ``-pix_fmt`` is not a separate feature to validate in isolation: the
mtl_st20p FFmpeg plugin (ecosystem/ffmpeg_plugin/mtl_st20p_{tx,rx}.c) maps
each AVPixelFormat straight onto an MTL ``input_fmt``/``transport_fmt`` pair
-- the same knobs RxTxApp exposes as ``pixel_format``/``transport_format``.
One input-format table therefore drives both apps instead of duplicating the
sweep per app (this replaces the old ffmpeg-only test_pix_fmt.py and the
rxtxapp-only test_st20p_pixel_formats in test_format_conversion.py).

Every case asserts both:
  1. Compliance -- ``pcap_capture`` records the wire traffic and the
     ``conftest.py`` teardown uploads it to the EBU LIST server.
  2. Integrity -- ``FileVideoIntegrityRunner`` MD5-compares the RX output
     against a reference file transcoded (via FFmpeg, host-side, independent
     of the app under test) from the canonical YUV422PLANAR10LE master.
"""

import logging
import os

import pytest
from common.integrity.integrity_runner import FileVideoIntegrityRunner
from mtl_engine.execute import log_fail, run
from mtl_engine.ffmpeg_app import decode_video_format_16_9, generate_reference_file
from mtl_engine.media_files import yuv_files_422p10le

pytestmark = [pytest.mark.verified, pytest.mark.nightly]

logger = logging.getLogger(__name__)

# One row per MTL st20p input pixel format reachable from both apps.
#
#   key              -- FFmpeg AVPixelFormat name; also generate_reference_file's
#                        dst_pix_fmt and FileVideoIntegrityRunner's file_format.
#   rxtxapp_format    -- matching RxTxApp pixel_format (input_format) enum name.
#   transport_format  -- MTL wire (ST20_FMT) transport this format converts to.
#
# Restricted to formats FFmpeg's swscale can produce as a real raw reference
# (see mtl_st20p_{tx,rx}.c's pix_fmt switch) AND that video_integrity.py knows
# the pixel size of. V210 and the packed RFC4175 wire pgroups (e.g.
# YUV422RFC4175PG2BE10) are deliberately excluded: they are internal wire
# layouts, not app-facing source encodings, and are already exercised as
# transport_format targets below via their natural planar/packed source.
INPUT_FORMATS = {
    "yuv422p10le": ("YUV422PLANAR10LE", "YUV_422_10bit"),
    "y210le": ("Y210", "YUV_422_10bit"),
    "uyvy422": ("UYVY", "YUV_422_8bit"),
    # Not an SMPTE ST 2110-20 pgroup -- MTL-to-MTL passthrough only (see
    # mtl_st20p_tx.c's YUV420CUSTOM8 comment). Compliance is expected to fail.
    "yuv420p": ("YUV420CUSTOM8", "YUV_420_8bit"),
    "yuv422p12le": ("YUV422PLANAR12LE", "YUV_422_12bit"),
    "yuv444p10le": ("YUV444PLANAR10LE", "YUV_444_10bit"),
    "yuv444p12le": ("YUV444PLANAR12LE", "YUV_444_12bit"),
    "gbrp10le": ("GBRPLANAR10LE", "RGB_10bit"),
    "gbrp12le": ("GBRPLANAR12LE", "RGB_12bit"),
}


@pytest.mark.parametrize("application", ["rxtxapp", "ffmpeg"])
@pytest.mark.parametrize("pix_fmt", list(INPUT_FORMATS.keys()))
@pytest.mark.parametrize(
    "video_format, media_file",
    [("i1080p25", yuv_files_422p10le["Penguin_1080p"])],
    indirect=["media_file"],
    ids=["Penguin_1080p"],
)
def test_st20p_input_format(
    application,
    app_factory,
    hosts,
    test_time,
    mtl_path,
    setup_interfaces,
    video_format,
    pix_fmt,
    test_config,
    media_file,
    pcap_capture,
    output_files,
):
    """TX->wire->RX for one MTL input format must be compliant and correct."""
    rxtxapp_format, transport_format = INPUT_FORMATS[pix_fmt]
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )
    video_size, _ = decode_video_format_16_9(video_format)

    # 1. Reference file in target pix_fmt (skip transcoding for the canonical fmt).
    if pix_fmt == "yuv422p10le":
        ref_file = media_file_path
        cleanup_ref = False
    else:
        ref_file = generate_reference_file(
            host=host,
            build=mtl_path,
            src_url=media_file_path,
            video_size=video_size,
            src_pix_fmt="yuv422p10le",
            dst_pix_fmt=pix_fmt,
        )
        cleanup_ref = True

    app = app_factory(application)
    rx_output = None
    try:
        if application == "ffmpeg":
            app.create_command(
                session_type="st20p",
                nic_port_list=interfaces_list,
                video_format=video_format,
                pg_format=media_file_info["format"],
                video_url=ref_file,
                output_format="yuv",
                mode="yuv_h264",
                tx_is_ffmpeg=True,
                pix_fmt=pix_fmt,
                keep_output=True,
                test_time=test_time,
            )
        else:
            rx_output = output_files.register(
                str(
                    host.connection.path(media_file_path).parent
                    / f"{media_file_info['filename']}_{pix_fmt}.out"
                )
            )
            app.create_command(
                session_type="st20p",
                nic_port_list=interfaces_list,
                test_mode="multicast",
                packing="GPM",
                width=media_file_info["width"],
                height=media_file_info["height"],
                framerate=f"p{media_file_info['fps']}",
                pixel_format=rxtxapp_format,
                transport_format=transport_format,
                input_file=ref_file,
                output_file=rx_output,
                test_time=test_time,
            )

        passed = app.execute_test(
            build=mtl_path,
            test_time=test_time,
            host=host,
            netsniff=pcap_capture,
            fail_on_error=False,
        )
        if application == "ffmpeg":
            rx_output = app.output_files[0] if app.output_files else None
        if not passed:
            log_fail(f"loopback failed for pix_fmt={pix_fmt}")
            pytest.fail(f"loopback failed for pix_fmt={pix_fmt}")

        # 2. Frame-by-frame MD5 integrity check.
        logger.info(f"Running video integrity check for pix_fmt={pix_fmt}")
        integrity = FileVideoIntegrityRunner(
            host=host,
            test_repo_path=mtl_path,
            src_url=ref_file,
            out_name=os.path.basename(rx_output),
            resolution=video_size,
            file_format=pix_fmt,
            out_path=os.path.dirname(rx_output),
            delete_file=False,
            integrity_path=os.path.join(
                mtl_path, "tests", "validation", "common", "integrity"
            ),
        )
        if not integrity.run():
            log_fail(f"integrity check failed for pix_fmt={pix_fmt}")
            pytest.fail(f"integrity check failed for pix_fmt={pix_fmt}")
    finally:
        # Best-effort cleanup of files we created (rxtxapp output is handled
        # by the output_files fixture; ffmpeg's keep_output=True output is not).
        if application == "ffmpeg" and rx_output:
            run(f"rm -f {rx_output}", host=host)
        if cleanup_ref:
            run(f"rm -f {ref_file}", host=host)
