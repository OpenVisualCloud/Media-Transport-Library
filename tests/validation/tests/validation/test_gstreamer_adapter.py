# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

"""Characterization tests for the GStreamer Application adapter.

No hardware or host is touched. Each case asserts the adapter emits exactly
the token lists produced by the procedural ``GstreamerApp`` builders (the
single source of truth), so the adapter is provably a thin wrapper.
"""

import pytest
from mtl_engine import GstreamerApp, ip_pools
from mtl_engine.gstreamer import GStreamer

BUILD = "/fake/build"


@pytest.fixture(autouse=True)
def _ip_pools():
    for pool in (
        ip_pools.rx,
        ip_pools.rx_multicast,
        ip_pools.tx,
        ip_pools.tx_r,
        ip_pools.rx_r,
    ):
        pool.clear()
    ip_pools.init(session_id=1)
    yield


def test_st20p_tokens_match_builders():
    nic = ["0000:01:00.0", "0000:01:00.1"]
    app = GStreamer(BUILD)
    app.create_command(
        build=BUILD,
        session_type="st20p",
        nic_port_list=nic,
        width=1920,
        height=1080,
        framerate="25",
        gst_format="I422_10LE",
        input_file="/in.yuv",
        output_file="/out.yuv",
        test_mode="multicast",
    )

    tx_golden = GstreamerApp.setup_gstreamer_st20p_tx_pipeline(
        build=BUILD,
        nic_port_list=nic[0],
        input_path="/in.yuv",
        width=1920,
        height=1080,
        framerate="25",
        format="I422_10LE",
        tx_payload_type=112,
        tx_queues=4,
    )
    rx_golden = GstreamerApp.setup_gstreamer_st20p_rx_pipeline(
        build=BUILD,
        nic_port_list=nic[1],
        output_path="/out.yuv",
        width=1920,
        height=1080,
        framerate="25",
        format="I422_10LE",
        rx_payload_type=112,
        rx_queues=4,
    )
    assert app._tx_command == tx_golden
    assert app._rx_command == rx_golden
    assert app.command == " ".join(rx_golden)


def test_st30_tokens_match_builders():
    nic = ["0000:01:00.0", "0000:01:00.1"]
    app = GStreamer(BUILD)
    app.create_command(
        build=BUILD,
        session_type="st30",
        nic_port_list=nic,
        input_file="/in.pcm",
        output_file="/out.pcm",
        audio_format="S24BE",
        audio_channels=2,
        audio_rate=48000,
    )

    tx_golden = GstreamerApp.setup_gstreamer_st30_tx_pipeline(
        build=BUILD,
        nic_port_list=nic[0],
        input_path="/in.pcm",
        tx_payload_type=111,
        tx_queues=4,
        audio_format="S24BE",
        channels=2,
        sampling=48000,
    )
    rx_golden = GstreamerApp.setup_gstreamer_st30_rx_pipeline(
        build=BUILD,
        nic_port_list=nic[1],
        output_path="/out.pcm",
        rx_payload_type=111,
        rx_queues=4,
        rx_audio_format=GstreamerApp.audio_format_change("S24BE", rx_side=True),
        rx_channels=2,
        rx_sampling=48000,
    )
    assert app._tx_command == tx_golden
    assert app._rx_command == rx_golden


def test_st40p_basic_tokens_match_builders():
    nic = ["0000:01:00.0", "0000:01:00.1"]
    app = GStreamer(BUILD)
    app.create_command(
        build=BUILD,
        session_type="st40p",
        nic_port_list=nic,
        input_file="/in.anc",
        output_file="/out.anc",
        tx_framebuff_cnt=3,
        tx_fps=25,
        tx_did=67,
        tx_sdid=2,
    )

    tx_golden = GstreamerApp.setup_gstreamer_st40p_tx_pipeline(
        build=BUILD,
        nic_port_list=nic[0],
        input_path="/in.anc",
        tx_payload_type=113,
        tx_queues=4,
        tx_framebuff_cnt=3,
        tx_fps=25,
        tx_did=67,
        tx_sdid=2,
    )
    rx_golden = GstreamerApp.setup_gstreamer_st40p_rx_pipeline(
        build=BUILD,
        nic_port_list=nic[1],
        output_path="/out.anc",
        rx_payload_type=113,
        rx_queues=4,
        timeout=15,
    )
    assert app._tx_command == tx_golden
    assert app._rx_command == rx_golden


