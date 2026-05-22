# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

import logging
from pathlib import Path

import pytest
from common.integrity.integrity_runner import FileAudioIntegrityRunner
from common.nicctl import InterfaceSetup
from mtl_engine.const import LOG_FOLDER
from mtl_engine.execute import log_fail
from mtl_engine.integrity import (
    calculate_st30p_framebuff_size,
    check_st30p_integrity,
    get_channel_number,
    get_sample_number,
    get_sample_size,
)
from mtl_engine.media_files import audio_files

logger = logging.getLogger(__name__)

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
            marks=pytest.mark.skip(reason="FFmpeg does not support st30p audio pipeline"),
        ),
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
):
    """Test st30p audio integrity (bit-exact comparison)."""
    media_file_info, media_file_path = media_file
    interfaces_list = setup_interfaces.get_interfaces_list_single(test_config.get("interface_type", "VF"))
    log_dir = Path.cwd() / LOG_FOLDER / "latest"
    log_dir.mkdir(parents=True, exist_ok=True)
    out_file_url = str(log_dir / "out.wav")
    host = list(hosts.values())[0]

    app = app_factory(application)
    app.create_command(
        session_type="st30p",
        nic_port_list=interfaces_list,
        test_mode="unicast",
        audio_format=media_file_info["format"],
        audio_channels=["U02"],
        audio_sampling="48kHz",
        audio_ptime="1",
        input_file=media_file_path,
        output_file=out_file_url,
        test_time=test_time,
    )

    app.execute_test(build=mtl_path, test_time=test_time, host=host, netsniff=pcap_capture)

    size = calculate_st30p_framebuff_size(format=media_file_info["format"], ptime="1", sampling="48kHz", channel="U02")
    result = check_st30p_integrity(src_url=media_file_path, out_url=out_file_url, size=size)
    if result:
        logger.info("INTEGRITY PASS")
    else:
        log_fail("INTEGRITY FAIL")


