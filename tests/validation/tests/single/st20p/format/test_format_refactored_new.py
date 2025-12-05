# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

import pytest
from mtl_engine.app_refactored import Application
from mtl_engine.media_files import yuv_files_422p10le, yuv_files_422rfc10


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
    Using Application class refactored interface
    """
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]

    # Get capture configuration from test_config.yaml
    # This controls whether tcpdump capture is enabled, where to store the pcap, etc.
    capture_cfg = dict(test_config.get("capture_cfg", {}))
    capture_cfg["test_name"] = (
        f"test_format_refactored_{media_file_info['filename']}"  # Set a unique pcap file name
    )

    # Create Application instance for RxTxApp
    app = Application("RxTxApp", f"{build}/tests/tools/RxTxApp/build")
    
    # Configure application using universal parameters
    # For large files (1080p and above), add performance optimizations
    config_params = {
        "session_type": "st20p",
        "nic_port": host.vfs[0] if host.vfs else "0000:31:01.0",
        "nic_port_list": host.vfs,
        "destination_ip": "239.168.48.9",
        "port": 20000,
        "width": media_file_info["width"],
        "height": media_file_info["height"],
        "framerate": f"p{media_file_info['fps']}",
        "pixel_format": media_file_info["file_format"],
        "transport_format": media_file_info["format"],
        "pixel_format_rx": media_file_info["file_format"],
        "input_file": media_file_path,
        "test_mode": "multicast",
    }
    
    # Add optimizations for large files (1080p and above)
    if media_file_info.get("height", 0) >= 1080:
        # Calculate file size to determine if it's a large file requiring special handling
        import os
        try:
            file_size_mb = os.path.getsize(media_file_path) / (1024 * 1024)
        except:
            file_size_mb = 0
            
            # For very large files (>500MB), implement ultra-aggressive optimizations
            if file_size_mb > 500:
                config_params.update({
                    "framebuffer_count": 16,       # Maximum frame buffers for very large content
                    "rx_video_fb_cnt": 8,          # Maximum valid RX frame buffer count (range [2:8])
                    "pacing": "gap",               # Use gap pacing like working test
                    "rx_separate_lcore": True,     # Dedicate RX cores for performance
                    "allow_across_numa_core": True, # Allow NUMA optimization
                    "sch_session_quota": 32,       # Maximum session quota per core for large files
                    "nb_tx_desc": 4096,           # Maximum TX descriptors for very large files
                    "nb_rx_desc": 4096,           # Maximum RX descriptors for very large files
                    "mono_pool": True,            # Use mono pool for better memory management
                    "tasklet_sleep": True,        # Enable tasklet sleep for better resource management
                    "rxtx_simd_512": True,        # Enable SIMD 512 for better performance
                })
            else:
                # Standard optimizations for regular 1080p files
                config_params.update({
                    "framebuffer_count": 4,        # More frame buffers for large content
                    "rx_video_fb_cnt": 4,          # Increase RX frame buffer count
                    "pacing": "gap",               # Use standard gap pacing for reliability
                    "rx_separate_lcore": True,     # Dedicate RX cores for performance
                    "allow_across_numa_core": True, # Allow NUMA optimization
                    "sch_session_quota": 8,        # Higher session quota per core
                    "nb_tx_desc": 1024,           # Increase TX descriptors for large files
                    "nb_rx_desc": 2048,           # Increase RX descriptors for large files
                })

    app.create_command(**config_params)

    # Execute test using Application class
    # Use optimized test time for large files to ensure accurate FPS measurement
    if media_file_info.get("height", 0) >= 1080:
        try:
            file_size_mb = os.path.getsize(media_file_path) / (1024 * 1024)
            # Very large files get longer test time to ensure accurate FPS measurement
            actual_test_time = 15 if file_size_mb > 500 else 10
        except:
            actual_test_time = 10
    else:
        actual_test_time = test_time

    app.execute_test(
        build=build,
        test_time=actual_test_time,
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
    Using Application class refactored interface
    """
    media_file_info, media_file_path = media_file
    output_format = convert1_formats[format]
    host = list(hosts.values())[0]
    
    # Create Application instance for RxTxApp
    app = Application("RxTxApp", f"{build}/tests/tools/RxTxApp/build")
    
    # Configure application using universal parameters
    app.create_command(
        session_type="st20p",
        nic_port=host.vfs[0] if host.vfs else "0000:31:01.0",
        nic_port_list=host.vfs,
        destination_ip="239.168.48.9",
        port=20000,
        width=media_file_info["width"],
        height=media_file_info["height"],
        framerate="p30",  # TODO: Hardcoded
        pixel_format="YUV422RFC4175PG2BE10",
        transport_format="YUV_422_10bit",
        pixel_format_rx=output_format,
        input_file=media_file_path,
        test_mode="multicast",
        packing="GPM",
    )

    # Execute test using Application class
    app.execute_test(
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
    Using Application class refactored interface
    """
    media_file_info, media_file_path = media_file
    text_format, transport_format, _ = convert2_formats[format]
    host = list(hosts.values())[0]
    
    # Create Application instance for RxTxApp
    app = Application("RxTxApp", f"{build}/tests/tools/RxTxApp/build")
    
    # Configure application using universal parameters
    app.create_command(
        session_type="st20p",
        nic_port=host.vfs[0] if host.vfs else "0000:31:01.0",
        nic_port_list=host.vfs,
        destination_ip="239.168.48.9",
        port=20000,
        width=media_file_info["width"],
        height=media_file_info["height"],
        framerate="p30",  # TODO: Hardcoded
        pixel_format=format,
        transport_format=transport_format,
        pixel_format_rx=format,
        input_file=media_file_path,
        test_mode="multicast",
        packing="GPM",
    )

    # Execute test using Application class
    app.execute_test(
        build=build,
        test_time=test_time,
        host=host,
    )


@pytest.mark.parametrize(
    "media_file",
    [yuv_files_422rfc10["test_8K"]],
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
    Using Application class refactored interface
    """
    media_file_info, media_file_path = media_file
    text_format, file_format = pixel_formats[format]
    host = list(hosts.values())[0]

    # Get capture configuration from test_config.yaml
    # This controls whether tcpdump capture is enabled, where to store the pcap, etc.
    capture_cfg = dict(test_config.get("capture_cfg", {}))
    capture_cfg["test_name"] = (
        f"test_format_refactored_formats_{format}"  # Set a unique pcap file name
    )

    # Create Application instance for RxTxApp
    app = Application("RxTxApp", f"{build}/tests/tools/RxTxApp/build")
    
    # Configure application using universal parameters
    app.create_command(
        session_type="st20p",
        nic_port=host.vfs[0] if host.vfs else "0000:31:01.0",
        nic_port_list=host.vfs,
        destination_ip="239.168.48.9",
        port=20000,
        width=media_file_info["width"],
        height=media_file_info["height"],
        framerate="p30",  # TODO: Hardcoded
        pixel_format=file_format,
        transport_format=format,
        pixel_format_rx=file_format,
        input_file=media_file_path,
        test_mode="multicast",
        packing="GPM",
    )

    # Execute test using Application class
    app.execute_test(
        build=build,
        test_time=test_time,
        host=host,
        capture_cfg=capture_cfg,
    )


@pytest.mark.parametrize(
    "media_file",
    [yuv_files_422p10le["Penguin_720p"]],
    indirect=["media_file"],
    ids=["Penguin_720p"],
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
    Test dual host configuration using Application class
    TX on one host, RX on another host
    """
    media_file_info, media_file_path = media_file
    
    # For dual host testing, we need at least 2 hosts
    if len(hosts) < 2:
        pytest.skip("Dual host test requires at least 2 hosts")
    
    host_list = list(hosts.values())
    tx_host = host_list[0]
    rx_host = host_list[1]

    # Get capture configuration from test_config.yaml
    capture_cfg = dict(test_config.get("capture_cfg", {}))
    capture_cfg["test_name"] = (
        f"test_format_refactored_dual_host_{media_file_info['filename']}"
    )

    # Create Application instance for RxTxApp
    app = Application("RxTxApp", f"{build}/tests/tools/RxTxApp/build")
    
    # Configure application using universal parameters
    app.create_command(
        session_type="st20p",
        nic_port=tx_host.vfs[0] if tx_host.vfs else "0000:31:01.0",
        nic_port_list=tx_host.vfs,
        destination_ip="239.168.48.9",
        port=20000,
        width=media_file_info["width"],
        height=media_file_info["height"],
        framerate=f"p{media_file_info['fps']}",
        pixel_format=media_file_info["file_format"],
        transport_format=media_file_info["format"],
        pixel_format_rx=media_file_info["file_format"],
        input_file=media_file_path,
        test_mode="multicast",
    )

    # Execute dual host test using Application class
    app.execute_test(
        build=build,
        test_time=test_time,
        tx_host=tx_host,
        rx_host=rx_host,
        capture_cfg=capture_cfg,
    )


@pytest.mark.parametrize(
    "media_file",
    [yuv_files_422p10le["Penguin_720p"]],
    indirect=["media_file"],
    ids=["Penguin_720p"],
)
def test_ffmpeg_format_refactored(
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
    Test FFmpeg integration using Application class
    """
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]

    # Get capture configuration from test_config.yaml
    capture_cfg = dict(test_config.get("capture_cfg", {}))
    capture_cfg["test_name"] = (
        f"test_format_refactored_ffmpeg_{media_file_info['filename']}"
    )

    # Create Application instance for FFmpeg
    app = Application("FFmpeg", "/usr/bin")
    
    # Configure application using universal parameters
    app.create_command(
        session_type="st20p",
        nic_port=host.vfs[0] if host.vfs else "0000:31:01.0",
        destination_ip="239.168.48.9",
        port=20000,
        width=media_file_info["width"],
        height=media_file_info["height"],
        framerate=f"p{media_file_info['fps']}",
        pixel_format=media_file_info["file_format"],
        transport_format=media_file_info["format"],
        input_file=media_file_path,
        output_file="/tmp/ffmpeg_output.yuv",
    )

    # Execute test using Application class
    app.execute_test(
        build=build,
        test_time=test_time,
        host=host,
        input_file=media_file_path,
        output_file="/tmp/ffmpeg_output.yuv",
        capture_cfg=capture_cfg,
    )


@pytest.mark.parametrize(
    "media_file",
    [yuv_files_422p10le["Penguin_720p"]],
    indirect=["media_file"],
    ids=["Penguin_720p"],
)
def test_gstreamer_format_refactored(
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
    Test GStreamer integration using Application class
    """
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]

    # Get capture configuration from test_config.yaml
    capture_cfg = dict(test_config.get("capture_cfg", {}))
    capture_cfg["test_name"] = (
        f"test_format_refactored_gstreamer_{media_file_info['filename']}"
    )

    # Create Application instance for GStreamer
    app = Application("GStreamer", "/usr/bin")
    
    # Configure application using universal parameters
    app.create_command(
        session_type="st20p",
        nic_port=host.vfs[0] if host.vfs else "0000:31:01.0",
        destination_ip="239.168.48.9",
        port=20000,
        width=media_file_info["width"],
        height=media_file_info["height"],
        framerate=f"p{media_file_info['fps']}",
        pixel_format=media_file_info["file_format"],
        transport_format=media_file_info["format"],
        input_file=media_file_path,
        output_file="/tmp/gstreamer_output.yuv",
    )

    # Execute test using Application class
    app.execute_test(
        build=build,
        test_time=test_time,
        host=host,
        input_file=media_file_path,
        output_file="/tmp/gstreamer_output.yuv",
        capture_cfg=capture_cfg,
    )
