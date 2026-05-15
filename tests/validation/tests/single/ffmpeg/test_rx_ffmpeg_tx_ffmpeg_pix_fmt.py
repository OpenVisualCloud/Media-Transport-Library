# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2026 Intel Corporation

import logging
import os

import pytest
from common.integrity.integrity_runner import FileVideoIntegrityRunner
from common.nicctl import InterfaceSetup
from mtl_engine import ffmpeg_app
from mtl_engine.execute import log_fail, run
from mtl_engine.media_files import yuv_files_422p10le

logger = logging.getLogger(__name__)

# Pixel formats supported by the mtl_st20p FFmpeg plugin.
# yuv422p10le pre-existing; yuv444p10le, gbrp10le, yuv420p, y210le added by this PR.
#
# Wire-format compliance vs SMPTE ST 2110-20:2022 §6.2:
#   yuv422p10le, y210le -> 4:2:2 10b RFC4175 PG2BE10  (Table 2)  compliant
#   yuv444p10le         -> 4:4:4 10b RFC4175 PG4BE10  (Table 1)  compliant
#   gbrp10le            -> RGB   10b RFC4175 PG4BE10  (Table 1)  compliant
#   yuv420p             -> ST_FRAME_FMT_YUV420CUSTOM8 passthrough, NON-compliant:
#                          MTL memcpys planar I420 straight to the wire instead of
#                          the §6.2.5 Table 3 pgroup (Y'00-Y'01-Y'10-Y'11-CB'00-CR'00).
#                          MTL<->MTL only; not interoperable with third-party receivers.
PIX_FMTS = [
    "yuv422p10le",
    "yuv444p10le",
    "gbrp10le",
    "yuv420p",
    "y210le",
]


@pytest.mark.nightly
@pytest.mark.parametrize("pix_fmt", PIX_FMTS)
@pytest.mark.parametrize(
    "video_format, media_file",
    [("i1080p25", yuv_files_422p10le["Penguin_1080p"])],
    indirect=["media_file"],
    ids=["Penguin_1080p"],
)
def test_rx_ffmpeg_tx_ffmpeg_pix_fmt(
    hosts,
    test_time,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    video_format,
    pix_fmt,
    test_config,
    media_file,
):
    """FFmpeg TX -> FFmpeg RX loopback with frame-by-frame integrity check.

    Validates every pixel format supported by the mtl_st20p FFmpeg plugin
    end-to-end:

    1. Pre-generates a reference file in the target ``pix_fmt`` from the
       canonical YUV422P10LE source (skipped when source already matches).
    2. Runs FFmpeg TX -> mtl_st20p -> FFmpeg RX loopback using that reference.
    3. Runs ``FileVideoIntegrityRunner`` to MD5-compare every received frame
       against the reference. Catches plane-layout / converter regressions
       that a simple "output > 0 bytes" check would miss.
    """
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )
    video_size, _ = ffmpeg_app.decode_video_format_16_9(video_format)

    # 1. Reference file in target pix_fmt (skip transcoding for the canonical fmt).
    if pix_fmt == "yuv422p10le":
        ref_file = media_file_path
        cleanup_ref = False
    else:
        ref_file = ffmpeg_app.generate_reference_file(
            host=host,
            build=mtl_path,
            src_url=media_file_path,
            video_size=video_size,
            src_pix_fmt="yuv422p10le",
            dst_pix_fmt=pix_fmt,
        )
        cleanup_ref = True

    rx_output = None
    try:
        # 2. TX/RX loopback. keep_output=True so integrity runner can read it.
        passed, rx_output = ffmpeg_app.execute_test(
            test_time=test_time,
            build=mtl_path,
            host=host,
            nic_port_list=interfaces_list,
            type_="frame",
            video_format=video_format,
            pg_format=media_file_info["format"],
            video_url=ref_file,
            output_format="yuv",
            pix_fmt=pix_fmt,
            keep_output=True,
        )
        if not passed:
            log_fail(f"loopback failed for pix_fmt={pix_fmt}")
            return

        # 3. Frame-by-frame MD5 integrity check.
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
    finally:
        # Best-effort cleanup of files we created.
        if rx_output:
            run(f"rm -f {rx_output}", host=host)
        if cleanup_ref:
            run(f"rm -f {ref_file}", host=host)
