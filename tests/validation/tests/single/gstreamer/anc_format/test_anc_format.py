# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

import os

import pytest
import tests.Engine.GstreamerApp as gstreamerapp
import tests.Engine.media_creator as media_create


@pytest.mark.parametrize("fps", [24, 25, 30, 50, 60, 100, 120])
@pytest.mark.parametrize("file_size_kb", [10, 100])
@pytest.mark.parametrize("framebuff", [3])
def test_st40p_fps_size(
    build,
    media,
    nic_port_list,
    file_size_kb,
    fps,
    framebuff,
):
    input_file_path = media_create.create_text_file(
        size_kb=file_size_kb, output_path=os.path.join(media, "test_anc.txt")
    )

    tx_config = gstreamerapp.setup_gstreamer_st40p_tx_pipeline(
        build=build,
        nic_port_list=nic_port_list[0],
        input_path=input_file_path,
        tx_payload_type=113,
        tx_queues=4,
        tx_framebuff_cnt=framebuff,
        tx_fps=fps,
        tx_did=67,
        tx_sdid=2,
    )

    rx_config = gstreamerapp.setup_gstreamer_st40p_rx_pipeline(
        build=build,
        nic_port_list=nic_port_list[1],
        output_path=os.path.join(media, "output_anc.txt"),
        rx_payload_type=113,
        rx_queues=4,
        timeout=61,
    )

    try:
        gstreamerapp.execute_test(
            build=build,
            tx_command=tx_config,
            rx_command=rx_config,
            input_file=input_file_path,
            output_file=os.path.join(media, "output_anc.txt"),
            type="st40",
            tx_first=False,
            sleep_interval=0,
        )
    finally:
        # Remove the files after the test
        media_create.remove_file(input_file_path)
        media_create.remove_file(os.path.join(media, "output_anc.txt"))


@pytest.mark.parametrize("fps", [60])
@pytest.mark.parametrize("file_size_kb", [100])
@pytest.mark.parametrize("framebuff", [1, 3, 6, 12])
def test_st40p_framebuff(
    build,
    media,
    nic_port_list,
    file_size_kb,
    fps,
    framebuff,
):
    input_file_path = media_create.create_text_file(
        size_kb=file_size_kb, output_path=os.path.join(media, "test_anc.txt")
    )

    tx_config = gstreamerapp.setup_gstreamer_st40p_tx_pipeline(
        build=build,
        nic_port_list=nic_port_list[0],
        input_path=input_file_path,
        tx_payload_type=113,
        tx_queues=4,
        tx_framebuff_cnt=framebuff,
        tx_fps=fps,
        tx_did=67,
        tx_sdid=2,
    )

    rx_config = gstreamerapp.setup_gstreamer_st40p_rx_pipeline(
        build=build,
        nic_port_list=nic_port_list[1],
        output_path=os.path.join(media, "output_anc.txt"),
        rx_payload_type=113,
        rx_queues=4,
        timeout=61,
    )

    try:
        gstreamerapp.execute_test(
            build=build,
            tx_command=tx_config,
            rx_command=rx_config,
            input_file=input_file_path,
            output_file=os.path.join(media, "output_anc.txt"),
            type="st40",
            tx_first=False,
            sleep_interval=0,
        )
    finally:
        # Remove the files after the test
        media_create.remove_file(input_file_path)
        media_create.remove_file(os.path.join(media, "output_anc.txt"))
