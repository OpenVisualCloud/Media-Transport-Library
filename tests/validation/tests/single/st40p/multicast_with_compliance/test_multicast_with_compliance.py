# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2025 Intel Corporation
"""Multicast + compliance tests for the st40p (ancillary pipeline) path.

Replaces the legacy ``ancillary/multicast_with_compliance`` test that
exercised the session API. Behaviour is identical: when
``capture_cfg.enable`` is set in the test config, the ``pcap_capture``
fixture starts ``netsniff-ng`` so the captured packets can be checked
for SMPTE ST 2110-40 compliance.
"""
import mtl_engine.RxTxApp as rxtxapp
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
def test_st40p_multicast_with_compliance(
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_time,
    test_config,
    prepare_ramdisk,
    pcap_capture,
    media_file,
):
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )

    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_st40p_sessions(
        config=config,
        nic_port_list=interfaces_list,
        test_mode="multicast",
        fps=media_file_info["fps"],
        st40p_url=media_file_path,
    )

    rxtxapp.execute_test(
        config=config,
        build=mtl_path,
        test_time=test_time,
        host=host,
        netsniff=pcap_capture,
    )
