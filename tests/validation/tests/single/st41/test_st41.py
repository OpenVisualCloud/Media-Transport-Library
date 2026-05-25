# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

import pytest
from common.nicctl import InterfaceSetup
from mtl_engine.media_files import st41_files

# Shared parametrize-id-to-value mappings for ST2110-41 (fastmetadata) tests.
payload_type_mapping = {
    "pt115": 115,
    "pt120": 120,
}

dit_mapping = {
    "dit0": 3648364,
    "dit1": 1234567,
}

k_bit_mapping = {
    "k0": 0,
    "k1": 1,
}


@pytest.mark.nightly
@pytest.mark.parametrize(
    "application",
    [
        "rxtxapp",
        pytest.param(
            "ffmpeg",
            marks=pytest.mark.skip(
                reason="FFmpeg does not support st41 fast metadata pipeline"
            ),
        ),
    ],
)
@pytest.mark.parametrize(
    "media_file",
    [st41_files["st41_p29_long_file"]],
    indirect=["media_file"],
    ids=["st41_p29_long_file"],
)
@pytest.mark.parametrize("dit", ["dit0", "dit1"])
def test_st41_dit(
    application,
    app_factory,
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_time,
    dit,
    test_config,
    media_file,
    pcap_capture,
):
    """Test the Data Item Type (DIT) fastmetadata_data_item_type."""
    _, media_file_path = media_file
    payload_type = payload_type_mapping["pt115"]
    k_bit = k_bit_mapping["k0"]

    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )

    app = app_factory(application)
    app.create_command(
        session_type="fastmetadata",
        nic_port_list=interfaces_list,
        test_mode="unicast",
        type_mode="rtp",
        payload_type=payload_type,
        fastmetadata_data_item_type=dit_mapping[dit],
        fastmetadata_k_bit=k_bit,
        fastmetadata_fps="p59",
        input_file=media_file_path,
        test_time=test_time,
    )

    app.execute_test(
        build=mtl_path, test_time=test_time, host=host, netsniff=pcap_capture
    )


@pytest.mark.nightly
@pytest.mark.parametrize(
    "application",
    [
        "rxtxapp",
        pytest.param(
            "ffmpeg",
            marks=pytest.mark.skip(
                reason="FFmpeg does not support st41 fast metadata pipeline"
            ),
        ),
    ],
)
@pytest.mark.parametrize(
    "media_file",
    [st41_files["st41_p29_long_file"]],
    indirect=["media_file"],
    ids=["st41_p29_long_file"],
)
@pytest.mark.parametrize(
    "fps",
    ["p23", "p24", "p25", "p29", "p30", "p50", "p59", "p60", "p100", "p119", "p120"],
)
def test_st41_fps(
    application,
    app_factory,
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_time,
    fps,
    test_config,
    media_file,
    pcap_capture,
):
    """Test st41 with different frame rates."""
    _, media_file_path = media_file
    payload_type = payload_type_mapping["pt115"]
    dit = dit_mapping["dit0"]
    k_bit = k_bit_mapping["k0"]

    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )

    app = app_factory(application)
    app.create_command(
        session_type="fastmetadata",
        nic_port_list=interfaces_list,
        test_mode="unicast",
        type_mode="rtp",
        payload_type=payload_type,
        fastmetadata_data_item_type=dit,
        fastmetadata_k_bit=k_bit,
        fastmetadata_fps=fps,
        input_file=media_file_path,
        test_time=test_time,
    )

    app.execute_test(
        build=mtl_path, test_time=test_time, host=host, netsniff=pcap_capture
    )


@pytest.mark.nightly
@pytest.mark.parametrize(
    "application",
    [
        "rxtxapp",
        pytest.param(
            "ffmpeg",
            marks=pytest.mark.skip(
                reason="FFmpeg does not support st41 fast metadata pipeline"
            ),
        ),
    ],
)
@pytest.mark.parametrize(
    "media_file",
    [st41_files["st41_p29_long_file"]],
    indirect=["media_file"],
    ids=["st41_p29_long_file"],
)
@pytest.mark.parametrize("k_bit", ["k0", "k1"])
def test_st41_k_bit(
    application,
    app_factory,
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_time,
    k_bit,
    test_config,
    media_file,
    pcap_capture,
):
    """Test st41 with different k-bit values."""
    _, media_file_path = media_file
    payload_type = payload_type_mapping["pt115"]
    dit = dit_mapping["dit0"]

    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )

    app = app_factory(application)
    app.create_command(
        session_type="fastmetadata",
        nic_port_list=interfaces_list,
        test_mode="unicast",
        type_mode="rtp",
        payload_type=payload_type,
        fastmetadata_data_item_type=dit,
        fastmetadata_k_bit=k_bit_mapping[k_bit],
        fastmetadata_fps="p59",
        input_file=media_file_path,
        test_time=test_time,
    )

    app.execute_test(
        build=mtl_path, test_time=test_time, host=host, netsniff=pcap_capture
    )


