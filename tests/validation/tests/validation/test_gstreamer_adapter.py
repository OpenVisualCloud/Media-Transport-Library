# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

"""Characterization tests for the GStreamer adapter's parameter translation.

No hardware or host is touched. Each case asserts the UNIVERSAL_PARAMS
vocabulary (shared with RxTxApp/FFmpeg) and the ``enable_ptp`` flag translate
onto exactly the token lists produced by the procedural ``GstreamerApp``
builders (the single source of truth), so a common parametrized test can drive
GStreamer identically to the other frameworks.
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
