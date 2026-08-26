# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

from pathlib import Path

import pytest
from common.nicctl import InterfaceSetup
from mtl_engine.const import LOG_FOLDER
from mtl_engine.media_files import audio_files

_AUDIO_FORMATS = ["PCM8", "PCM16", "PCM24"]
_AUDIO_CHANNELS = ["M", "DM", "ST", "LtRt", "51", "71", "222", "SGRP"]
_SMOKE_CASE = ("PCM16", "M")


@pytest.mark.nightly
@pytest.mark.parametrize(
    "application",
    [
        "rxtxapp",
        pytest.param(
            "ffmpeg",
            marks=pytest.mark.skip(
                reason="FFmpeg does not support st30p audio pipeline"
            ),
        ),
        "gstreamer",
    ],
)
@pytest.mark.parametrize(
    "media_file",
    [
        audio_files["PCM8"],
        audio_files["PCM24"],
    ],
    indirect=["media_file"],
    ids=["PCM8", "PCM24"],
)
@pytest.mark.tx_and_rx
def test_st30p_integrity(
    application,
    app_factory,
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_time,
    test_config,
    media_file,
    pcap_capture,
    output_files,
    media_integrity,
):
    """Test st30p audio integrity (bit-exact comparison)."""
    media_file_info, media_file_path = media_file
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )
    log_dir = Path.cwd() / LOG_FOLDER / "latest"
    log_dir.mkdir(parents=True, exist_ok=True)
    out_file_url = output_files.register(str(log_dir / "out.wav"))
    host = list(hosts.values())[0]

    app = app_factory(
        application, session_type="st30p", audio_format=media_file_info["format"]
    )
    app.create_command(
        session_type="st30p",
        nic_port_list=interfaces_list,
        test_mode="multicast",
        audio_format=media_file_info["format"],
        audio_channels=["U02"],
        audio_sampling="48kHz",
        audio_ptime="1",
        input_file=media_file_path,
        output_file=out_file_url,
        test_time=test_time,
    )

    app.execute_test(
        build=mtl_path,
        test_time=test_time,
        host=host,
        compliance=pcap_capture,
        integrity=media_integrity,
    )


@pytest.mark.nightly
@pytest.mark.parametrize(
    "application",
    [
        "rxtxapp",
        "ffmpeg",
        "gstreamer",
    ],
)
@pytest.mark.parametrize(
    ("media_file", "audio_channel"),
    [
        pytest.param(
            audio_files[fmt],
            ch,
            marks=[pytest.mark.smoke] if (fmt, ch) == _SMOKE_CASE else [],
            id=f"{fmt}-{ch}",
        )
        for fmt in _AUDIO_FORMATS
        for ch in _AUDIO_CHANNELS
    ],
    indirect=["media_file"],
)
@pytest.mark.tx_and_rx
def test_st30p_channel(
    application,
    app_factory,
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_time,
    audio_channel,
    test_config,
    pcap_capture,
    media_file,
    output_files,
    media_integrity,
):
    """Test st30p with different audio channel configurations."""
    media_file_info, media_file_path = media_file

    if media_file_info["format"] in ["PCM16", "PCM24"] and audio_channel == "222":
        pytest.skip("Unsupported parameter combination")

    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )
    out_file_url = output_files.register(
        str(host.connection.path(media_file_path).parent / "out.pcm")
    )

    app = app_factory(
        application,
        session_type="st30p",
        audio_format=media_file_info["format"],
        audio_channels=[audio_channel],
    )
    app.create_command(
        session_type="st30p",
        nic_port_list=interfaces_list,
        test_mode="multicast",
        audio_format=media_file_info["format"],
        audio_channels=[audio_channel],
        audio_sampling="48kHz",
        audio_ptime="1",
        input_file=media_file_path,
        output_file=out_file_url,
        test_time=test_time,
    )

    app.execute_test(
        build=mtl_path,
        test_time=test_time,
        host=host,
        compliance=pcap_capture,
        integrity=media_integrity,
    )


@pytest.mark.smoke
@pytest.mark.low_bandwidth
@pytest.mark.nightly
@pytest.mark.parametrize(
    "application",
    [
        "rxtxapp",
        "ffmpeg",
        "gstreamer",
    ],
)
@pytest.mark.parametrize(
    "media_file",
    [
        audio_files["PCM8"],
        audio_files["PCM16"],
        audio_files["PCM24"],
    ],
    indirect=["media_file"],
    ids=["PCM8", "PCM16", "PCM24"],
)
@pytest.mark.tx_and_rx
def test_st30p_format(
    application,
    app_factory,
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_time,
    test_config,
    media_file,
    pcap_capture,
    output_files,
    media_integrity,
):
    """Test st30p with different audio formats (PCM8, PCM16, PCM24)."""
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )
    out_file_url = output_files.register(
        str(host.connection.path(media_file_path).parent / "out.pcm")
    )

    app = app_factory(
        application, session_type="st30p", audio_format=media_file_info["format"]
    )
    app.create_command(
        session_type="st30p",
        nic_port_list=interfaces_list,
        test_mode="multicast",
        audio_format=media_file_info["format"],
        audio_channels=["U02"],
        audio_sampling="48kHz",
        audio_ptime="1",
        input_file=media_file_path,
        output_file=out_file_url,
        test_time=test_time,
    )

    app.execute_test(
        build=mtl_path,
        test_time=test_time,
        host=host,
        compliance=pcap_capture,
        integrity=media_integrity,
    )


