# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation
"""Refactored: st40p (ancillary pipeline) basic single-host test.

Mirrors ``test_basic.py`` (legacy session API ancillary type_mode test) but
uses the unified ``rxtxapp`` fixture (``session_type="st40p"``).
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
        anc_files["text_p59"],
    ],
    indirect=["media_file"],
    ids=[
        "text_p29",
        "text_p50",
        "text_p59",
    ],
)
@pytest.mark.refactored
def test_st40p_basic_refactored(
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_time,
    test_config,
    prepare_ramdisk,
    media_file,
    rxtxapp,
):
    """Smoke test: TX st40p -> RX st40p over the pipeline ancillary API (unicast)."""
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )

    rxtxapp.create_command(
        session_type="st40p",
        nic_port_list=interfaces_list,
        test_mode="unicast",
        framerate=media_file_info["fps"],
        input_file=media_file_path,
        test_time=test_time,
    )

    rxtxapp.execute_test(
        build=mtl_path,
        test_time=test_time,
        host=host,
    )
