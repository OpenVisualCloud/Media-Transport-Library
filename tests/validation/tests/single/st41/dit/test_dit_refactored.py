# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

import pytest
from common.nicctl import InterfaceSetup
from mtl_engine.media_files import st41_files
from tests.single.st41 import dit_mapping, k_bit_mapping, payload_type_mapping


@pytest.mark.nightly
@pytest.mark.parametrize(
    "media_file",
    [st41_files["st41_p29_long_file"]],
    indirect=["media_file"],
    ids=["st41_p29_long_file"],
)
@pytest.mark.refactored
@pytest.mark.parametrize("dit", ["dit0", "dit1"])
def test_dit_refactored(
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_time,
    dit,
    test_config,
    media_file,
    pcap_capture,
    application,
):
    """Test the Data Item Type (DIT) fastmetadata_data_item_type.

    :param hosts: Mapping of host objects from the topology configuration.
    :param mtl_path: Path to the MTL build directory on the remote host.
    :param setup_interfaces: Interface setup helper for NIC / VF configuration.
    :param test_time: Duration to run the streaming pipeline, in seconds.
    :param dit: Parametrized data item type for ST 2110-41 fast metadata.
    :param test_config: Test configuration dictionary loaded from ``test_config.yaml``.
    :param media_file: Parametrized media file fixture (info dict, file path).
    :param application: Media application driver fixture (currently ``RxTxApp``).
    :param pcap_capture: Pcap capture fixture for EBU ST 2110-21 compliance check.
    """
    _, media_file_path = media_file
    payload_type = payload_type_mapping["pt115"]
    k_bit = k_bit_mapping["k0"]

    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )

    application.create_command(
        session_type="fastmetadata",
        nic_port_list=interfaces_list,
        test_mode="unicast",
        type_mode="rtp",
        payload_type=payload_type,
        fastmetadata_data_item_type=dit_mapping[dit],
        fastmetadata_k_bit=k_bit,
        fastmetadata_fps="p59",
        input_file=media_file_path,
        test_time=test_time,
    )

    application.execute_test(
        build=mtl_path, test_time=test_time, host=host, netsniff=pcap_capture
    )