@pytest.mark.nightly
@pytest.mark.parametrize(
    "application",
    [
        "rxtxapp",
        "ffmpeg",
        "gstreamer",
    ],
)
@pytest.mark.parametrize(
    "media_file",
    [
        audio_files["PCM8"],
        audio_files["PCM16"],
        audio_files["PCM24"],
    ],
    indirect=["media_file"],
    ids=["PCM8", "PCM16", "PCM24"],
)
@pytest.mark.parametrize("audio_ptime", ["1", "0.12", "0.25", "0.33", "4"])
@pytest.mark.tx_and_rx
def test_st30p_ptime(
    application,
    app_factory,
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_time,
    audio_ptime,
    test_config,
    media_file,
    pcap_capture,
    output_files,
    media_integrity,
):
    """Test st30p with different ptime values."""
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )
    out_file_url = output_files.register(
        str(host.connection.path(media_file_path).parent / "out.pcm")
    )

    app = app_factory(
        application,
        session_type="st30p",
        audio_format=media_file_info["format"],
        audio_ptime=audio_ptime,
    )
    app.create_command(
        session_type="st30p",
        nic_port_list=interfaces_list,
        test_mode="multicast",
        audio_format=media_file_info["format"],
        audio_channels=["U02"],
        audio_sampling="48kHz",
        audio_ptime=audio_ptime,
        input_file=media_file_path,
        output_file=out_file_url,
        test_time=test_time,
    )

    app.execute_test(
        build=mtl_path,
        test_time=test_time,
        host=host,
        compliance=pcap_capture,
        integrity=media_integrity,
    )


@pytest.mark.nightly
@pytest.mark.parametrize(
    "application",
    [
        "rxtxapp",
        "ffmpeg",
        "gstreamer",
    ],
)
@pytest.mark.parametrize(
    "media_file",
    [
        audio_files["PCM8"],
        audio_files["PCM16"],
        audio_files["PCM24"],
    ],
    indirect=["media_file"],
    ids=["PCM8", "PCM16", "PCM24"],
)
@pytest.mark.parametrize("audio_sampling", ["48kHz", "96kHz"])
@pytest.mark.tx_and_rx
def test_st30p_sampling(
    application,
    app_factory,
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_time,
    audio_sampling,
    test_config,
    media_file,
    pcap_capture,
    output_files,
    media_integrity,
):
    """Test st30p with different sampling rates."""
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )
    out_file_url = output_files.register(
        str(host.connection.path(media_file_path).parent / "out.pcm")
    )

    app = app_factory(
        application,
        session_type="st30p",
        audio_format=media_file_info["format"],
        audio_sampling=audio_sampling,
    )
    app.create_command(
        session_type="st30p",
        nic_port_list=interfaces_list,
        test_mode="multicast",
        audio_format=media_file_info["format"],
        audio_channels=["U02"],
        audio_sampling=audio_sampling,
        audio_ptime="1",
        input_file=media_file_path,
        output_file=out_file_url,
        test_time=test_time,
    )

    app.execute_test(
        build=mtl_path,
        test_time=test_time,
        host=host,
        compliance=pcap_capture,
        integrity=media_integrity,
    )


@pytest.mark.nightly
@pytest.mark.parametrize(
    "application",
    [
        "rxtxapp",
        "ffmpeg",
        "gstreamer",
    ],
)
@pytest.mark.parametrize(
    "media_file",
    [
        audio_files["PCM8"],
        audio_files["PCM16"],
        audio_files["PCM24"],
    ],
    indirect=["media_file"],
    ids=["PCM8", "PCM16", "PCM24"],
)
@pytest.mark.tx_side
def test_st30p_multicast(
    application,
    app_factory,
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_time,
    test_config,
    media_file,
    pcap_capture,
):
    """Test st30p multicast transmission mode."""
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )
    log_dir = Path.cwd() / LOG_FOLDER / "latest"
    log_dir.mkdir(parents=True, exist_ok=True)
    out_file_url = str(log_dir / "out.wav")

    app = app_factory(
        application, session_type="st30p", audio_format=media_file_info["format"]
    )
    app.create_command(
        session_type="st30p",
        nic_port_list=interfaces_list,
        test_mode="multicast",
        audio_format=media_file_info["format"],
        audio_channels=["U02"],
        audio_sampling="48kHz",
        audio_ptime="1",
        input_file=media_file_path,
        output_file=out_file_url,
        test_time=test_time,
    )

    app.execute_test(
        build=mtl_path, test_time=test_time, host=host, compliance=pcap_capture
    )
