# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

import mtl_engine.RxTxApp as rxtxapp
import pytest
from common.nicctl import InterfaceSetup
from mtl_engine.media_files import yuv_files_422p10le, yuv_files_422rfc10


@pytest.mark.nightly
@pytest.mark.verified
@pytest.mark.parametrize(
    "media_file",
    list(yuv_files_422p10le.values()),
    indirect=["media_file"],
    ids=list(yuv_files_422p10le.keys()),
)
def test_422p10le(
    hosts,
    build,
    setup_interfaces: InterfaceSetup,
    test_time,
    test_config,
    prepare_ramdisk,
    pcap_capture,
    media_file,
):
    """
    Validate multicast ST20P for YUV422 planar 10-bit sources converted to
    transport format ``YUV_422_10bit`` and back. This ensures the session
    creation, multicast setup, and payload pacing succeed for representative
    10-bit planar assets, even though no pixel-level verification is
    performed here.

    :param hosts: Mapping of hosts available for the test run.
    :param build: Compiled Rx/Tx application artifact used for execution.
    :param setup_interfaces: Fixture configuring NIC interfaces per test
        settings.
    :param test_time: Duration to run the streaming pipeline.
    :param test_config: Test configuration dictionary (e.g., interface type).
    :param prepare_ramdisk: Fixture preparing RAM disk storage for media files.
    :param pcap_capture: Fixture enabling optional packet capture for
        validation.
    :param media_file: Tuple fixture containing media metadata and file path.
    """
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF"),
    )

    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_st20p_sessions(
        config=config,
        nic_port_list=interfaces_list,
        test_mode="multicast",
        width=media_file_info["width"],
        height=media_file_info["height"],
        fps=f"p{media_file_info['fps']}",
        input_format=media_file_info["file_format"],
        transport_format=media_file_info["format"],
        output_format=media_file_info["file_format"],
        st20p_url=media_file_path,
    )

    rxtxapp.execute_test(
        config=config,
        build=build,
        test_time=test_time,
        host=host,
        netsniff=pcap_capture,
    )


# List of supported formats based on st_frame_fmt_from_transport()
pixel_formats = dict(
    YUV_422_10bit=("ST20_FMT_YUV_422_10BIT", "YUV422RFC4175PG2BE10"),
    YUV_422_8bit=("ST20_FMT_YUV_422_8BIT", "UYVY"),
    YUV_422_12bit=("ST20_FMT_YUV_422_12BIT", "YUV422RFC4175PG2BE12"),
    YUV_444_10bit=("ST20_FMT_YUV_444_10BIT", "YUV444RFC4175PG4BE10"),
    YUV_444_12bit=("ST20_FMT_YUV_444_12BIT", "YUV444RFC4175PG2BE12"),
    YUV_420_8bit=("ST20_FMT_YUV_420_8BIT", "YUV420CUSTOM8"),
    RGB_8bit=("ST20_FMT_RGB_8BIT", "RGB8"),
    RGB_10bit=("ST20_FMT_RGB_10BIT", "RGBRFC4175PG4BE10"),
    RGB_12bit=("ST20_FMT_RGB_12BIT", "RGBRFC4175PG2BE12"),
    YUV_422_PLANAR10LE=("ST20_FMT_YUV_422_PLANAR10LE", "YUV422PLANAR10LE"),
    V210=("ST20_FMT_V210", "V210"),
)


# List of supported one-way convertions based on st_frame_get_converter()
convert1_formats = dict(
    UYVY="UYVY",
    YUV422PLANAR8="YUV422PLANAR8",
    YUV420PLANAR8="YUV420PLANAR8",
)


@pytest.mark.nightly
@pytest.mark.verified
@pytest.mark.parametrize(
    "media_file",
    [yuv_files_422rfc10["Penguin_1080p"]],
    indirect=["media_file"],
    ids=["Penguin_1080p"],
)
@pytest.mark.parametrize("format", convert1_formats.keys())
def test_convert_on_rx(
    hosts,
    build,
    setup_interfaces: InterfaceSetup,
    test_time,
    format,
    media_file,
    test_config,
):
    """
    Verify RX-side format conversion from ``YUV_422_10bit`` transport to each
    supported ``convert1_formats`` output (e.g., UYVY, planar 4:2:2/4:2:0
    8-bit) while transmitting multicast payloads. The test confirms the
    converter lookup and pipeline negotiation succeed; it does not yet
    compare decoded pixels.

    :param hosts: Mapping of hosts used to run the Rx/Tx pipeline.
    :param build: Compiled Rx/Tx application artifact used for execution.
    :param setup_interfaces: Fixture configuring NIC interfaces per test
        settings.
    :param test_time: Duration to run the streaming pipeline.
    :param format: Target RX output format selected from ``convert1_formats``.
    :param media_file: Tuple fixture containing media metadata and file path.
    :param test_config: Test configuration dictionary (e.g., interface type).
    """
    media_file_info, media_file_path = media_file
    output_format = convert1_formats[format]
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF"),
    )
    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_st20p_sessions(
        config=config,
        nic_port_list=interfaces_list,
        test_mode="multicast",
        packing="GPM",
        width=media_file_info["width"],
        height=media_file_info["height"],
        fps="p30",  # TODO: Hardcoded
        input_format="YUV422RFC4175PG2BE10",
        transport_format="YUV_422_10bit",
        output_format=output_format,
        st20p_url=media_file_path,
    )

    rxtxapp.execute_test(
        config=config,
        build=build,
        test_time=test_time,
        host=host,
    )


