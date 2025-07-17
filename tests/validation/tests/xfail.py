# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation
import logging

import pytest
from mtl_engine.stash import set_issue


def add_issue(issue: str, request):
    logging.debug(issue)
    set_issue(issue)
    request.node.add_marker(pytest.mark.xfail)


def SDBQ1001_audio_channel_check(audio_channel: str, audio_format: str, request):
    if audio_channel == "222" and (audio_format == "PCM16" or audio_format == "PCM24"):
        add_issue(
            "XFAIL: SDBQ-1001 - Audio/St30p, PCM-16 with 222 audio channel tx_audio_session_attach error",
            request,
        )


def SDBQ1002_pg_format_error_check(video_format: str, pg_format: str, request):
    if video_format == "i720p50" and pg_format == "V210":
        add_issue(
            "XFAIL: SDBQ-1002 - Video, i720p50fps with V210 pg_format Error: tv_frame_free_cb",
            request,
        )


def SDBQ1971_conversion_v210_720p_error(
    video_format: str, resolution_width: int, request
):
    if video_format == "v210" and resolution_width == 720:
        add_issue(
            "XFAIL: SDBQ-1971 - Conversion from v210 format does not work on 720p",
            request,
        )
        assert False
