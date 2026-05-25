# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

import logging
import os
from pathlib import Path

import pytest
from mtl_engine import media_files as mf
from mtl_engine.const import LOG_FOLDER
from mtl_engine.execute import log_fail
from mtl_engine.integrity import calculate_yuv_frame_size, check_st20p_integrity

pytestmark = [pytest.mark.verified, pytest.mark.nightly]

INTEGRITY_MEDIA = [
    ("Penguin_720p_422rfc10", "yuv_files_422rfc10", "Penguin_720p"),
    ("Penguin_1080p_422rfc10", "yuv_files_422rfc10", "Penguin_1080p"),
    ("Penguin_720p_422p10le", "yuv_files_422p10le", "Penguin_720p"),
    ("Penguin_1080p_422p10le", "yuv_files_422p10le", "Penguin_1080p"),
]

logger = logging.getLogger(__name__)


@pytest.mark.parametrize("application", ["ffmpeg", "rxtxapp"])
@pytest.mark.parametrize(
    "media_case, media_dict, media_key",
    INTEGRITY_MEDIA,
    ids=[case[0] for case in INTEGRITY_MEDIA],
)
def test_integrity(
    application,
    app_factory,
    hosts,
    mtl_path,
    setup_interfaces,
    test_config,
    test_time,
    media_case,
    media_dict,
    media_key,
):
    media_file_info = getattr(mf, media_dict)[media_key]
    media_path = test_config.get("media_path", "/mnt/media")
    media_file_path = os.path.join(media_path, media_file_info["filename"])
    log_dir = Path.cwd() / LOG_FOLDER / "latest"
    log_dir.mkdir(parents=True, exist_ok=True)
    out_file_url = str(log_dir / "out.yuv")
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )
    app = app_factory(application)
    app.create_command(
        session_type="st20p",
        nic_port_list=interfaces_list,
        source_ip=None,
        destination_ip=None,
        port=20000,
        width=media_file_info["width"],
        height=media_file_info["height"],
        framerate="p25",
        pixel_format=media_file_info["file_format"],
        transport_format=media_file_info["format"],
        input_file=media_file_path,
        output_file=out_file_url,
        test_mode="unicast",
        pacing="linear",
        test_time=test_time,
    )
    actual_test_time = max(test_time, 8)
    app.execute_test(build=mtl_path, test_time=actual_test_time, host=host)
    frame_size = calculate_yuv_frame_size(
        media_file_info["width"],
        media_file_info["height"],
        media_file_info["file_format"],
    )
    result = check_st20p_integrity(
        src_url=media_file_path, out_url=out_file_url, frame_size=frame_size
    )
    if result:
        logger.info("INTEGRITY PASS")
    else:
        log_fail("INTEGRITY FAIL")
        raise AssertionError(
            "st20p integrity test failed content integrity comparison."
        )
