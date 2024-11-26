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
from tests.Engine.media_files import anc_files


@pytest.mark.parametrize("type_mode", ["rtp", "frame"])
@pytest.mark.parametrize("anc_keys", anc_files.keys())
def test_type_mode(build, media, nic_port_list, test_time, type_mode, anc_keys):
    ancillary_file = anc_files[anc_keys]

    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_ancillary_sessions(
        config=config,
        nic_port_list=nic_port_list,
        test_mode="unicast",
        type_=type_mode,
        ancillary_format="closed_caption",
        ancillary_fps=ancillary_file["fps"],
        ancillary_url=os.path.join(media, ancillary_file["filename"]),
    )

    rxtxapp.execute_test(config=config, build=build, test_time=test_time)
