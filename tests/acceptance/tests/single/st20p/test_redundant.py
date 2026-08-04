# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation
"""ST 2022-7 as one st20p session with two independent member streams."""

import pytest
from common.nicctl import InterfaceSetup
from mtl_engine import ip_pools
from mtl_engine.media_files import parse_fps_to_pformat, yuv_files

pytestmark = [pytest.mark.verified, pytest.mark.nightly]


@pytest.mark.parametrize("application", ["rxtxapp"])
@pytest.mark.parametrize(
    "media_file", [yuv_files["i1080p59"]], indirect=True, ids=["i1080p59"]
)
def test_st20p_redundant(
    application,
    app_factory,
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_time,
    pcap_capture,
    output_files,
    media_integrity,
    media_file,
):
    """A two-port session must deliver a byte-exact, compliant stream."""
    media_info, media_path = media_file
    host = list(hosts.values())[0]
    # Four VFs: the allocator fills PFs in order, so the first pair lands on
    # one PF for TX and the second on the other for RX.
    interfaces = setup_interfaces.get_interfaces_list_single("VF", count=4)

    app = app_factory(application)
    app.create_command(
        session_type="st20p",
        nic_port_list=interfaces,
        redundant=True,
        source_ip=ip_pools.tx[0],
        destination_ip=ip_pools.rx_multicast[0],
        source_ip_r=ip_pools.tx_r[0],
        destination_ip_r=ip_pools.rx_multicast[1],
        port=20000,
        test_mode="multicast",
        input_file=media_path,
        output_file=output_files.register(f"{media_path}.redundant.out"),
        width=media_info["width"],
        height=media_info["height"],
        framerate=parse_fps_to_pformat(media_info["fps"]),
        transport_format=media_info["format"],
        pixel_format=media_info["file_format"],
        test_time=test_time,
    )
    app.execute_test(
        build=mtl_path,
        test_time=test_time,
        host=host,
        compliance=pcap_capture,
        integrity=media_integrity,
    )
