# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation
# Framework mappings and configuration
# All parameter mappings, format conversions, and app configuration for RxTxApp, FFmpeg, and GStreamer

# ============================================================================
# RxTxApp Configuration
# ============================================================================

# Map framework names to executable names
APP_NAME_MAP = {
    "rxtxapp": "RxTxApp",
    "ffmpeg": "ffmpeg",
    "gstreamer": "gst-launch-1.0",
}

# RxTxApp command-line parameter mapping
# These parameters are passed as command-line arguments
RXTXAPP_CMDLINE_PARAM_MAP = {
    "config_file": "--config_file",
    "enable_ptp": "--ptp",
    "lcores": "--lcores",
    "test_time": "--test_time",
    "dma_dev": "--dma_dev",
    "log_level": "--log_level",
    "log_file": "--log_file",
    "arp_timeout_s": "--arp_timeout_s",
    "allow_across_numa_core": "--allow_across_numa_core",
    "no_multicast": "--no_multicast",
    "rx_separate_lcore": "--rx_separate_lcore",
    "rx_mix_lcore": "--rx_mix_lcore",
    "runtime_session": "--runtime_session",
    "rx_timing_parser": "--rx_timing_parser",
    "auto_stop": "--auto_stop",
    "rx_max_file_size": "--rx_max_file_size",
    "pcapng_dump": "--pcapng_dump",
    "rx_video_file_frames": "--rx_video_file_frames",
    "framebuffer_count": "--rx_video_fb_cnt",
    "promiscuous": "--promiscuous",
    "cni_thread": "--cni_thread",
    "sch_session_quota": "--sch_session_quota",
    "p_tx_dst_mac": "--p_tx_dst_mac",
    "r_tx_dst_mac": "--r_tx_dst_mac",
    "nb_tx_desc": "--nb_tx_desc",
    "nb_rx_desc": "--nb_rx_desc",
    "tasklet_time": "--tasklet_time",
    "tsc": "--tsc",
    "pacing_way": "--pacing_way",
    "shaping": "--shaping",
    "start_vrx": "--start_vrx",
    "pad_interval": "--pad_interval",
    "timestamp_epoch": "--timestamp_epoch",
    "ts_delta_us": "--ts_delta_us",
    "mono_pool": "--mono_pool",
    "tasklet_thread": "--tasklet_thread",
    "tasklet_sleep": "--tasklet_sleep",
    "tasklet_sleep_us": "--tasklet_sleep_us",
    "app_bind_lcore": "--app_bind_lcore",
    "rxtx_simd_512": "--rxtx_simd_512",
    "rss_mode": "--rss_mode",
    "tx_no_chain": "--tx_no_chain",
    "tx_copy_once": "--tx_copy_once",
    "multi_src_port": "--multi_src_port",
    "audio_fifo_size": "--audio_fifo_size",
    "dhcp": "--dhcp",
    "virtio_user": "--virtio_user",
    "afxdp_zc_disable": "--afxdp_zc_disable",
    "shared_tx_queues": "--shared_tx_queues",
    "shared_rx_queues": "--shared_rx_queues",
    "rx_burst_size": "--rx_burst_size",
    "static_pad": "--static_pad",
    "no_bulk": "--no_bulk",
    "random_src_port": "--random_src_port",
    "hdr_split": "--hdr_split",
    "rx_video_fb_cnt": "--rx_video_fb_cnt",
    "phc2sys": "--phc2sys",
    "ptp_sync_sys": "--ptp_sync_sys",
    "rss_sch_nb": "--rss_sch_nb",
    "log_time_ms": "--log_time_ms",
    "rx_audio_dump_time_s": "--rx_audio_dump_time_s",
    "dedicated_sys_lcore": "--dedicated_sys_lcore",
    "bind_numa": "--bind_numa",
    "force_numa": "--force_numa",
    "disable_migrate": "--disable_migrate",
}

# ============================================================================
# FFmpeg Configuration
# ============================================================================

# Format conversion mappings for FFmpeg: MTL pixel_format enum name (RxTxApp's
# input_format / output_format, e.g. from media_files.py's ``file_format``) ->
# FFmpeg AVPixelFormat name (the mtl_st20p plugin's ``-pix_fmt``).
FFMPEG_FORMAT_MAP = {
    "YUV422PLANAR10LE": "yuv422p10le",
    "YUV422PLANAR8": "yuv422p",
    "YUV420PLANAR8": "yuv420p",
    "YUV420PLANAR10LE": "yuv420p10le",
    "RGB24": "rgb24",
    "RGBA": "rgba",
    "Y210": "y210le",
    "UYVY": "uyvy422",
    "YUV420CUSTOM8": "yuv420p",
    "YUV422PLANAR12LE": "yuv422p12le",
    "YUV444PLANAR10LE": "yuv444p10le",
    "YUV444PLANAR12LE": "yuv444p12le",
    "GBRPLANAR10LE": "gbrp10le",
    "GBRPLANAR12LE": "gbrp12le",
}


