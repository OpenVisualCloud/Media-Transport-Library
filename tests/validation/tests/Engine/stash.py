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
import os
from typing import List

result_media = []
result_log = []
issue = ""
result_note = ""


def get_result_media() -> List[str]:
    return result_media


def clear_result_media() -> List[str]:
    global result_media
    result_media = []
    return result_media


def add_result_media(media: str) -> List[str]:
    result_media.append(media)
    return result_media


def remove_result_media() -> None:
    for file in result_media:
        try:
            if os.path.exists(file):
                os.remove(file)
        except OSError as e:
            print(f"Error removing file {file}: {e}")


def clear_result_log() -> List[str]:
    global result_log
    result_log = []
    return result_log


def add_result_log(msg: str) -> List[str]:
    result_log.append(msg)
    return result_log


def get_result_log() -> List[str]:
    return result_log


def clear_issue() -> str:
    global issue
    issue = ""
    return issue


def set_issue(msg: str) -> str:
    global issue
    issue = msg
    return issue


def get_issue() -> str:
    return issue


def clear_result_note() -> str:
    global result_note
    result_note = ""
    return result_note


def set_result_note(note: str) -> str:
    global result_note
    result_note = note
    return result_note


def get_result_note() -> str:
    return result_note
