# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

import pytest
import logging
from mtl_engine.app_refactored import Application
from mtl_engine.media_files import yuv_files_422p10le, yuv_files_422rfc10

logger = logging.getLogger(__name__)


@pytest.mark.nightly
@pytest.mark.parametrize(
    "media_file",
    list(yuv_files_422p10le.values()),
    indirect=["media_file"],
    ids=list(yuv_files_422p10le.keys()),
)
def test_422p10le_refactored(
    hosts,
    build,
    media,
    nic_port_list,
    test_time,
    test_config,
    prepare_ramdisk,
    media_file,
):
    """
    Send files in YUV422PLANAR10LE format converting to transport format YUV_422_10bit
    Using the new refactored Application class - matches the working test_422p10le
    """
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]
    build = '/root/awilczyn/Media-Transport-Library/tests/tools/RxTxApp/build'
    # Get capture configuration from test_config.yaml - matches working test
    capture_cfg = dict(test_config.get("capture_cfg", {}))
    capture_cfg["test_name"] = (
        f"test_format_{media_file_info['filename']}"  # Match working test pattern
    )

    # Create application instance - use the build path to find RxTxApp
    # build fixture provides the MTL build path, we need the RxTxApp within it
    app = Application("RxTxApp", build)
    
    # Configure test parameters to exactly match working test behavior
    app.create_command(
        session_type="st20p",
        nic_port_list=host.vfs,  # Use VF list like working test
        test_mode="multicast",
        width=media_file_info["width"],
        height=media_file_info["height"],
        framerate=f"p{media_file_info['fps']}",
        pixel_format=media_file_info["file_format"],     # Input/output format
        transport_format=media_file_info["format"],      # Transport format
        input_file=media_file_path,  # Input file for TX session
    )

    # Execute test using the Application's execute_test method
    app.execute_test(
        build=build,  # Use the build fixture directly
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


@pytest.mark.parametrize(
    "media_file",
    [yuv_files_422rfc10["Penguin_1080p"]],
    indirect=["media_file"],
    ids=["Penguin_1080p"],
)
@pytest.mark.parametrize("format", convert1_formats.keys())
def test_convert_on_rx_refactored(
    hosts, build, media, nic_port_list, test_time, format, media_file
):
    """
    Send file in YUV_422_10bit pixel formats with supported convertion on RX side
    Using the new refactored Application class - matches the working test_convert_on_rx
    """
    media_file_info, media_file_path = media_file
    output_format = convert1_formats[format]
    host = list(hosts.values())[0]
    
    # Create application instance - use the build path to find RxTxApp
    app = Application("RxTxApp", build)
    
    # Configure test parameters to exactly match working test
    app.create_command(
        session_type="st20p",
        nic_port_list=host.vfs,
        test_mode="multicast",
        packing="GPM",  # Match working test
        width=media_file_info["width"],
        height=media_file_info["height"],
        framerate="p30",  # Hardcoded like working test
        pixel_format="YUV422RFC4175PG2BE10",     # Input format for TX
        transport_format="YUV_422_10bit",        # Transport format
        pixel_format_rx=output_format,           # Output format for RX conversion
        input_file=media_file_path,              # Input file for TX session
    )

    # Execute test using the Application's execute_test method
    app.execute_test(
        build=build,  # Use the build fixture directly
        test_time=test_time,
        host=host
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
    [yuv_files_422rfc10["test_8K"]],  # Use test_8K like working test
    indirect=["media_file"],
    ids=["test_8K"],
)
@pytest.mark.parametrize("format", convert2_formats.keys())
def test_tx_rx_conversion_refactored(
    hosts,
    build,
    media,
    nic_port_list,
    test_time,
    format,
    media_file,
):
    """
    Send random file in different pixel formats with supported two-way convertion on TX and RX
    Using the new refactored Application class - matches the working test_tx_rx_conversion
    """
    media_file_info, media_file_path = media_file
    text_format, transport_format, _ = convert2_formats[format]
    host = list(hosts.values())[0]
    
    # Create application instance - use the build path to find RxTxApp
    app = Application("RxTxApp", build)
    
    # Configure test parameters to exactly match working test
    app.create_command(
        session_type="st20p",
        nic_port_list=host.vfs,
        test_mode="multicast",
        packing="GPM",  # Match working test
        width=media_file_info["width"],
        height=media_file_info["height"],
        framerate="p30",  # Hardcoded like working test
        pixel_format=format,                 # Input/output format (two-way conversion)
        transport_format=transport_format,   # Transport format
        input_file=media_file_path,          # Input file for TX session
    )

    # Execute test using the Application's execute_test method
    app.execute_test(
        build=build,  # Use the build fixture directly
        test_time=test_time,
        host=host
    )


@pytest.mark.parametrize(
    "media_file",
    [yuv_files_422rfc10["test_8K"]],  # Use test_8K like working test
    indirect=["media_file"],
    ids=["test_8K"],
)
@pytest.mark.parametrize("format", pixel_formats.keys())
def test_formats_refactored(
    hosts,
    build,
    media,
    nic_port_list,
    test_time,
    format,
    test_config,
    prepare_ramdisk,
    media_file,
):
    """
    Send random file in different supported pixel formats without convertion during transport
    Using the new refactored Application class - matches the working test_formats
    """
    media_file_info, media_file_path = media_file
    text_format, file_format = pixel_formats[format]
    host = list(hosts.values())[0]

    # Get capture configuration from test_config.yaml - matches working test
    capture_cfg = dict(test_config.get("capture_cfg", {}))
    capture_cfg["test_name"] = (
        f"test_format_formats_{format}"  # Match working test pattern
    )

    # Create application instance - use the build path to find RxTxApp
    app = Application("RxTxApp", build)
    
    # Configure test parameters to exactly match working test
    app.create_command(
        session_type="st20p",
        nic_port_list=host.vfs,
        test_mode="multicast",
        packing="GPM",  # Match working test
        width=media_file_info["width"],
        height=media_file_info["height"],
        framerate="p30",  # Hardcoded like working test
        pixel_format=file_format,       # Input/output format (pixel format specific)
        transport_format=format,        # Transport format
        input_file=media_file_path,     # Input file for TX session
    )

    # Execute test using the Application's execute_test method
    app.execute_test(
        build=build,  # Use the build fixture directly
        test_time=test_time,
        host=host,
        capture_cfg=capture_cfg,
    )


# Additional test demonstrating dual-host testing with refactored Application
@pytest.mark.dual_host
@pytest.mark.parametrize(
    "media_file",
    [yuv_files_422p10le["Penguin_1080p"]],
    indirect=["media_file"],
    ids=["Penguin_1080p"],
)
def test_dual_host_refactored(
    hosts,
    build,
    media,
    nic_port_list,
    test_time,
    test_config,
    prepare_ramdisk,
    media_file,
):
    """
    Dual host test example using the new refactored Application class
    """
    media_file_info, media_file_path = media_file
    host_list = list(hosts.values())
    
    if len(host_list) < 2:
        pytest.skip("Dual host test requires at least 2 hosts")
    
    tx_host = host_list[0]
    rx_host = host_list[1]

    # Get capture configuration
    capture_cfg = dict(test_config.get("capture_cfg", {}))
    capture_cfg["test_name"] = "test_format_refactored_dual_host"

    # Create application instance
    # Note: build fixture points to mtl_path, but we need the RxTxApp build directory
    rxtxapp_build_path = "/root/awilczyn/Media-Transport-Library/tests/tools/RxTxApp/build"
    app = Application("RxTxApp", rxtxapp_build_path)
    
    # Configure test parameters
    app.create_command(
        session_type="st20p",
        # Don't specify direction - let it create both TX and RX sessions like original working test
        nic_port=tx_host.vfs[0] if tx_host.vfs else "0000:31:01.0",
        source_ip="192.168.1.10",
        destination_ip="239.1.1.1",
        width=media_file_info["width"],
        height=media_file_info["height"],
        framerate=f"p{media_file_info['fps']}",
        pixel_format=media_file_info["file_format"],
        transport_format=media_file_info["format"],
        input_file=media_file_path,
        output_file="/tmp/received_output.yuv",
        packing="BPM",
        test_mode="multicast"
    )

    # Execute dual host test
    app.execute_test(
        build=rxtxapp_build_path,  # Use the RxTxApp build path
        test_time=test_time,
        tx_host=tx_host,
        rx_host=rx_host,
        input_file=media_file_path,
        output_file="/tmp/received_output.yuv",
        capture_cfg=capture_cfg,
    )


# Test demonstrating FFmpeg integration with refactored Application
@pytest.mark.ffmpeg
@pytest.mark.parametrize(
    "media_file",
    [yuv_files_422p10le["Penguin_1080p"]],
    indirect=["media_file"],
    ids=["Penguin_1080p"],
)
def test_ffmpeg_format_refactored(
    hosts,
    build,
    media,
    nic_port_list,
    test_time,
    media_file,
):
    """
    FFmpeg test example using the new refactored Application class
    """
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]

    # Create FFmpeg application instance
    app = Application("FFmpeg", "/usr/bin")
    
    # Configure test parameters for TX
    app.create_command(
        session_type="st20p",
        # Don't specify direction - let it create both TX and RX sessions like original working test
        nic_port_list=host.vfs,  # Use full VF list like working test
        source_ip="192.168.1.10",
        destination_ip="239.1.1.1",
        width=media_file_info["width"],
        height=media_file_info["height"],
        pixel_format=media_file_info["file_format"],
        input_file=media_file_path,
        port=20000,
        payload_type=112
    )

    # Execute test - FFmpeg doesn't need RxTxApp build path
    app.execute_test(
        build="/usr/bin",  # Use the FFmpeg bin path
        test_time=test_time,
        host=host,
        input_file=media_file_path,
        output_file="/tmp/ffmpeg_output.yuv"
    )


