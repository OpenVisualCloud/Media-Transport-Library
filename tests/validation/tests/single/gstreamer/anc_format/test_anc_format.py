# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

import os

import mtl_engine.media_creator as media_create
import pytest
from mtl_engine import GstreamerApp


@pytest.mark.parametrize("fps", [24, 25, 30, 50, 60, 100, 120])
@pytest.mark.parametrize("file_size_kb", [10, 100])
@pytest.mark.parametrize("framebuff", [3])
def test_st40p_fps_size(
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
    # Get the first host for remote execution
    host = list(hosts.values())[0]

    input_file_path = media_create.create_text_file(
        size_kb=file_size_kb,
        output_path=os.path.join(media, "test_anc.txt"),
        host=host,
    )

    tx_config = GstreamerApp.setup_gstreamer_st40p_tx_pipeline(
        build=build,
        nic_port_list=host.vfs[0],
        input_path=input_file_path,
        tx_payload_type=113,
        tx_queues=4,
        tx_framebuff_cnt=framebuff,
        tx_fps=fps,
        tx_did=67,
        tx_sdid=2,
    )

    rx_config = GstreamerApp.setup_gstreamer_st40p_rx_pipeline(
        build=build,
        nic_port_list=host.vfs[1],
        output_path=os.path.join(media, "output_anc.txt"),
        rx_payload_type=113,
        rx_queues=4,
        timeout=15,
    )

    capture_cfg = dict(test_config.get("capture_cfg", {}))
    capture_cfg["test_name"] = f"test_st40p_fps_size_{fps}_{file_size_kb}_{framebuff}"

    try:
        GstreamerApp.execute_test(
            build=build,
            tx_command=tx_config,
            rx_command=rx_config,
            input_file=input_file_path,
            output_file=os.path.join(media, "output_anc.txt"),
            test_time=test_time,
            host=host,
            tx_first=False,
            sleep_interval=5,
            capture_cfg=capture_cfg,
        )
    finally:
        # Remove the files after the test
        media_create.remove_file(input_file_path, host=host)
        media_create.remove_file(os.path.join(media, "output_anc.txt"), host=host)


@pytest.mark.parametrize("fps", [60])
@pytest.mark.parametrize("file_size_kb", [100])
@pytest.mark.parametrize("framebuff", [1, 3, 6, 12])
def test_st40p_framebuff(
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
    # Get the first host for remote execution
    host = list(hosts.values())[0]
    # Base the timeout on parameter to make sure the amount of time between RX and TX
    # is less than the timeout period
    timeout_period = 15

    input_file_path = media_create.create_text_file(
        size_kb=file_size_kb,
        output_path=os.path.join(media, "test_anc.txt"),
        host=host,
    )

    tx_config = GstreamerApp.setup_gstreamer_st40p_tx_pipeline(
        build=build,
        nic_port_list=host.vfs[0],
        input_path=input_file_path,
        tx_payload_type=113,
        tx_queues=4,
        tx_framebuff_cnt=framebuff,
        tx_fps=fps,
        tx_did=67,
        tx_sdid=2,
        tx_rfc8331=True,
    )

    rx_config = GstreamerApp.setup_gstreamer_st40p_rx_pipeline(
        build=build,
        nic_port_list=host.vfs[1],
        output_path=os.path.join(media, "output_anc.txt"),
        rx_payload_type=113,
        rx_queues=4,
        timeout=timeout_period + 10,
    )

    capture_cfg = dict(test_config.get("capture_cfg", {}))
    capture_cfg["test_name"] = f"test_st40p_framebuff_{fps}_{file_size_kb}_{framebuff}"

    try:
        GstreamerApp.execute_test(
            build=build,
            tx_command=tx_config,
            rx_command=rx_config,
            input_file=input_file_path,
            output_file=os.path.join(media, "output_anc.txt"),
            test_time=test_time,
            host=host,
            tx_first=False,
            sleep_interval=timeout_period,
            capture_cfg=capture_cfg,
        )
    finally:
        # Remove the files after the test
        media_create.remove_file(input_file_path, host=host)
        media_create.remove_file(os.path.join(media, "output_anc.txt"), host=host)


@pytest.mark.parametrize("fps", [24, 25, 30, 50, 60, 100, 120])
@pytest.mark.parametrize("framebuff", [1, 3, 6, 12])
def test_st40p_format_8331(
    hosts,
    build,
    media,
    nic_port_list,
    fps,
    framebuff,
    test_time,
    test_config,
    prepare_ramdisk,
):
    # Get the first host for remote execution
    host = list(hosts.values())[0]
    # Based on this parameters
    timeout_period = 15

    input_file_path = media_create.create_ancillary_rfc8331_pseudo_file(
        size_frames=fps * 10,
        output_path=os.path.join(media, "test_anc.txt"),
        host=host,
    )

    tx_config = GstreamerApp.setup_gstreamer_st40p_tx_pipeline(
        build=build,
        nic_port_list=host.vfs[0],
        input_path=input_file_path,
        tx_payload_type=113,
        tx_queues=4,
        tx_framebuff_cnt=framebuff,
        tx_fps=fps,
        tx_did=67,
        tx_sdid=2,
        tx_rfc8331=True,
    )

    rx_config = GstreamerApp.setup_gstreamer_st40p_rx_pipeline(
        build=build,
        nic_port_list=host.vfs[1],
        output_path=os.path.join(media, "output_anc.txt"),
        rx_payload_type=113,
        rx_queues=4,
        timeout=timeout_period + 10,
        capture_metadata=True,
    )

    capture_cfg = dict(test_config.get("capture_cfg", {}))
    capture_cfg["test_name"] = f"test_st40p_framebuff_{fps}_{framebuff}"

    try:
        GstreamerApp.execute_test(
            build=build,
            tx_command=tx_config,
            rx_command=rx_config,
            input_file=input_file_path,
            output_file=os.path.join(media, "output_anc.txt"),
            test_time=test_time,
            host=host,
            tx_first=False,
            sleep_interval=timeout_period,
            capture_cfg=capture_cfg,
        )
    finally:
        # Remove the files after the test
        media_create.remove_file(input_file_path, host=host)
        media_create.remove_file(os.path.join(media, "output_anc.txt"), host=host)
