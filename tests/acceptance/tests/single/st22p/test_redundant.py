# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation
"""ST 2022-7 as one st22p session with two independent member streams."""

import pytest
from common.nicctl import InterfaceSetup
from mtl_engine import ip_pools
from mtl_engine.media_files import yuv_files_422p10le

pytestmark = [pytest.mark.nightly]


@pytest.mark.parametrize("application", ["rxtxapp"])
@pytest.mark.parametrize(
    "media_file",
    [yuv_files_422p10le["Penguin_1080p"]],
    indirect=True,
    ids=["Penguin_1080p"],
)
def test_st22p_redundant(
    application,
    app_factory,
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_time,
    media_file,
):
    """A two-port st22p session must deliver the stream over both legs."""
    media_info, media_path = media_file
    host = list(hosts.values())[0]
    # Four VFs: the allocator fills PFs in order, so the first pair lands on
    # one PF for TX and the second on the other for RX.
    interfaces = setup_interfaces.get_interfaces_list_single("VF", count=4)
    # JPEG-XS plugin init adds 3-10s on top of MTL init.
    test_time = max(test_time, 90)

    app = app_factory(application)
    app.create_command(
        session_type="st22p",
        nic_port_list=interfaces,
        redundant=True,
        source_ip=ip_pools.tx[0],
        destination_ip=ip_pools.rx_multicast[0],
        source_ip_r=ip_pools.tx_r[0],
        destination_ip_r=ip_pools.rx_multicast[1],
        test_mode="multicast",
        input_file=media_path,
        width=media_info["width"],
        height=media_info["height"],
        framerate=f"p{media_info['fps']}",
        codec="JPEG-XS",
        quality="speed",
        pixel_format=media_info["file_format"],
        codec_threads=2,
        test_time=test_time,
    )
    app.execute_test(build=mtl_path, test_time=test_time, host=host)
    app.assert_redundant_paths()
