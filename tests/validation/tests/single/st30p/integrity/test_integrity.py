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

import pytest
import tests.Engine.RxTxApp as rxtxapp
from tests.Engine.execute import LOG_FOLDER, log_info
from tests.Engine.integrity import check_st30p_integrity
from tests.Engine.media_files import audio_files


@pytest.mark.parametrize("audio_format", ["PCM8", "PCM24"])
def test_integrity(build, media, nic_port_list, test_time, audio_format):
    st30p_file = audio_files[audio_format]
    st30p_file_url = os.path.join(media, st30p_file["filename"])

    out_file_url = os.path.join(os.getcwd(), LOG_FOLDER, "latest", "out.wav")

    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_st30p_sessions(
        config=config,
        nic_port_list=nic_port_list,
        test_mode="unicast",
        audio_format=audio_format,
        audio_channel=["U02"],
        audio_sampling="48kHz",
        audio_ptime="1",
        filename=st30p_file_url,
        out_url=out_file_url,
    )

    rxtxapp.execute_test(config=config, build=build, test_time=test_time)

    result = check_st30p_integrity(src_url=st30p_file_url, out_url=out_file_url)

    if result:
        log_info("INTEGRITY PASS")
    else:
        log_info("INTEGRITY FAIL")
