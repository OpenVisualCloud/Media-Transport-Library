# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

import re

import pytest
from common.nicctl import InterfaceSetup
from mtl_engine import ip_pools
from mtl_engine.dma import get_numa_node_from_pci
from mtl_engine.media_files import yuv_files_422p10le

pytestmark = pytest.mark.verified

# The RX video session got a DMA device (rv_init_dma in st_rx_video_session.c).
RX_DMA_SESSION = re.compile(r"rv_init_dma\(\d+\), succ")


def _run(host, command: str) -> str:
    return host.connection.execute_command(command, shell=True).stdout.strip()


def _driver(host, bdf: str) -> str:
    return _run(host, f"basename $(readlink /sys/bus/pci/devices/{bdf}/driver)")


def _skip_without_dma_on_port_node(host) -> str:
    """Skip unless the bound DMA device is on the NUMA node of the port.

    setup_host_dma falls back to a device on another node, but the auto probe
    only takes devices on the node of a port. Return the port BDF.
    """
    dma = getattr(host, "dma_device", None)
    if not dma:
        pytest.skip("no DMA device on vfio-pci")
    port = host.network_interfaces[0].pci_address.lspci
    if get_numa_node_from_pci(host, dma.split(",")[0]) != get_numa_node_from_pci(
        host, port
    ):
        pytest.skip(f"DMA device {dma} is not on the NUMA node of {port}")
    return port


@pytest.mark.smoke
@pytest.mark.nightly
@pytest.mark.parametrize("application", ["rxtxapp", "ffmpeg", "gstreamer"])
@pytest.mark.parametrize(
    "media_file",
    [yuv_files_422p10le["Penguin_1080p"]],
    indirect=["media_file"],
    ids=["Penguin_1080p"],
)
def test_dma_auto_st20p(
    application,
    app_factory,
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_time,
    test_config,
    output_files,
    media_integrity,
    media_file,
):
    """With --dma_auto, the st20p RX copies with a DMA device of its NUMA node."""
    host = next(iter(hosts.values()))
    _skip_without_dma_on_port_node(host)

    media_file_info, media_file_path = media_file
    app = app_factory(application)
    app.create_command(
        session_type="st20p",
        nic_port_list=setup_interfaces.get_interfaces_list_single(
            test_config.get("interface_type", "VF")
        ),
        test_mode="multicast",
        destination_ip=ip_pools.rx_multicast[0],
        port=20000,
        width=media_file_info["width"],
        height=media_file_info["height"],
        framerate=f"p{media_file_info['fps']}",
        pixel_format=media_file_info["file_format"],
        transport_format=media_file_info["format"],
        input_file=media_file_path,
        output_file=output_files.register(f"{media_file_path}.out"),
        test_time=test_time,
    )
    assert app.params["dma_auto"], "app_factory did not turn on dma_auto"

    app.execute_test(
        build=mtl_path, test_time=test_time, host=host, integrity=media_integrity
    )

    assert RX_DMA_SESSION.search(
        app.last_output or ""
    ), "the RX video session did not get a DMA device"


@pytest.mark.smoke
@pytest.mark.nightly
def test_dma_setup_get(hosts, mtl_path, host_dma_devices):
    """dma_setup.sh get prints DMA devices on vfio-pci on the NUMA node of the port."""
    host = next(iter(hosts.values()))
    port = _skip_without_dma_on_port_node(host)
    devices = _run(host, f"{mtl_path}/script/dma_setup.sh get {port}").split(",")

    assert host.dma_device.split(",")[0] in devices
    for dev in devices:
        assert get_numa_node_from_pci(host, dev) == get_numa_node_from_pci(
            host, port
        ), f"{dev} is on another node"
        assert _driver(host, dev) == "vfio-pci", f"{dev} is not on vfio-pci"