# List of supported two-way convertions based on st_frame_get_converter()
convert2_formats = dict(
    V210=("ST20_FMT_YUV_422_10BIT", "YUV_422_10bit", "YUV422RFC4175PG2BE10"),
    Y210=("ST20_FMT_YUV_422_10BIT", "YUV_422_10bit", "YUV422RFC4175PG2BE10"),
    YUV422PLANAR12LE=(
        "ST20_FMT_YUV_422_12BIT",
        "YUV_422_12bit",
        "YUV422RFC4175PG2BE12",
    ),
    YUV444PLANAR10LE=(
        "ST20_FMT_YUV_444_10BIT",
        "YUV_444_10bit",
        "YUV444RFC4175PG4BE10",
    ),
    YUV444PLANAR12LE=(
        "ST20_FMT_YUV_444_12BIT",
        "YUV_444_12bit",
        "YUV444RFC4175PG2BE12",
    ),
    GBRPLANAR10LE=("ST20_FMT_RGB_10BIT", "RGB_10bit", "RGBRFC4175PG4BE10"),
    GBRPLANAR12LE=("ST20_FMT_RGB_12BIT", "RGB_12bit", "RGBRFC4175PG2BE12"),
)


@pytest.mark.parametrize(
    "media_file",
    [yuv_files_422rfc10["test_8K"]],
    indirect=["media_file"],
    ids=["test_8K"],
)
@pytest.mark.parametrize("format", convert2_formats.keys())
def test_tx_rx_conversion(
    hosts,
    build,
    setup_interfaces: InterfaceSetup,
    test_config,
    test_time,
    format,
    media_file,
):
    """
    Exercise two-way conversions where TX encodes to the requested transport
    format and RX converts back to the same pixel layout defined in
    ``convert2_formats`` (e.g., V210/Y210 and 10/12-bit planar RGB/YUV). This
    validates that encoder and decoder selections coexist correctly under
    the chosen packing and frame size without asserting image fidelity.

    :param hosts: Mapping of hosts available for the test run.
    :param build: Compiled Rx/Tx application artifact used for execution.
    :param setup_interfaces: Fixture configuring NIC interfaces per test
        settings.
    :param test_config: Test configuration dictionary (e.g., interface type).
    :param test_time: Duration to run the streaming pipeline.
    :param format: Pixel format key used for both TX input and RX output.
    :param media_file: Tuple fixture containing media metadata and file path.
    """
    media_file_info, media_file_path = media_file
    text_format, transport_format, _ = convert2_formats[format]
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF"),
    )
    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_st20p_sessions(
        config=config,
        nic_port_list=interfaces_list,
        test_mode="multicast",
        packing="GPM",
        width=media_file_info["width"],
        height=media_file_info["height"],
        fps=f"p{media_file_info['fps']}",
        input_format=format,
        transport_format=transport_format,
        output_format=format,
        st20p_url=media_file_path,
    )

    rxtxapp.execute_test(
        config=config,
        build=build,
        test_time=test_time,
        host=host,
    )


@pytest.mark.nightly
@pytest.mark.parametrize(
    "media_file",
    [yuv_files_422rfc10["test_8K"]],
    indirect=["media_file"],
    ids=["test_8K"],
)
@pytest.mark.verified
@pytest.mark.parametrize("format", pixel_formats.keys())
def test_formats(
    hosts,
    build,
    setup_interfaces: InterfaceSetup,
    test_time,
    format,
    test_config,
    prepare_ramdisk,
    media_file,
):
    """
    Sanity-check that each supported pixel format in ``pixel_formats`` can
    traverse the ST20P multicast pipeline without additional conversions.
    This covers a mix of YUV and RGB bit depths to catch configuration or
    caps negotiation issues across the supported matrix of frame formats.

    :param hosts: Mapping of hosts available for the test run.
    :param build: Compiled Rx/Tx application artifact used for execution.
    :param setup_interfaces: Fixture configuring NIC interfaces per test
        settings.
    :param test_time: Duration to run the streaming pipeline.
    :param format: Pixel format key drawn from ``pixel_formats``.
    :param test_config: Test configuration dictionary (e.g., interface type).
    :param prepare_ramdisk: Fixture preparing RAM disk storage for media files.
    :param media_file: Tuple fixture containing media metadata and file path.
    """
    media_file_info, media_file_path = media_file
    text_format, file_format = pixel_formats[format]
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF"),
    )

    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_st20p_sessions(
        config=config,
        nic_port_list=interfaces_list,
        test_mode="multicast",
        packing="GPM",
        width=media_file_info["width"],
        height=media_file_info["height"],
        fps=f"p{media_file_info['fps']}",
        input_format=file_format,
        transport_format=format,
        output_format=file_format,
        st20p_url=media_file_path,
    )

    rxtxapp.execute_test(
        config=config,
        build=build,
        test_time=test_time,
        host=host,
    )
