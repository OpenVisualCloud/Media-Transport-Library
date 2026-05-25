# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

import logging
import os

import pytest
from common.integrity.integrity_runner import FileVideoIntegrityRunner
from mtl_engine.execute import log_fail, run
from mtl_engine.ffmpeg_app import decode_video_format_16_9, generate_reference_file
from mtl_engine.media_files import yuv_files_422p10le

pytestmark = [pytest.mark.verified, pytest.mark.nightly]

PIX_FMTS = [
    "yuv422p10le",
    "yuv444p10le",
    "gbrp10le",
    "yuv420p",
    "y210le",
]

logger = logging.getLogger(__name__)


@pytest.mark.parametrize(
    "application",
    [
        "ffmpeg",
        pytest.param(
            "rxtxapp",
            marks=pytest.mark.skip(
                reason="RxTxApp does not support pix_fmt conversion"
            ),
        ),
    ],
)
@pytest.mark.parametrize("pix_fmt", PIX_FMTS)
@pytest.mark.parametrize(
    "video_format, media_file",
    [("i1080p25", yuv_files_422p10le["Penguin_1080p"])],
    indirect=["media_file"],
    ids=["Penguin_1080p"],
)
def test_pix_fmt(
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
):
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
        passed = app.execute_test(
            build=mtl_path,
            test_time=test_time,
            host=host,
            fail_on_error=False,
        )
        rx_output = app.output_files[0] if app.output_files else None
        if not passed:
            log_fail(f"loopback failed for pix_fmt={pix_fmt}")
            pytest.fail(f"loopback failed for pix_fmt={pix_fmt}")

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
            pytest.fail(f"integrity check failed for pix_fmt={pix_fmt}")
    finally:
        # Best-effort cleanup of files we created.
        if rx_output:
            run(f"rm -f {rx_output}", host=host)
        if cleanup_ref:
            run(f"rm -f {ref_file}", host=host)
