# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

import mtl_engine.RxTxApp as rxtxapp
import pytest
from mtl_engine.media_files import yuv_files_422rfc10


@pytest.mark.nightly
@pytest.mark.parametrize("packing", ["GPM_SL", "GPM"])
@pytest.mark.parametrize(
    "media_file",
    [
        yuv_files_422rfc10["Crosswalk_720p"],
        yuv_files_422rfc10["ParkJoy_1080p"],
        yuv_files_422rfc10["Pedestrian_4K"],
    ],
    indirect=["media_file"],
    ids=["Crosswalk_720p", "ParkJoy_1080p", "Pedestrian_4K"],
)
def test_packing(
    hosts,
    build,
    media,
    nic_port_list,
    test_time,
    packing,
    test_config,
    prepare_ramdisk,
    media_file,
):
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]

    # Get capture configuration from test_config.yaml
    # This controls whether tcpdump capture is enabled, where to store the pcap, etc.
    capture_cfg = dict(test_config.get("capture_cfg", {}))
    capture_cfg["test_name"] = (
        f"test_packing_{media_file_info['filename']}_{packing}"  # Set a unique pcap file name
    )

    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_st20p_sessions(
        config=config,
        nic_port_list=host.vfs,
        test_mode="unicast",
        width=media_file_info["width"],
        height=media_file_info["height"],
        fps=f"p{media_file_info['fps']}",
        input_format=media_file_info["file_format"],
        transport_format=media_file_info["format"],
        output_format=media_file_info["file_format"],
        st20p_url=media_file_path,
        packing=packing,
    )

    rxtxapp.execute_test(
        config=config,
        build=build,
        test_time=test_time,
        host=host,
        capture_cfg=capture_cfg,
    )
