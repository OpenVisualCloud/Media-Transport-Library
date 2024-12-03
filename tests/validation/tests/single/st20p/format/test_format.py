# INTEL CONFIDENTIAL
# Copyright 2024-2024 Intel Corporation.
#
# This software and the related documents are Intel copyrighted materials, and your use of them is governed
# by the express license under which they were provided to you ("License"). Unless the License provides otherwise,
# you may not use, modify, copy, publish, distribute, disclose or transmit this software or the related documents
# without Intel's prior written permission.
#
# This software and the related documents are provided as is, with no express or implied warranties,
# other than those that are expressly stated in the License.
import os
import re

import pytest
import tests.Engine.RxTxApp as rxtxapp
from tests.Engine.execute import log_fail
from tests.Engine.media_files import yuv_files_422p10le, yuv_files_422rfc10


@pytest.mark.parametrize("file", yuv_files_422p10le.keys())
def test_422p10le(
    build,
    media,
    nic_port_list,
    test_time,
    file,
):
    """
    Send files in YUV422PLANAR10LE format converting to transport format YUV_422_10bit
    """
    st20p_file = yuv_files_422p10le[file]

    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_st20p_sessions(
        config=config,
        nic_port_list=nic_port_list,
        test_mode="multicast",
        width=st20p_file["width"],
        height=st20p_file["height"],
        fps="p30",
        input_format=st20p_file["file_format"],
        transport_format=st20p_file["format"],
        output_format=st20p_file["file_format"],
        st20p_url=os.path.join(media, st20p_file["filename"]),
    )

    stdout = rxtxapp.execute_test(config=config, build=build, test_time=test_time)
    if not re.search("st20p_tx_create.+transport fmt ST20_FMT_YUV_422_10BIT.+YUV422PLANAR10LE", stdout):
        log_fail("Could not find expected TX pixel formats")
    if not re.search("st20p_rx_create.+transport fmt ST20_FMT_YUV_422_10BIT.+YUV422PLANAR10LE", stdout):
        log_fail("Could not find expected RX pixel formats")


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
    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_st20p_sessions(
        config=config,
        nic_port_list=nic_port_list,
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

    stdout = rxtxapp.execute_test(config=config, build=build, test_time=test_time)
    if not re.search("st20p_tx_create.+transport fmt ST20_FMT_YUV_422_10BIT.+YUV422RFC4175PG2BE10", stdout):
        log_fail("Could not find expected TX pixel formats")
    if not re.search(f"st20p_rx_create.+transport fmt ST20_FMT_YUV_422_10BIT.+{format}", stdout):
        log_fail("Could not find expected RX pixel formats")


# List of supported two-way convertions based on st_frame_get_converter()
convert2_formats = dict(
    V210=("ST20_FMT_YUV_422_10BIT", "YUV_422_10bit", "YUV422RFC4175PG2BE10"),
    Y210=("ST20_FMT_YUV_422_10BIT", "YUV_422_10bit", "YUV422RFC4175PG2BE10"),
    YUV422PLANAR12LE=("ST20_FMT_YUV_422_12BIT", "YUV_422_12bit", "YUV422RFC4175PG2BE12"),
    YUV444PLANAR10LE=("ST20_FMT_YUV_444_10BIT", "YUV_444_10bit", "YUV444RFC4175PG4BE10"),
    YUV444PLANAR12LE=("ST20_FMT_YUV_444_12BIT", "YUV_444_12bit", "YUV444RFC4175PG2BE12"),
    GBRPLANAR10LE=("ST20_FMT_RGB_10BIT", "RGB_10bit", "RGBRFC4175PG4BE10"),
    GBRPLANAR12LE=("ST20_FMT_RGB_12BIT", "RGB_12bit", "RGBRFC4175PG2BE12"),
)


@pytest.mark.parametrize("format", convert2_formats.keys())
def tx_rx_conversion(
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
    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_st20p_sessions(
        config=config,
        nic_port_list=nic_port_list,
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

    stdout = rxtxapp.execute_test(config=config, build=build, test_time=test_time)
    if not re.search(f"st20p_tx_create.+transport fmt {text_format}.+{format}", stdout):
        log_fail("Could not find expected TX pixel formats")
    if not re.search(f"st20p_rx_create.+transport fmt {text_format}.+{format}", stdout):
        log_fail("Could not find expected RX pixel formats")


@pytest.mark.parametrize("format", pixel_formats.keys())
def test_formats(
    build,
    media,
    nic_port_list,
    test_time,
    format,
):
    """
    Send random file in different supported pixel formats without convertion during transport
    """
    text_format, file_format = pixel_formats[format]
    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_st20p_sessions(
        config=config,
        nic_port_list=nic_port_list,
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

    stdout = rxtxapp.execute_test(config=config, build=build, test_time=test_time)
    if not re.search(f"st20p_tx_create.+transport fmt {text_format}.+{file_format}", stdout):
        log_fail("Could not find expected TX pixel formats")
    if not re.search(f"st20p_rx_create.+transport fmt {text_format}.+{file_format}", stdout):
        log_fail("Could not find expected RX pixel formats")
