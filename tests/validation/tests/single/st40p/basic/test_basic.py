# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation
"""Basic single-host tests for the st40p (ancillary pipeline) RX path.

Replaces the legacy ``ancillary/type_mode`` tests that exercised the
session API. The pipeline API (``st40p_*``) does not have an "rtp" type
mode, so only the frame-equivalent path is covered here.
"""
import mtl_engine.RxTxApp as rxtxapp
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
def test_st40p_basic(
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_time,
    test_config,
    prepare_ramdisk,
    media_file,
):
    """Smoke test: TX st40p -> RX st40p over the pipeline ancillary API (unicast)."""
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )

    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_st40p_sessions(
        config=config,
        nic_port_list=interfaces_list,
        test_mode="unicast",
        fps=media_file_info["fps"],
        st40p_url=media_file_path,
    )

    rxtxapp.execute_test(
        config=config,
        build=mtl_path,
        test_time=test_time,
        host=host,
    )


@pytest.mark.nightly
@pytest.mark.parametrize(
    "media_file",
    [anc_files["text_p59"]],
    indirect=["media_file"],
    ids=["text_p59"],
)
def test_st40p_rtcp(
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_time,
    test_config,
    prepare_ramdisk,
    media_file,
):
    """Verify RX st40p path accepts the RTCP feedback flag (pipeline-API extra)."""
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )

    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_st40p_sessions(
        config=config,
        nic_port_list=interfaces_list,
        test_mode="unicast",
        fps=media_file_info["fps"],
        st40p_url=media_file_path,
        enable_rtcp=True,
    )

    rxtxapp.execute_test(
        config=config,
        build=mtl_path,
        test_time=test_time,
        host=host,
    )
