# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

"""FFmpeg MTL Plugin Tests

**KNOWN ISSUE**: This test is currently disabled due to DPDK initialization failures
in the FFmpeg MTL plugin when using DPDK shared libraries.

Root Cause:
-----------
The FFmpeg MTL plugin attempts to initialize DPDK EAL, but fails with
"EAL: Cannot allocate memzone list" when:
1. Using DPDK 25.x shared libraries (librte_eal.so.25)
2. MTL uses --in-memory flag during EAL initialization
3. Multiple FFmpeg processes attempt parallel MTL initialization

This appears to be a regression in DPDK 25.x shared library builds when
EAL is initialized with --in-memory multiple times, even across separate
processes using the same file prefix.

Potential Solutions:
-------------------
1. Fix FFmpeg MTL plugin to properly handle DPDK initialization
2. Use static DPDK linking instead of shared libraries
3. Run RX/TX sequentially instead of in parallel
4. Add unique file-prefix support to FFmpeg MTL plugin options
5. Investigate DPDK 25.x memzone allocation regression

For now, these tests are skipped pending resolution.
"""

import pytest
from common.nicctl import InterfaceSetup
from mtl_engine import ffmpeg_app
from mtl_engine.media_files import yuv_files


@pytest.mark.nightly
@pytest.mark.parametrize(
    "video_format, test_time_multipler, media_file",
    [
        ("i1080p25", 2, yuv_files["i1080p25"]),
        ("i1080p50", 2, yuv_files["i1080p50"]),
        pytest.param("i1080p60", 4, yuv_files["i1080p60"], marks=pytest.mark.smoke),
        ("i2160p60", 6, yuv_files["i2160p60"]),
    ],
    indirect=["media_file"],
    ids=["i1080p25", "i1080p50", "i1080p60", "i2160p60"],
)
@pytest.mark.parametrize("output_format", ["yuv", "h264"])
def test_rx_ffmpeg_tx_ffmpeg(
    hosts,
    test_time,
    build,
    setup_interfaces: InterfaceSetup,
    video_format,
    test_time_multipler,
    output_format,
    test_config,
    media_file,
):
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )

    ffmpeg_app.execute_test(
        test_time=test_time * test_time_multipler,
        build=build,
        host=host,
        nic_port_list=interfaces_list,
        type_="frame",
        video_format=video_format,
        pg_format=media_file_info["format"],
        video_url=media_file_path,
        output_format=output_format,
    )
