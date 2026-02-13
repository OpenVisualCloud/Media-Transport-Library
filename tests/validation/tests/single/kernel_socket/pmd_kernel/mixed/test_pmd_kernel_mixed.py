# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation
"""PMD+Kernel Mixed Media Tests.

This module tests hybrid network backend configurations with multiple ST2110 stream
types (video, audio, and ancillary data) using different networking backends on the
same system: DPDK PMD for transmit and kernel socket for receive.

Test Purpose
------------
Validate that MTL can successfully transmit and receive multiple ST2110 stream types
simultaneously when using different networking backends:

- **ST2110-20 Video**: Uncompressed video streams
- **ST2110-30 Audio**: PCM audio streams (24-bit)
- **ST2110-40 Ancillary**: Ancillary data streams

All streams use:

- **TX (Transmit)**: DPDK PMD backend via VF for high-performance transmission
- **RX (Receive)**: Kernel socket backend via native interface for reception

This tests MTL's ability to handle complex mixed-media workflows with hybrid
networking configurations, simulating real-world broadcast scenarios where multiple
signal types must be synchronized.

Test Methodology
----------------
1. Create VF on first interface and bind to DPDK (vfio-pci driver) for TX
2. Use second interface with kernel driver for RX (kernel socket mode)
3. Configure multiple ST2110 sessions:

   - ST2110-20: Video sessions with specified format and frame rate
   - ST2110-30: Audio sessions with PCM24 format
   - ST2110-40: Ancillary data sessions

4. Transmit all streams simultaneously via DPDK PMD interface
5. Receive and validate all streams via kernel socket interface
6. Verify frame rates, packet counts, and data integrity for each stream type
7. Check synchronization between video, audio, and ancillary streams

Topology Requirements
---------------------
Requires at least 2 network interfaces configured in topology_config.yaml:

- First interface: Used for DPDK VF creation (TX path)
- Second interface: Used for kernel socket (RX path)

Both interfaces should be on the same NIC (same PCI bus) for proper connectivity.
If only one interface is configured, tests will be skipped.
"""
import mtl_engine.RxTxApp as rxtxapp
import pytest
from common.nicctl import InterfaceSetup
from mtl_engine.media_files import (
    anc_files,
    audio_files,
    parse_fps_to_pformat,
    yuv_files,
)


@pytest.mark.nightly
@pytest.mark.parametrize("test_mode", ["multicast"])
@pytest.mark.parametrize(
    "media_file",
    [yuv_files["i1080p59"]],
    indirect=["media_file"],
    ids=["i1080p59"],
)
@pytest.mark.parametrize("replicas", [1, 4])
def test_pmd_kernel_mixed_format(
    hosts,
    mtl_path,
    media,
    setup_interfaces: InterfaceSetup,
    test_time,
    test_mode,
    replicas,
    test_config,
    media_file,
):
    """Test mixed ST2110 streams (video, audio, ancillary) using DPDK PMD (TX) and kernel socket (RX).

    :param hosts: Dictionary of host objects from topology configuration
    :type hosts: dict
    :param build: Path to MTL build directory
    :type build: str
    :param media: Path to media files directory containing audio and ancillary files
    :type media: str
    :param setup_interfaces: Interface setup helper for network configuration
    :type setup_interfaces: InterfaceSetup
    :param test_time: Duration to run the test in seconds
    :type test_time: int
    :param test_mode: Network mode for testing (multicast/unicast)
    :type test_mode: str
    :param replicas: Number of concurrent mixed-media session sets to create
    :type replicas: int
    :param test_config: Test configuration dictionary from test_config.yaml
    :type test_config: dict
    :param media_file: Media file fixture (video file info and path)
    :type media_file: tuple

    :raises pytest.skip: If less than 2 network interfaces configured in topology
    """
    media_file_info, media_file_path = media_file
    audio_file = audio_files["PCM24"]
    ancillary_file = anc_files["text_p50"]
    host = list(hosts.values())[0]

    # Get hybrid interface list: one DPDK (VF/PF) and one kernel socket
    interfaces_list = setup_interfaces.get_pmd_kernel_interfaces(
        test_config.get("interface_type", "VF")
    )

    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_st20p_sessions(
        config=config,
        nic_port_list=interfaces_list,
        test_mode=test_mode,
        width=media_file_info["width"],
        height=media_file_info["height"],
        fps=parse_fps_to_pformat(media_file_info["fps"]),
        input_format=media_file_info["file_format"],
        transport_format=media_file_info["format"],
        output_format=media_file_info["file_format"],
        st20p_url=media_file_path,
    )
    config = rxtxapp.change_replicas(
        config=config, session_type="st20p", replicas=replicas
    )
    config = rxtxapp.add_st30p_sessions(
        config=config,
        nic_port_list=interfaces_list,
        test_mode=test_mode,
        audio_format="PCM24",
        audio_channel=["U02"],
        audio_sampling="48kHz",
        audio_ptime="1",
        filename=str(host.connection.path(media) / audio_file["filename"]),
        out_url=str(
            host.connection.path(media_file_path).parent
            / ("out_" + audio_file["filename"])
        ),
    )
    config = rxtxapp.change_replicas(
        config=config, session_type="st30p", replicas=replicas
    )
    config = rxtxapp.add_ancillary_sessions(
        config=config,
        nic_port_list=interfaces_list,
        test_mode=test_mode,
        type_="frame",
        ancillary_format="closed_caption",
        ancillary_fps=ancillary_file["fps"],
        ancillary_url=str(host.connection.path(media) / ancillary_file["filename"]),
    )
    # rxtxapp.check_and_set_ip('eth2')
    config = rxtxapp.change_replicas(
        config=config, session_type="ancillary", replicas=replicas
    )
    rxtxapp.execute_test(
        config=config,
        build=mtl_path,
        test_time=test_time,
        host=host,
        interface_setup=setup_interfaces,
    )
