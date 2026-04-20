# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

import pytest
from common.nicctl import InterfaceSetup
from mtl_engine.media_files import anc_files


@pytest.mark.nightly
@pytest.mark.parametrize(
    "media_file",
    [
        anc_files["text_p29"],
        anc_files["text_p50"],
        anc_files["text_p59"],
    ],
    indirect=["media_file"],
    ids=[
        "text_p29",
        "text_p50",
        "text_p59",
    ],
)
def test_multicast_with_compliance_refactored(
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_time,
    test_config,
    prepare_ramdisk,
    pcap_capture,
    media_file,
    rxtxapp,
):
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )

    rxtxapp.create_command(
        session_type="ancillary",
        nic_port_list=interfaces_list,
        test_mode="multicast",
        type_mode="frame",
        ancillary_format="closed_caption",
        ancillary_fps=media_file_info["fps"],
        ancillary_url=media_file_path,
        test_time=test_time,
    )

    rxtxapp.execute_test(
        build=mtl_path,
        test_time=test_time,
        host=host,
        netsniff=pcap_capture,
    )