def test_st40p_redundant_crossed_tokens():
    nic = ["0000:01:00.0", "0000:01:00.1", "0000:01:00.2", "0000:01:00.3"]
    app = GStreamer(BUILD)
    app.create_command(
        build=BUILD,
        session_type="st40p",
        nic_port_list=nic,
        input_file="/in.anc",
        output_file="/out.anc",
        tx_framebuff_cnt=3,
        rx_framebuff_cnt=3,
        tx_fps=25,
        tx_did=67,
        tx_sdid=2,
        redundant=True,
    )

    tx_golden = GstreamerApp.setup_gstreamer_st40p_tx_pipeline(
        build=BUILD,
        nic_port_list=nic[0],
        input_path="/in.anc",
        tx_payload_type=113,
        tx_queues=4,
        tx_framebuff_cnt=3,
        tx_fps=25,
        tx_did=67,
        tx_sdid=2,
        dev_port_red=nic[1],
        dev_ip_red=ip_pools.rx[1],
        ip_red=ip_pools.tx[1],
        udp_port_red=40001,
    )
    rx_golden = GstreamerApp.setup_gstreamer_st40p_rx_pipeline(
        build=BUILD,
        nic_port_list=nic[2],
        output_path="/out.anc",
        rx_payload_type=113,
        rx_queues=4,
        timeout=15,
        rx_framebuff_cnt=3,
        dev_port_red=nic[3],
        dev_ip_red=ip_pools.tx[1],
        ip_red=ip_pools.rx[1],
        udp_port_red=40001,
    )
    assert app._tx_command == tx_golden
    assert app._rx_command == rx_golden

    assert f"dev-port-red={nic[1]}" in app._tx_command
    assert f"dev-ip-red={ip_pools.rx[1]}" in app._tx_command
    assert f"ip-red={ip_pools.tx[1]}" in app._tx_command
    assert "udp-port-red=40001" in app._tx_command

    assert f"dev-port-red={nic[3]}" in app._rx_command
    assert f"dev-ip-red={ip_pools.tx[1]}" in app._rx_command
    assert f"ip-red={ip_pools.rx[1]}" in app._rx_command
    assert "udp-port-red=40001" in app._rx_command


def test_set_params_accepts_all_gstreamer_kwargs():
    app = GStreamer(BUILD)
    app.set_params(
        session_type="st40p",
        nic_port_list=["0000:01:00.0", "0000:01:00.1"],
        input_file="/in",
        output_file="/out",
        width=1920,
        height=1080,
        framerate="25",
        build=BUILD,
        gst_format="I422_10LE",
        tx_queues=4,
        rx_queues=4,
        tx_payload_type=113,
        rx_payload_type=113,
        audio_format="S24BE",
        audio_channels=2,
        audio_rate=48000,
        redundant=True,
        frame_info_path="/tmp/fi.log",
        capture_metadata=True,
        skip_file_compare=True,
        rx_timeout=15,
        tx_framebuff_cnt=3,
        rx_framebuff_cnt=3,
        tx_fps=25,
        tx_did=67,
        tx_sdid=2,
        tx_rfc8331=True,
        tx_user_pacing=True,
        tx_user_controlled_pacing=True,
        tx_user_controlled_pacing_offset=0,
        tx_interlaced=True,
        rx_interlaced=True,
        tx_split_anc_by_pkt=True,
        tx_test_mode="seq-gap",
        tx_test_pkt_count=200,
        tx_test_pacing_ns=0,
        rx_disable_auto_detect=True,
        rx_rtp_ring_size=1024,
    )