# Test demonstrating GStreamer integration with refactored Application
@pytest.mark.gstreamer
@pytest.mark.parametrize(
    "media_file",
    [yuv_files_422p10le["Penguin_1080p"]],
    indirect=["media_file"],
    ids=["Penguin_1080p"],
)
def test_gstreamer_format_refactored(
    hosts,
    build,
    media,
    nic_port_list,
    test_time,
    media_file,
):
    """
    GStreamer test example using the new refactored Application class
    """
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]

    # Create a proper output directory in /tmp with write permissions
    import tempfile
    import os
    output_dir = tempfile.mkdtemp()
    output_file = os.path.join(output_dir, "gstreamer_output.yuv")

    # Create GStreamer application instance
    app = Application("GStreamer", "/usr/bin")
    
    # Configure test parameters for TX
    app.create_command(
        session_type="st20p",
        nic_port_list=host.vfs,  # Use VF list like other tests
        source_ip="192.168.1.10",
        destination_ip="239.1.1.1",
        width=media_file_info["width"],
        height=media_file_info["height"],
        framerate=f"{media_file_info['fps']}/1",
        input_file=media_file_path
    )

    # Execute test with proper output file handling
    try:
        app.execute_test(
            build="/usr/bin",  # Use the GStreamer bin path
            test_time=test_time,
            host=host,
            input_file=media_file_path,
            output_file=output_file
        )
    finally:
        # Cleanup: Remove temporary output file and directory
        if os.path.exists(output_file):
            os.remove(output_file)
        if os.path.exists(output_dir):
            os.rmdir(output_dir)
