# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

import mtl_engine.RxTxApp as rxtxapp
import pytest
from common.nicctl import InterfaceSetup
from mtl_engine.media_files import yuv_files_422rfc10

pytestmark = pytest.mark.verified


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
    setup_interfaces: InterfaceSetup,
    test_time,
    test_config,
    prepare_ramdisk,
    pcap_capture,
    media_file,
):
    """
    Validate multicast ST20P streaming across the full catalog of YUV422
    RFC4175 sample resolutions to ensure pipeline configuration scales with
    frame size and bitrate. Packet capture is enabled to aid
    troubleshooting if large-frame configurations expose fragmentation or
    pacing issues.

    :param hosts: Mapping of hosts available for the test run.
    :param build: Compiled Rx/Tx application artifact used for execution.
    :param setup_interfaces: Fixture configuring NIC interfaces per test
        settings.
    :param test_time: Duration to run the streaming pipeline.
    :param test_config: Test configuration dictionary (e.g., interface type).
    :param prepare_ramdisk: Fixture preparing RAM disk storage for media files.
    :param pcap_capture: Fixture enabling optional packet capture for
        validation.
    :param media_file: Tuple fixture containing media metadata and file path.
    """
    media_file_info, media_file_path = media_file

    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF"),
    )

    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_st20p_sessions(
        config=config,
        nic_port_list=interfaces_list,
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
