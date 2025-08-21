# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

import os

import mtl_engine.media_creator as media_create
import pytest
from mtl_engine import GstreamerApp


@pytest.mark.parametrize("fps", [24, 25, 30, 50, 60])
@pytest.mark.parametrize("file_size_kb", [10, 100])
@pytest.mark.parametrize("framebuff", [3])
def test_st40p_fps_size_dual(
    hosts,
    build,
    media,
    nic_port_list,
    file_size_kb,
    fps,
    framebuff,
    test_time,
    test_config,
    prepare_ramdisk,
):
    """Test GStreamer ST40P ANC format in dual host configuration."""
    # Get TX and RX hosts
    host_list = list(hosts.values())
    if len(host_list) < 2:
        pytest.skip("Dual tests require at least 2 hosts")

    tx_host = host_list[0]
    rx_host = host_list[1]

    # Create input file on TX host
    input_file_path = media_create.create_text_file(
        size_kb=file_size_kb,
        output_path=os.path.join(media, "test_anc.txt"),
        host=tx_host,
    )

    # Create output file path for RX host
    output_file_path = os.path.join(media, "output_anc_dual.txt")

    # Setup TX pipeline using existing function
    tx_config = GstreamerApp.setup_gstreamer_st40p_tx_pipeline(
        build=build,
        nic_port_list=tx_host.vfs[0],
        input_path=input_file_path,
        tx_payload_type=113,
        tx_queues=4,
        tx_framebuff_cnt=framebuff,
        tx_fps=fps,
        tx_did=67,
        tx_sdid=2,
    )

    # Setup RX pipeline using existing function
    rx_config = GstreamerApp.setup_gstreamer_st40p_rx_pipeline(
        build=build,
        nic_port_list=rx_host.vfs[0],
        output_path=output_file_path,
        rx_payload_type=113,
        rx_queues=4,
        timeout=15,
    )

    capture_cfg = dict(test_config.get("capture_cfg", {})) if test_config else {}
    capture_cfg["test_name"] = (
        f"test_st40p_fps_size_dual_{fps}_{file_size_kb}kb_{framebuff}"
    )

    try:
        result = GstreamerApp.execute_test(
            build=build,
            tx_command=tx_config,
            rx_command=rx_config,
            input_file=input_file_path,
            output_file=output_file_path,
            test_time=test_time,
            tx_host=tx_host,
            rx_host=rx_host,
            sleep_interval=3,
            tx_first=False,
            capture_cfg=capture_cfg,
        )

        assert result, f"GStreamer dual ST40P test failed for fps={fps}, size={file_size_kb}KB"

    finally:
        # Remove the input file on TX host and output file on RX host
        media_create.remove_file(input_file_path, host=tx_host)
        media_create.remove_file(output_file_path, host=rx_host)
