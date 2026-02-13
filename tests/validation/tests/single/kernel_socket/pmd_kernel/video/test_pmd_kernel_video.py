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
import mtl_engine.RxTxApp as rxtxapp
import pytest
from common.nicctl import InterfaceSetup
from mtl_engine.media_files import parse_fps_to_pformat, yuv_files


@pytest.mark.nightly
@pytest.mark.parametrize("test_mode", ["multicast"])
@pytest.mark.parametrize(
    "media_file",
    [yuv_files["i1080p59"]],
    indirect=["media_file"],
    ids=["i1080p59"],
)  # Note: i2160p59 excluded - kernel socket cannot handle 4K@59fps bandwidth (10.4Gbps)
@pytest.mark.parametrize("replicas", [1, 2])
def test_pmd_kernel_video_format(
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_time,
    test_mode,
    replicas,
    test_config,
    media_file,
):
    """Test ST2110-20 video transmission using DPDK PMD (TX) and kernel socket (RX).

    Note: This test uses kernel socket for RX which has bandwidth limitations.
    4K formats (i2160p59 @ 10.4 Gbps) are excluded as kernel socket backend
    cannot reliably process packets at that rate, causing packet loss and FPS drops.

    :param hosts: Dictionary of host objects from topology configuration
    :type hosts: dict
    :param build: Path to MTL build directory
    :type build: str
    :param setup_interfaces: Interface setup helper for network configuration
    :type setup_interfaces: InterfaceSetup
    :param test_time: Duration to run the test in seconds
    :type test_time: int
    :param test_mode: Network mode for testing (multicast/unicast)
    :type test_mode: str
    :param replicas: Number of concurrent video sessions to create
    :type replicas: int
    :param test_config: Test configuration dictionary from test_config.yaml
    :type test_config: dict
    :param media_file: Media file fixture (video file info and path)
    :type media_file: tuple

    :raises pytest.skip: If less than 2 network interfaces configured in topology
    """
    media_file_info, media_file_path = media_file
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
    # rxtxapp.check_and_set_ip('eth2')
    config = rxtxapp.change_replicas(
        config=config, session_type="st20p", replicas=replicas
    )
    rxtxapp.execute_test(
        config=config,
        build=mtl_path,
        test_time=test_time * 2,
        host=host,
        interface_setup=setup_interfaces,
    )