def ffmpeg_pix_fmt(pixel_format: str) -> str:
    """Translate an MTL pixel_format enum name to its FFmpeg AVPixelFormat.

    Raises ``ValueError`` naming the unmapped enum instead of silently
    defaulting -- an unmapped format must never fall through to whatever
    FFmpeg's ``-pix_fmt`` default happens to be.
    """
    try:
        return FFMPEG_FORMAT_MAP[pixel_format]
    except KeyError:
        raise ValueError(
            f"No FFmpeg AVPixelFormat mapping for pixel_format={pixel_format!r}; "
            f"add it to FFMPEG_FORMAT_MAP in mtl_engine/config/mappings.py"
        )


# FFmpeg parameter mapping
# Maps universal params to FFmpeg MTL plugin flags.
# Width & height both map to -video_size; command builders coalesce them into WxH format.
# Framerate maps to -fps (distinct from input rawvideo's -framerate).
FFMPEG_PARAM_MAP = {
    # Network parameters
    "source_ip": "-p_sip",
    "destination_ip": "-p_tx_ip",  # TX unicast destination
    "multicast_ip": "-p_rx_ip",  # RX multicast group
    "port": "-udp_port",
    "nic_port": "-p_port",
    # Video parameters (width/height combined externally)
    "width": "-video_size",
    "height": "-video_size",
    "framerate": "-fps",
    "pixel_format": "-pix_fmt",
    # Streaming parameters
    "payload_type": "-payload_type",
    "session_type": "-f",  # Converted via SESSION_TYPE_MAP
    # File I/O
    "input_file": "-i",
    "output_file": "",  # Output appears last (no explicit flag)
}

# Universal audio_ptime (milliseconds, as a string) -> the mtl_st30p muxer's
# ``-ptime`` value. The plugin implements these two packet times only
# (ecosystem/ffmpeg_plugin/mtl_st30p_{tx,rx}.c).
FFMPEG_ST30P_PTIME_MAP = {
    "1": "1ms",
    "0.12": "125us",
    "0.125": "125us",
    "125us": "125us",
}


def ffmpeg_ptime(audio_ptime) -> str:
    """Translate a universal audio_ptime to the FFmpeg ``-ptime`` spelling.

    Raises ``ValueError`` instead of defaulting to ``1ms``: a silent fallback
    would stream a different packet time than the test asked for and still
    report a pass.
    """
    try:
        return FFMPEG_ST30P_PTIME_MAP[str(audio_ptime)]
    except KeyError:
        raise ValueError(
            f"The FFmpeg mtl_st30p plugin does not support "
            f"audio_ptime={audio_ptime!r}; supported: "
            f"{sorted(FFMPEG_ST30P_PTIME_MAP)}"
        )


# Default FFmpeg configuration
DEFAULT_FFMPEG_CONFIG = {
    "default_pixel_format": "yuv422p10le",
    "default_session_type": "mtl_st20p",
}

# ============================================================================
# GStreamer Configuration
# ============================================================================

# MTL pixel_format enum name -> GstVideoFormat nick understood by
# ``rawvideoparse format=`` (and by the plugin's ``rx-pixel-format``, which
# takes the MTL name instead).
#
# The list is short on purpose: ecosystem/gstreamer_plugin/gst_mtl_st20p_rx.c
# converts exactly two MTL frame formats (ST_FRAME_FMT_V210 and
# ST_FRAME_FMT_YUV422PLANAR10LE). Everything else is unsupported by the plugin,
# not merely unmapped here.
GSTREAMER_ST20P_FORMAT_MAP = {
    "YUV422PLANAR10LE": "i422-10le",
    "v210": "v210",
}


def gstreamer_video_format(pixel_format: str) -> str:
    """Translate an MTL pixel_format enum name to its GstVideoFormat nick.

    Raises ``ValueError`` naming the unsupported enum instead of silently
    defaulting -- a wrong ``rawvideoparse format=`` would reinterpret the
    source bytes and turn a plugin gap into a corrupt-pixels failure.
    """
    try:
        return GSTREAMER_ST20P_FORMAT_MAP[pixel_format]
    except KeyError:
        raise ValueError(
            f"The MTL GStreamer st20p plugin does not support "
            f"pixel_format={pixel_format!r}; supported: "
            f"{sorted(GSTREAMER_ST20P_FORMAT_MAP)}"
        )


# MTL fps token (RxTxApp's "pNN" naming, see the fps parsing in
# tests/tools/RxTxApp/src/parse_json.c) -> exact GstFraction.
#
# The plugin converts the caps fraction with ``st_frame_rate_to_st_fps()``,
# which matches the *decimal* value against st_fps_timings[]
# (lib/src/st2110/st_fmt.c) using per-entry tolerance windows. Emitting the
# canonical mul/den pair from that table is the only way to guarantee the
# intended enum is picked for the 1000/1001 rates -- e.g. "2997/100" is a
# different number from 30000/1001 and lands outside ST_FPS_P29_97's window.
GSTREAMER_FRAMERATE_MAP = {
    "p23": "24000/1001",
    "p24": "24/1",
    "p25": "25/1",
    "p29": "30000/1001",
    "p30": "30/1",
    "p50": "50/1",
    "p59": "60000/1001",
    "p60": "60/1",
    "p100": "100/1",
    "p119": "120000/1001",
    "p120": "120/1",
}


