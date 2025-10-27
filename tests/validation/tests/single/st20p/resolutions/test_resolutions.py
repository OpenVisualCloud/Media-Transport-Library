# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

import mtl_engine.RxTxApp as rxtxapp
import pytest
from mtl_engine.media_files import yuv_files_422rfc10


@pytest.mark.nightly
@pytest.mark.parametrize(
    "media_file",
    list(yuv_files_422rfc10.values()),
    indirect=["media_file"],
    ids=list(yuv_files_422rfc10.keys()),
)
def test_resolutions(
    hosts,
    build,
    media,
    nic_port_list,
    test_time,
    test_config,
    prepare_ramdisk,
    pcap_capture,
    media_file,
):
    media_file_info, media_file_path = media_file

    host = list(hosts.values())[0]

    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_st20p_sessions(
        config=config,
        nic_port_list=host.vfs,
        test_mode="multicast",
        width=media_file_info["width"],
        height=media_file_info["height"],
        fps=f"p{media_file_info['fps']}",
        input_format=media_file_info["file_format"],
        transport_format=media_file_info["format"],
        output_format=media_file_info["file_format"],
        st20p_url=media_file_path,
    )

    rxtxapp.execute_test(
        config=config,
        build=build,
        test_time=test_time,
        host=host,
        netsniff=pcap_capture,
    )
