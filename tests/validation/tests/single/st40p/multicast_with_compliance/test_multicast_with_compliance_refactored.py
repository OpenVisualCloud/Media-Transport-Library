# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation
"""Refactored: st40p (ancillary pipeline) multicast + compliance.

Mirrors ``test_multicast_with_compliance.py`` but uses the unified
``application`` fixture (``session_type="st40p"``).
"""
import pytest
from common.nicctl import InterfaceSetup
from mtl_engine.media_files import anc_files


@pytest.mark.nightly
@pytest.mark.parametrize(
    "media_file",
    [
        anc_files["text_p29"],
        anc_files["text_p50"],
        pytest.param(anc_files["text_p59"], marks=pytest.mark.smoke),
    ],
    indirect=["media_file"],
    ids=[
        "text_p29",
        "text_p50",
        "text_p59",
    ],
)
@pytest.mark.refactored
def test_st40p_multicast_with_compliance_refactored(
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_time,
    test_config,
    pcap_capture,
    media_file,
    application,
):
    """Refactored test for st40p multicast with compliance.

    :param hosts: Mapping of host objects from the topology configuration.
    :param mtl_path: Path to the MTL build directory on the remote host.
    :param setup_interfaces: Interface setup helper for NIC / VF configuration.
    :param test_time: Duration to run the streaming pipeline, in seconds.
    :param test_config: Test configuration dictionary loaded from ``test_config.yaml``.
    :param pcap_capture: Pcap capture fixture for EBU ST 2110-21 compliance check.
    :param media_file: Parametrized media file fixture (info dict, file path).
    :param application: Media application driver fixture (currently ``RxTxApp``).
    """
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )
    # EBU compliance verdict needs many ancillary packets to classify.
    test_time = max(test_time, 90)

    application.create_command(
        session_type="st40p",
        nic_port_list=interfaces_list,
        test_mode="multicast",
        framerate=media_file_info["fps"],
        input_file=media_file_path,
        test_time=test_time,
    )

    application.execute_test(
        build=mtl_path,
        test_time=test_time,
        host=host,
        netsniff=pcap_capture,
    )
