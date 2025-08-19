# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation
import os

import mtl_engine.RxTxApp as rxtxapp
import pytest
from mtl_engine.media_files import yuv_files_422p10le, yuv_files_422rfc10

@pytest.mark.nightly
@pytest.mark.parametrize("file", yuv_files_422p10le.keys())
def test_422p10le(
    hosts,
    build,
    media,
    nic_port_list,
    test_time,
    file,
    test_config,
    prepare_ramdisk,
):
    """
    Send files in YUV422PLANAR10LE format converting to transport format YUV_422_10bit
    """
    st20p_file = yuv_files_422p10le[file]
    host = list(hosts.values())[0]

    # Get capture configuration from test_config.yaml
    # This controls whether tcpdump capture is enabled, where to store the pcap, etc.
    capture_cfg = dict(test_config.get("capture_cfg", {}))
    capture_cfg["test_name"] = f"test_format_{file}"  # Set a unique pcap file name

    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_st20p_sessions(
        config=config,
        nic_port_list=host.vfs,
        test_mode="multicast",
        width=st20p_file["width"],
        height=st20p_file["height"],
        fps=f"p{st20p_file['fps']}",
        input_format=st20p_file["file_format"],
        transport_format=st20p_file["format"],
        output_format=st20p_file["file_format"],
        st20p_url=os.path.join(media, st20p_file["filename"]),
    )

    rxtxapp.execute_test(
        config=config,
        build=build,
        test_time=test_time,
        host=host,
        capture_cfg=capture_cfg,
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


@pytest.mark.parametrize("format", convert1_formats.keys())
def convert_on_rx(
    hosts,
    build,
    media,
    nic_port_list,
    test_time,
    format,
):
    """
    Send file in YUV_422_10bit pixel formats with supported convertion on RX side
    """
    output_format = convert1_formats[format]
    host = list(hosts.values())[0]
    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_st20p_sessions(
        config=config,
        nic_port_list=host.vfs,
        test_mode="multicast",
        packing="GPM",
        width=1920,
        height=1080,
        fps="p30",
        input_format="YUV422RFC4175PG2BE10",
        transport_format="YUV_422_10bit",
        output_format=output_format,
        st20p_url=os.path.join(media, yuv_files_422rfc10["Penguin_1080p"]["filename"]),
    )

    rxtxapp.execute_test(config=config, build=build, test_time=test_time, host=host)


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


@pytest.mark.parametrize("format", convert2_formats.keys())
def tx_rx_conversion(
    hosts,
    build,
    media,
    nic_port_list,
    test_time,
    format,
):
    """
    Send random file in different pixel formats with supported two-way convertion on TX and RX
    """
    text_format, transport_format, _ = convert2_formats[format]
    host = list(hosts.values())[0]
    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_st20p_sessions(
        config=config,
        nic_port_list=host.vfs,
        test_mode="multicast",
        packing="GPM",
        width=1920,
        height=1080,
        fps="p30",
        input_format=format,
        transport_format=transport_format,
        output_format=format,
        st20p_url=os.path.join(media, "test_8k.yuv"),
    )

    rxtxapp.execute_test(config=config, build=build, test_time=test_time, host=host)


@pytest.mark.parametrize("format", pixel_formats.keys())
def test_formats(
    hosts,
    build,
    media,
    nic_port_list,
    test_time,
    format,
    test_config,
    prepare_ramdisk,
):
    """
    Send random file in different supported pixel formats without convertion during transport
    """
    text_format, file_format = pixel_formats[format]
    host = list(hosts.values())[0]

    # Get capture configuration from test_config.yaml
    # This controls whether tcpdump capture is enabled, where to store the pcap, etc.
    capture_cfg = dict(test_config.get("capture_cfg", {}))
    capture_cfg["test_name"] = (
        f"test_format_formats_{format}"  # Set a unique pcap file name
    )

    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_st20p_sessions(
        config=config,
        nic_port_list=host.vfs,
        test_mode="multicast",
        packing="GPM",
        width=1920,
        height=1080,
        fps="p30",
        input_format=file_format,
        transport_format=format,
        output_format=file_format,
        st20p_url=os.path.join(media, "test_8k.yuv"),
    )

    rxtxapp.execute_test(
        config=config,
        build=build,
        test_time=test_time,
        host=host,
        capture_cfg=capture_cfg,
    )
