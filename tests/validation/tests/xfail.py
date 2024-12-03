# INTEL CONFIDENTIAL
# Copyright 2024-2024 Intel Corporation.
#
# This software and the related documents are Intel copyrighted materials, and your use of them is governed
# by the express license under which they were provided to you ("License"). Unless the License provides otherwise,
# you may not use, modify, copy, publish, distribute, disclose or transmit this software or the related documents
# without Intel's prior written permission.
#
# This software and the related documents are provided as is, with no express or implied warranties,
# other than those that are expressly stated in the License.
import logging

import pytest
from tests.Engine.stash import set_issue


def add_issue(issue: str, request):
    logging.debug(issue)
    set_issue(issue)
    request.node.add_marker(pytest.mark.xfail)


def SDBQ1001_audio_channel_check(audio_channel: str, audio_format: str, request):
    if audio_channel == "222" and (audio_format == "PCM16" or audio_format == "PCM24"):
        add_issue(
            "XFAIL: SDBQ-1001 - Audio/St30p, PCM-16 with 222 audio channel tx_audio_session_attach error", request
        )


def SDBQ1002_pg_format_error_check(video_format: str, pg_format: str, request):
    if video_format == "i720p50" and pg_format == "V210":
        add_issue("XFAIL: SDBQ-1002 - Video, i720p50fps with V210 pg_format Error: tv_frame_free_cb", request)