def gstreamer_framerate(framerate: str) -> str:
    """Translate an MTL fps token (``p25``, ``p59``, ...) to a GstFraction.

    Interlaced tokens (``i50``) select the same rate as their progressive
    counterpart; the field split is carried by the ``interlaced`` parameter.
    """
    token = f"p{str(framerate).lstrip('pi')}"
    try:
        return GSTREAMER_FRAMERATE_MAP[token]
    except KeyError:
        raise ValueError(
            f"No GStreamer framerate mapping for framerate={framerate!r}; "
            f"supported: {sorted(GSTREAMER_FRAMERATE_MAP)}"
        )


# MTL audio_format name -> GstAudioFormat nick. Matches the st30p pad templates
# (format = { S8, S16BE, S24BE }) in gst_mtl_st30p_{tx,rx}.c; the plugin's own
# ``rx-audio-format`` property takes the MTL name instead.
GSTREAMER_AUDIO_FORMAT_MAP = {
    "PCM8": "s8",
    "PCM16": "s16be",
    "PCM24": "s24be",
}


def gstreamer_audio_format(audio_format: str) -> str:
    """Translate an MTL audio_format name to its GstAudioFormat nick."""
    try:
        return GSTREAMER_AUDIO_FORMAT_MAP[audio_format]
    except KeyError:
        raise ValueError(
            f"The MTL GStreamer st30p plugin does not support "
            f"audio_format={audio_format!r}; supported: "
            f"{sorted(GSTREAMER_AUDIO_FORMAT_MAP)}"
        )


# Universal audio_ptime (milliseconds, as a string) -> the ptime spelling
# ``gst_mtl_common_parse_ptime()`` accepts.
GSTREAMER_AUDIO_PTIME_MAP = {
    "0.08": "80us",
    "0.09": "0.09ms",
    "0.12": "125us",
    "0.125": "125us",
    "0.14": "0.14ms",
    "0.25": "250us",
    "0.33": "333us",
    "1": "1ms",
    "1.09": "1.09ms",
    "4": "4ms",
}


def gstreamer_ptime(audio_ptime) -> str:
    """Translate a universal audio_ptime to a ``tx-ptime``/``rx-ptime`` value."""
    try:
        return GSTREAMER_AUDIO_PTIME_MAP[str(audio_ptime)]
    except KeyError:
        raise ValueError(
            f"``gst_mtl_common_parse_ptime()`` does not accept "
            f"audio_ptime={audio_ptime!r}; supported: "
            f"{sorted(GSTREAMER_AUDIO_PTIME_MAP)}"
        )


# ============================================================================
# Common Configuration
# ============================================================================

# ST 2110-20 packing mode libmtl uses when a session does not ask for one.
# Neither plugin exposes a packing knob, so this is the only mode they can
# produce -- see the ``unsupported_reason()`` guards in ffmpeg.py/gstreamer.py.
MTL_DEFAULT_PACKING = "BPM"

# ST 2110-30 channel-group label -> channel count. Shared by every adapter that
# has to spell the count out (FFmpeg's ``-ac``, GStreamer's caps/``rx-channel``);
# RxTxApp passes the label straight through to its JSON config instead.
AUDIO_CHANNEL_COUNTS = {
    "M": 1,
    "DM": 2,
    "ST": 2,
    "LtRt": 2,
    "51": 6,
    "71": 8,
    "222": 24,
    "SGRP": 4,
    "U01": 1,
    "U02": 2,
}

# Universal audio_sampling label -> rate in Hz.
AUDIO_SAMPLING_HZ = {
    "44.1kHz": 44100,
    "48kHz": 48000,
    "96kHz": 96000,
}


def audio_channel_count(audio_channels) -> int:
    """Total channel count for a universal ``audio_channels`` value.

    Accepts the list form used by the tests (``["U02"]``) or a bare label.
    Multiple groups sum, so ``["ST", "M"]`` is 3 channels.
    """
    labels = (
        audio_channels
        if isinstance(audio_channels, (list, tuple))
        else [audio_channels]
    )
    total = 0
    for label in labels:
        try:
            total += AUDIO_CHANNEL_COUNTS[label]
        except KeyError:
            raise ValueError(
                f"Unknown audio channel group {label!r}; supported: "
                f"{sorted(AUDIO_CHANNEL_COUNTS)}"
            )
    return total


def audio_sampling_hz(audio_sampling) -> int:
    """Sample rate in Hz for a universal ``audio_sampling`` label."""
    try:
        return AUDIO_SAMPLING_HZ[audio_sampling]
    except KeyError:
        raise ValueError(
            f"Unknown audio_sampling {audio_sampling!r}; supported: "
            f"{sorted(AUDIO_SAMPLING_HZ)}"
        )


# FFmpeg ``-f`` device name per session type. The GStreamer plugin needs no such
# map: it names an element pair per session type (see mtl_engine.gstreamer).
SESSION_TYPE_MAP = {
    "ffmpeg": {
        "st20p": "mtl_st20p",
        "st22p": "mtl_st22p",
        "st30p": "mtl_st30p",
    },
}
