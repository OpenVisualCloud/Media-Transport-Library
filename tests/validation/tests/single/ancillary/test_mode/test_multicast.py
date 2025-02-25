# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation
import os

import pytest
import tests.Engine.RxTxApp as rxtxapp
from tests.Engine.media_files import anc_files


@pytest.mark.parametrize("anc_keys", anc_files.keys())
def test_multicast(build, media, nic_port_list, test_time, anc_keys):
    ancillary_file = anc_files[anc_keys]

    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_ancillary_sessions(
        config=config,
        nic_port_list=nic_port_list,
        test_mode="multicast",
        type_="frame",
        ancillary_format="closed_caption",
        ancillary_fps=ancillary_file["fps"],
        ancillary_url=os.path.join(media, ancillary_file["filename"]),
    )

    rxtxapp.execute_test(config=config, build=build, test_time=test_time)
