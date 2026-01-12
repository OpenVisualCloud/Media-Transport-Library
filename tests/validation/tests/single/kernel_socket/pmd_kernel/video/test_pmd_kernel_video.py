# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation
"""PMD+Kernel Video Tests.

This module tests hybrid network backend configurations where video streams use
different networking backends on the same system: DPDK PMD (Poll Mode Driver) for
transmit and kernel socket for receive.

Test Purpose
------------
Validate that MTL can successfully transmit and receive ST2110-20 video streams
when using different networking backends in the same test run:

- **TX (Transmit)**: Uses DPDK PMD backend via VF (Virtual Function) for high-performance,
  kernel-bypass transmission
- **RX (Receive)**: Uses kernel socket backend via native interface for standard Linux
  networking stack reception

This configuration tests MTL's flexibility in supporting mixed deployment scenarios
where some interfaces use DPDK for maximum performance while others use kernel
networking for compatibility or resource constraints.

Test Methodology
----------------
1. Create VF on first interface and bind to DPDK (vfio-pci driver) for TX
2. Use second interface with kernel driver for RX (kernel socket mode)
3. Configure ST2110-20 video sessions with specified format and frame rate
4. Transmit video frames via DPDK PMD interface
5. Receive and validate video frames via kernel socket interface
6. Verify frame rate, frame count, and stream integrity

Topology Requirements
---------------------
Requires at least 2 network interfaces configured in topology_config.yaml:

- First interface: Used for DPDK VF creation (TX path)
- Second interface: Used for kernel socket (RX path)

Both interfaces should be on the same NIC (same PCI bus) for proper connectivity.
If only one interface is configured, tests will be skipped.
"""
import os

import mtl_engine.RxTxApp as rxtxapp
import pytest
from common.nicctl import InterfaceSetup
from mtl_engine.media_files import parse_fps_to_pformat, yuv_files


@pytest.mark.nightly
@pytest.mark.parametrize("test_mode", ["multicast"])
@pytest.mark.parametrize(
    "video_format", ["i1080p59"]
)  # Note: i2160p59 excluded - kernel socket cannot handle 4K@59fps bandwidth (10.4Gbps)
@pytest.mark.parametrize("replicas", [1, 2])
def test_pmd_kernel_video_format(
    hosts,
    build,
    media,
    setup_interfaces: InterfaceSetup,
    test_time,
    test_mode,
    video_format,
    replicas,
    test_config,
    prepare_ramdisk,
):
    """Test ST2110-20 video transmission using DPDK PMD (TX) and kernel socket (RX).

    Note: This test uses kernel socket for RX which has bandwidth limitations.
    4K formats (i2160p59 @ 10.4 Gbps) are excluded as kernel socket backend
    cannot reliably process packets at that rate, causing packet loss and FPS drops.

    :param hosts: Dictionary of host objects from topology configuration
    :type hosts: dict
    :param build: Path to MTL build directory
    :type build: str
    :param media: Path to media files directory
    :type media: str
    :param setup_interfaces: Interface setup helper for network configuration
    :type setup_interfaces: InterfaceSetup
    :param test_time: Duration to run the test in seconds
    :type test_time: int
    :param test_mode: Network mode for testing (multicast/unicast)
    :type test_mode: str
    :param video_format: Video format identifier (e.g., 'i1080p59', 'i2160p59')
    :type video_format: str
    :param replicas: Number of concurrent video sessions to create
    :type replicas: int
    :param test_config: Test configuration dictionary from test_config.yaml
    :type test_config: dict
    :param prepare_ramdisk: Ramdisk preparation fixture (if enabled)
    :type prepare_ramdisk: object or None

    :raises pytest.skip: If less than 2 network interfaces configured in topology
    """

    video_file = yuv_files[video_format]

    # rxtxapp.check_and_bind_interface(["0000:38:00.0","0000:38:00.1"], "pmd")
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
        width=video_file["width"],
        height=video_file["height"],
        fps=parse_fps_to_pformat(video_file["fps"]),
        input_format=video_file["file_format"],
        transport_format=video_file["format"],
        output_format=video_file["file_format"],
        st20p_url=os.path.join(media, video_file["filename"]),
    )
    # rxtxapp.check_and_set_ip('eth2')
    config = rxtxapp.change_replicas(
        config=config, session_type="st20p", replicas=replicas
    )
    rxtxapp.execute_test(
        config=config,
        build=build,
        test_time=test_time * 2,
        host=host,
        interface_setup=setup_interfaces,
    )