def test_st20p_universal_params_translate_to_builders():
    """UNIVERSAL_PARAMS vocabulary (transport_format + ``pNN`` framerate) maps
    onto the same tokens the GStreamer-only vocabulary produced."""
    nic = ["0000:01:00.0", "0000:01:00.1"]
    app = GStreamer(BUILD)
    app.create_command(
        session_type="st20p",
        nic_port_list=nic,
        width=1920,
        height=1080,
        framerate="p25",
        transport_format="YUV_422_10bit",
        pixel_format="YUV422PLANAR10LE",
        input_file="/in.yuv",
        output_file="/out.yuv",
        test_mode="multicast",
    )

    gst_format = GstreamerApp.video_format_change("YUV_422_10bit")
    tx_golden = GstreamerApp.setup_gstreamer_st20p_tx_pipeline(
        build=BUILD,
        nic_port_list=nic[0],
        input_path="/in.yuv",
        width=1920,
        height=1080,
        framerate="25",
        format=gst_format,
        tx_payload_type=112,
        tx_queues=4,
    )
    rx_golden = GstreamerApp.setup_gstreamer_st20p_rx_pipeline(
        build=BUILD,
        nic_port_list=nic[1],
        output_path="/out.yuv",
        width=1920,
        height=1080,
        framerate="25",
        format=gst_format,
        rx_payload_type=112,
        rx_queues=4,
    )
    assert app._tx_command == tx_golden
    assert app._rx_command == rx_golden


def test_st20p_enable_ptp_emits_token():
    """``enable_ptp=True`` appends ``enable-ptp=true`` to both TX and RX; the
    default keeps it absent."""
    nic = ["0000:01:00.0", "0000:01:00.1"]
    app = GStreamer(BUILD)
    app.create_command(
        session_type="st20p",
        nic_port_list=nic,
        width=1920,
        height=1080,
        framerate="p25",
        gst_format="I422_10LE",
        input_file="/in.yuv",
        output_file="/out.yuv",
        enable_ptp=True,
    )
    assert "enable-ptp=true" in app._tx_command
    assert "enable-ptp=true" in app._rx_command

    app_default = GStreamer(BUILD)
    app_default.create_command(
        session_type="st20p",
        nic_port_list=nic,
        width=1920,
        height=1080,
        framerate="p25",
        gst_format="I422_10LE",
        input_file="/in.yuv",
        output_file="/out.yuv",
    )
    assert "enable-ptp=true" not in app_default._tx_command
    assert "enable-ptp=true" not in app_default._rx_command


def test_st30p_universal_audio_params_translate_to_builders():
    """Universal audio vocabulary (``PCM24`` / ``["U02"]`` / ``48kHz`` and the
    ``st30p`` alias) maps onto the S24BE / 2ch / 48000 GStreamer builders."""
    nic = ["0000:01:00.0", "0000:01:00.1"]
    app = GStreamer(BUILD)
    app.create_command(
        session_type="st30p",
        nic_port_list=nic,
        input_file="/in.pcm",
        output_file="/out.pcm",
        audio_format="PCM24",
        audio_channels=["U02"],
        audio_sampling="48kHz",
    )

    tx_golden = GstreamerApp.setup_gstreamer_st30_tx_pipeline(
        build=BUILD,
        nic_port_list=nic[0],
        input_path="/in.pcm",
        tx_payload_type=111,
        tx_queues=4,
        audio_format="S24BE",
        channels=2,
        sampling=48000,
    )
    rx_golden = GstreamerApp.setup_gstreamer_st30_rx_pipeline(
        build=BUILD,
        nic_port_list=nic[1],
        output_path="/out.pcm",
        rx_payload_type=111,
        rx_queues=4,
        rx_audio_format=GstreamerApp.audio_format_change("S24BE", rx_side=True),
        rx_channels=2,
        rx_sampling=48000,
    )
    assert app._tx_command == tx_golden
    assert app._rx_command == rx_golden
