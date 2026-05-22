# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

import pytest
from common.nicctl import InterfaceSetup
from mtl_engine.media_files import anc_files


@pytest.mark.nightly
@pytest.mark.parametrize(
    "application",
    [
        "rxtxapp",
        pytest.param(
            "ffmpeg",
            marks=pytest.mark.skip(reason="FFmpeg does not support st40p ancillary data pipeline"),
        ),
    ],
)
@pytest.mark.parametrize(
    "media_file",
    [
        anc_files["text_p29"],
        anc_files["text_p50"],
        anc_files["text_p59"],
    ],
    indirect=["media_file"],
    ids=["text_p29", "text_p50", "text_p59"],
)
def test_st40p_basic(
    application,
    app_factory,
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_time,
    test_config,
    media_file,
    pcap_capture,
):
    """Smoke test: TX st40p -> RX st40p over the pipeline ancillary API (unicast)."""
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(test_config.get("interface_type", "VF"))

    app = app_factory(application)
    app.create_command(
        session_type="st40p",
        nic_port_list=interfaces_list,
        test_mode="unicast",
        framerate=media_file_info["fps"],
        input_file=media_file_path,
        test_time=test_time,
    )

    app.execute_test(build=mtl_path, test_time=test_time, host=host, netsniff=pcap_capture)


@pytest.mark.nightly
@pytest.mark.parametrize(
    "application",
    [
        "rxtxapp",
        pytest.param(
            "ffmpeg",
            marks=pytest.mark.skip(reason="FFmpeg does not support st40p ancillary data pipeline"),
        ),
    ],
)
@pytest.mark.parametrize(
    "media_file",
    [
        anc_files["text_p29"],
        anc_files["text_p50"],
        pytest.param(anc_files["text_p59"], marks=pytest.mark.smoke),
    ],
    indirect=["media_file"],
    ids=["text_p29", "text_p50", "text_p59"],
)
def test_st40p_multicast_with_compliance(
    application,
    app_factory,
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_time,
    test_config,
    pcap_capture,
    media_file,
):
    """Test st40p multicast with EBU compliance check."""
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(test_config.get("interface_type", "VF"))
    # EBU compliance verdict needs many ancillary packets to classify.
    test_time = max(test_time, 90)

    app = app_factory(application)
    app.create_command(
        session_type="st40p",
        nic_port_list=interfaces_list,
        test_mode="multicast",
        framerate=media_file_info["fps"],
        input_file=media_file_path,
        test_time=test_time,
    )

    app.execute_test(
        build=mtl_path,
        test_time=test_time,
        host=host,
        netsniff=pcap_capture,
    )


@pytest.mark.nightly
@pytest.mark.parametrize(
    "application",
    [
        "rxtxapp",
        pytest.param(
            "ffmpeg",
            marks=pytest.mark.skip(reason="FFmpeg does not support st40p ancillary data pipeline"),
        ),
    ],
)
@pytest.mark.parametrize(
    "media_file",
    [anc_files["text_p59"]],
    indirect=["media_file"],
    ids=["text_p59"],
)
def test_st40p_rtcp(
    application,
    app_factory,
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_time,
    test_config,
    media_file,
    pcap_capture,
):
    """Verify st40p path accepts the RTCP feedback flag (pipeline-API extra)."""
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(test_config.get("interface_type", "VF"))

    app = app_factory(application)
    app.create_command(
        session_type="st40p",
        nic_port_list=interfaces_list,
        test_mode="unicast",
        framerate=media_file_info["fps"],
        input_file=media_file_path,
        enable_rtcp=True,
        test_time=test_time,
    )

    app.execute_test(
        build=mtl_path,
        test_time=test_time,
        host=host,
        netsniff=pcap_capture,
    )