@pytest.mark.nightly
@pytest.mark.parametrize(
    "application",
    [
        "rxtxapp",
        pytest.param(
            "ffmpeg",
            marks=pytest.mark.skip(
                reason="FFmpeg does not support st41 fast metadata pipeline"
            ),
        ),
    ],
)
@pytest.mark.parametrize(
    "media_file",
    [st41_files["st41_p29_long_file"]],
    indirect=["media_file"],
    ids=["st41_p29_long_file"],
)
@pytest.mark.parametrize("type_mode", ["rtp", "frame"])
def test_st41_no_chain(
    application,
    app_factory,
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_time,
    type_mode,
    test_config,
    media_file,
    pcap_capture,
):
    """Test st41 with tx_no_chain=True."""
    _, media_file_path = media_file
    payload_type = payload_type_mapping["pt115"]
    k_bit = k_bit_mapping["k0"]
    dit = dit_mapping["dit0"]

    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )

    app = app_factory(application)
    app.create_command(
        session_type="fastmetadata",
        nic_port_list=interfaces_list,
        test_mode="unicast",
        type_mode=type_mode,
        tx_no_chain=True,
        payload_type=payload_type,
        fastmetadata_data_item_type=dit,
        fastmetadata_k_bit=k_bit,
        fastmetadata_fps="p59",
        input_file=media_file_path,
        test_time=test_time,
    )

    app.execute_test(
        build=mtl_path, test_time=test_time, host=host, netsniff=pcap_capture
    )


@pytest.mark.nightly
@pytest.mark.parametrize(
    "application",
    [
        "rxtxapp",
        pytest.param(
            "ffmpeg",
            marks=pytest.mark.skip(
                reason="FFmpeg does not support st41 fast metadata pipeline"
            ),
        ),
    ],
)
@pytest.mark.parametrize(
    "media_file",
    [st41_files["st41_p29_long_file"]],
    indirect=["media_file"],
    ids=["st41_p29_long_file"],
)
@pytest.mark.parametrize("payload_type", ["pt115", "pt120"])
@pytest.mark.parametrize("type_mode", ["rtp", "frame"])
def test_st41_payload_type(
    application,
    app_factory,
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_time,
    payload_type,
    type_mode,
    test_config,
    media_file,
    pcap_capture,
):
    """Test st41 with different payload types."""
    _, media_file_path = media_file
    dit = dit_mapping["dit0"]
    k_bit = k_bit_mapping["k0"]

    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )

    app = app_factory(application)
    app.create_command(
        session_type="fastmetadata",
        nic_port_list=interfaces_list,
        test_mode="unicast",
        type_mode=type_mode,
        payload_type=payload_type_mapping[payload_type],
        fastmetadata_data_item_type=dit,
        fastmetadata_k_bit=k_bit,
        fastmetadata_fps="p59",
        input_file=media_file_path,
        test_time=test_time,
    )

    app.execute_test(
        build=mtl_path, test_time=test_time, host=host, netsniff=pcap_capture
    )


@pytest.mark.nightly
@pytest.mark.parametrize(
    "application",
    [
        "rxtxapp",
        pytest.param(
            "ffmpeg",
            marks=pytest.mark.skip(
                reason="FFmpeg does not support st41 fast metadata pipeline"
            ),
        ),
    ],
)
@pytest.mark.parametrize(
    "media_file",
    [st41_files["st41_p29_long_file"]],
    indirect=["media_file"],
    ids=["st41_p29_long_file"],
)
@pytest.mark.parametrize("test_mode", ["unicast", "multicast"])
@pytest.mark.parametrize("type_mode", ["rtp", "frame"])
def test_st41_type_mode(
    application,
    app_factory,
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_time,
    test_mode,
    type_mode,
    test_config,
    media_file,
    pcap_capture,
):
    """Test st41 with different transmission modes (unicast, multicast)."""
    _, media_file_path = media_file
    payload_type = payload_type_mapping["pt115"]
    k_bit = k_bit_mapping["k0"]
    dit = dit_mapping["dit0"]

    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )

    app = app_factory(application)
    app.create_command(
        session_type="fastmetadata",
        nic_port_list=interfaces_list,
        test_mode=test_mode,
        type_mode=type_mode,
        payload_type=payload_type,
        fastmetadata_data_item_type=dit,
        fastmetadata_k_bit=k_bit,
        fastmetadata_fps="p59",
        input_file=media_file_path,
        test_time=test_time,
    )

    app.execute_test(
        build=mtl_path, test_time=test_time, host=host, netsniff=pcap_capture
    )