@pytest.mark.nightly
@pytest.mark.parametrize(
    "application",
    [
        "rxtxapp",
        "ffmpeg",
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
):
    """Test st30p with different audio channel configurations."""
    media_file_info, media_file_path = media_file

    if media_file_info["format"] in ["PCM16", "PCM24"] and audio_channel == "222":
        pytest.skip("Unsupported parameter combination")

    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(test_config.get("interface_type", "VF"))
    out_file_url = host.connection.path(media_file_path).parent / "out.pcm"

    app = app_factory(application)
    app.create_command(
        session_type="st30p",
        nic_port_list=interfaces_list,
        test_mode="multicast",
        audio_format=media_file_info["format"],
        audio_channels=[audio_channel],
        audio_sampling="48kHz",
        audio_ptime="1",
        input_file=media_file_path,
        output_file=str(out_file_url),
        test_time=test_time,
    )

    app.execute_test(
        build=mtl_path,
        test_time=test_time,
        host=host,
        netsniff=pcap_capture,
    )

    if test_config.get("integrity_check", True):
        logger.info("Running audio integrity check...")
        integrity = FileAudioIntegrityRunner(
            host=host,
            test_repo_path=mtl_path,
            src_url=media_file_path,
            out_name=out_file_url.name,
            channel_num=get_channel_number(audio_channel),
            sample_size=get_sample_size(media_file_info["format"]),
            out_path=str(out_file_url.parent),
            delete_file=True,
        )
        result = integrity.run()
        if not result:
            log_fail("Audio integrity check failed")


@pytest.mark.smoke
@pytest.mark.nightly
@pytest.mark.parametrize(
    "application",
    [
        "rxtxapp",
        "ffmpeg",
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
):
    """Test st30p with different audio formats (PCM8, PCM16, PCM24)."""
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(test_config.get("interface_type", "VF"))
    out_file_url = host.connection.path(media_file_path).parent / "out.pcm"

    app = app_factory(application)
    app.create_command(
        session_type="st30p",
        nic_port_list=interfaces_list,
        test_mode="unicast",
        audio_format=media_file_info["format"],
        audio_channels=["U02"],
        audio_sampling="48kHz",
        audio_ptime="1",
        input_file=media_file_path,
        output_file=str(out_file_url),
        test_time=test_time,
    )

    app.execute_test(build=mtl_path, test_time=test_time, host=host, netsniff=pcap_capture)
    if test_config.get("integrity_check", True):
        logger.info("Running audio integrity check...")
        integrity = FileAudioIntegrityRunner(
            host=host,
            test_repo_path=mtl_path,
            src_url=media_file_path,
            out_name=out_file_url.name,
            sample_size=get_sample_size(media_file_info["format"]),
            out_path=str(out_file_url.parent),
        )
        result = integrity.run()
        if not result:
            log_fail("Audio integrity check failed")


@pytest.mark.nightly
@pytest.mark.parametrize(
    "application",
    [
        "rxtxapp",
        "ffmpeg",
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
):
    """Test st30p with different ptime values."""
    # FFmpeg mtl_st30p plugin only supports ptime "1ms" and "125us".
    if application == "ffmpeg" and audio_ptime not in ("1", "0.12"):
        pytest.skip(f"FFmpeg st30p plugin does not support ptime={audio_ptime}")

    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(test_config.get("interface_type", "VF"))
    out_file_url = host.connection.path(media_file_path).parent / "out.pcm"

    app = app_factory(application)
    app.create_command(
        session_type="st30p",
        nic_port_list=interfaces_list,
        test_mode="unicast",
        audio_format=media_file_info["format"],
        audio_channels=["U02"],
        audio_sampling="48kHz",
        audio_ptime=audio_ptime,
        input_file=media_file_path,
        output_file=str(out_file_url),
        test_time=test_time,
    )

    app.execute_test(build=mtl_path, test_time=test_time, host=host, netsniff=pcap_capture)

    if test_config.get("integrity_check", True):
        logger.info("Running audio integrity check...")
        integrity = FileAudioIntegrityRunner(
            host=host,
            test_repo_path=mtl_path,
            src_url=media_file_path,
            out_name=out_file_url.name,
            sample_size=get_sample_size(media_file_info["format"]),
            out_path=str(out_file_url.parent),
        )
        result = integrity.run()
        if not result:
            log_fail("Audio integrity check failed")


@pytest.mark.nightly
@pytest.mark.parametrize(
    "application",
    [
        "rxtxapp",
        "ffmpeg",
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
):
    """Test st30p with different sampling rates."""
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(test_config.get("interface_type", "VF"))
    out_file_url = host.connection.path(media_file_path).parent / "out.pcm"

    app = app_factory(application)
    app.create_command(
        session_type="st30p",
        nic_port_list=interfaces_list,
        test_mode="unicast",
        audio_format=media_file_info["format"],
        audio_channels=["U02"],
        audio_sampling=audio_sampling,
        audio_ptime="1",
        input_file=media_file_path,
        output_file=str(out_file_url),
        test_time=test_time,
    )

    app.execute_test(build=mtl_path, test_time=test_time, host=host, netsniff=pcap_capture)

    if test_config.get("integrity_check", True):
        logger.info("Running audio integrity check...")
        integrity = FileAudioIntegrityRunner(
            host=host,
            test_repo_path=mtl_path,
            src_url=media_file_path,
            out_name=out_file_url.name,
            sample_size=get_sample_size(media_file_info["format"]),
            sample_num=get_sample_number(audio_sampling, "1"),
            out_path=str(out_file_url.parent),
        )
        result = integrity.run()
        if not result:
            log_fail("Audio integrity check failed")


@pytest.mark.nightly
@pytest.mark.parametrize(
    "application",
    [
        "rxtxapp",
        "ffmpeg",
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
    interfaces_list = setup_interfaces.get_interfaces_list_single(test_config.get("interface_type", "VF"))
    log_dir = Path.cwd() / LOG_FOLDER / "latest"
    log_dir.mkdir(parents=True, exist_ok=True)
    out_file_url = str(log_dir / "out.wav")

    app = app_factory(application)
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

    app.execute_test(build=mtl_path, test_time=test_time, host=host, netsniff=pcap_capture)
