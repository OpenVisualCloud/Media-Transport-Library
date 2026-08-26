# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation
"""FFmpeg framework adapter (unified Application model).

Mirrors the ``RxTxApp`` adapter pattern in :mod:`mtl_engine.rxtxapp` but for the
FFmpeg MTL plugin. Reuses the legacy command/config builders and validators
from :mod:`mtl_engine.ffmpeg_app` so we keep a single source of truth for
command-line strings, JSON configs and pass/fail criteria.

Lifecycle (single host only — these tests never split TX/RX across hosts):
    1. ``create_command(...)`` — build the RX command (stored in ``self.command``)
       and any TX commands (stored in ``self._tx_commands``).
    2. ``prepare_execution(build, host)`` — create empty output files on the host.
    3. ``execute_test(build, test_time, host)`` — start RX, sleep, start TX(s),
       wait, stop everything, validate.
    4. ``validate_results()`` — dispatches by mode (``yuv``/``h264``/``rgb24``).
"""

from __future__ import annotations

import logging

from mtl_engine import ffmpeg_app, ip_pools
from mtl_engine.application_base import (
    MTL_ENCODER_PLUGIN_MAP,
    Application,
    ProcSpec,
    mtl_plugin_check_cmd,
)
from mtl_engine.config.mappings import (
    APP_NAME_MAP,
    FFMPEG_FORMAT_MAP,
    FFMPEG_ST30P_PTIME_MAP,
    MTL_DEFAULT_PACKING,
    SESSION_TYPE_MAP,
    audio_channel_count,
    audio_sampling_hz,
    ffmpeg_pix_fmt,
    ffmpeg_ptime,
)
from mtl_engine.const import FFMPEG_EXE, RXTXAPP_EXE
from mtl_engine.integrity_session import NO_INTEGRITY
from mtl_engine.pcap_compliance import NO_COMPLIANCE

logger = logging.getLogger(__name__)


# Modes selected by tests — each maps onto one of the legacy execute_test_*
# command-generation paths.
_MODE_YUV_H264 = "yuv_h264"  # FFmpeg RX + (FFmpeg or RxTxApp) TX, yuv|h264 output
_MODE_RGB24 = "rgb24"  # FFmpeg TX (rgb24) + RxTxApp RX, single stream
_MODE_RGB24_MULTI = "rgb24_multiple"  # FFmpeg TX×2 (rgb24) + RxTxApp RX, two streams
_MODE_ST22P = "st22p"  # FFmpeg RX + FFmpeg TX via mtl_st22p (compressed video)
_MODE_ST30P = "st30p"  # FFmpeg RX + FFmpeg TX via mtl_st30p (audio)

# Map session_type → FFmpeg mode (used when no explicit ``mode`` kwarg given).
_SESSION_TYPE_TO_MODE = {
    "st22p": _MODE_ST22P,
    "st30p": _MODE_ST30P,
}


class FFmpeg(Application):
    """FFmpeg framework adapter (single-host RX+TX orchestrator)."""

    # Parameters the FFmpeg adapter understands but which do not belong in
    # ``UNIVERSAL_PARAMS``. Stripped from ``set_params`` and routed into
    # ``self._ff_params`` instead so the base class' validation does not reject
    # them.
    _FFMPEG_ONLY_KEYS = (
        "output_format",  # "yuv" | "h264"
        "multiple_sessions",  # bool
        "tx_is_ffmpeg",  # bool — False: TX is RxTxApp instead of FFmpeg
        "mode",  # "yuv_h264" | "rgb24" | "rgb24_multiple" | "st22p" | "st30p"
        "video_format_list",  # list[str]  — rgb24_multiple only
        "video_url_list",  # list[str]   — rgb24_multiple only
        "pix_fmt",  # str — yuv_h264 only; default "yuv422p10le"
        "st22_codec",  # str — st22p only; default "jpegxs"
        "bpp",  # float — st22p only; bits per pixel, default 3.0
    )

    def __init__(self, app_path, config_file_path=None):
        super().__init__(app_path, config_file_path)
        self._ff_params: dict = {}
        self._tx_commands: list[str] = []
        self._build: str | None = None
        self._rx_output: str | None = None

    # ------------------------------------------------------------------ ABCs
    def get_app_name(self) -> str:
        return "FFmpeg"

    def get_executable_name(self) -> str:
        return APP_NAME_MAP["ffmpeg"]

    # ----------------------------------------------------- capabilities
    def unsupported_reason(self, **params):
        """Report MTL FFmpeg plugin gaps so a shared test can skip cleanly."""
        session_type = params.get("session_type")
        if session_type is not None and session_type not in SESSION_TYPE_MAP["ffmpeg"]:
            # ecosystem/ffmpeg_plugin registers muxers/demuxers for these
            # session types only -- there is no ancillary (st40p) or
            # fastmetadata (st41) device to select with ``-f``.
            return (
                f"The MTL FFmpeg plugin implements "
                f"{sorted(SESSION_TYPE_MAP['ffmpeg'])} devices, "
                f"not session_type={session_type}"
            )
        if session_type == "st20p":
            pixel_format = params.get("pixel_format")
            if pixel_format is not None and pixel_format not in FFMPEG_FORMAT_MAP:
                return (
                    f"No FFmpeg pixel format carries MTL "
                    f"pixel_format={pixel_format}; the mtl_st20p plugin "
                    f"handles {sorted(FFMPEG_FORMAT_MAP)}"
                )
            packing = params.get("packing")
            if packing not in (None, MTL_DEFAULT_PACKING):
                # mtl_st20p_tx.c declares no packing option, so the session
                # keeps the library default; accepting GPM here would report a
                # pass for a mode that never reached the wire.
                return (
                    f"The FFmpeg mtl_st20p plugin has no packing option and "
                    f"leaves the library default {MTL_DEFAULT_PACKING}, "
                    f"not packing={packing}"
                )
        if session_type == "st30p":
            audio_ptime = params.get("audio_ptime")
            if (
                audio_ptime is not None
                and str(audio_ptime) not in FFMPEG_ST30P_PTIME_MAP
            ):
                return (
                    f"The FFmpeg mtl_st30p plugin implements only "
                    f"{sorted(FFMPEG_ST30P_PTIME_MAP)} packet times, "
                    f"not audio_ptime={audio_ptime}"
                )
        return None

    def require_encoder(self, host, encoder: str, use_mtl_plugin: bool = False) -> None:
        """Raise EnvironmentError if *encoder* is not available on *host*.

        When *use_mtl_plugin* is True the codec is delivered by the MTL st22p
        device (FFmpeg uses ``-f mtl_st22p``), so the matching MTL plugin .so is
        checked instead of FFmpeg's built-in encoder list. Otherwise FFmpeg's
        own encoder (e.g. ``-c:v libopenh264`` for an st20p YUV->H264 pipe) is
        verified against ``ffmpeg -encoders``.
        """
        if use_mtl_plugin:
            # Codecs delivered via MTL plugin (FFmpeg uses -f mtl_st22p)
            plugin_so = MTL_ENCODER_PLUGIN_MAP.get(encoder)
            if plugin_so:
                res = host.connection.execute_command(
                    mtl_plugin_check_cmd(plugin_so),
                    shell=True,
                    expected_return_codes=None,
                )
                if res.return_code != 0:
                    raise EnvironmentError(
                        f"MTL codec plugin {plugin_so} (for {encoder}) not found; "
                        f"install the codec library and rebuild MTL plugins"
                    )
            return

        res = host.connection.execute_command(
            f"ffmpeg -encoders 2>/dev/null | grep {encoder} || true",
            shell=True,
            timeout=10,
        )
        if encoder not in (res.stdout or ""):
            raise EnvironmentError(
                f"{encoder} encoder not available in FFmpeg; "
                f"install the codec library and rebuild FFmpeg"
            )

    # ----------------------------------------------------- param plumbing
    def set_params(self, **kwargs):
        """Strip FFmpeg-only kwargs into ``self._ff_params`` first, then defer.

        UNIVERSAL_PARAMS does not (and should not) carry FFmpeg-specific keys
        like ``output_format`` or ``tx_is_ffmpeg``. Filtering them here avoids
        ``ValueError: Unknown parameter`` from :meth:`Application.set_params`.
        """
        self._ff_params = {}
        for key in self._FFMPEG_ONLY_KEYS:
            if key in kwargs:
                self._ff_params[key] = kwargs.pop(key)
        super().set_params(**kwargs)

    # ----------------------------------------------------- command build
    # Shared template for an RxTxApp-driven RX side. ``rx_cfg`` is filled in
    # by ``prepare_execution`` once the live host is known.
    _RXTXAPP_RX_TMPL = (
        f"{RXTXAPP_EXE} --config_file {{rx_cfg}} --test_time {{test_time}}"
    )

    @staticmethod
    def _ffmpeg_st20p_tx_cmd(
        *,
        video_size: str,
        pix_fmt: str,
        video_url: str,
        port: str,
        sip: str,
        mcast: str,
        framerate=None,
        filter_v: str = "",
        udp_port: int = 20000,
        ptp_enable: bool = False,
        pacing_way=None,
    ) -> str:
        """Build an FFmpeg → mtl_st20p TX command line.

        Used by every mode: yuv|h264 (raw 422p10le), rgb24 (yuv422p10be →
        rgb24 filter, single stream), rgb24_multiple (same, two streams).
        ``framerate`` only emitted for the rgb24 family — yuv_h264 omits
        ``-framerate`` because the source already carries the canonical fps.
        ``filter_v`` is the full ``-filter:v <chain>`` token (empty by default).
        ``ptp_enable`` is independent from ``pacing_way``.
        """
        framerate_token = f"-framerate {framerate} " if framerate is not None else ""
        filter_token = f"{filter_v} " if filter_v else ""
        ptp_token = ""
        if ptp_enable:
            ptp_token += "-ptp_enable 1 "
        if pacing_way:
            ptp_token += f"-pacing_way {pacing_way} "
        # ``-re`` throttles the rawvideo reader to the source framerate. Without
        # it FFmpeg drains the (ramdisk) file as fast as it can and hands frames
        # to st20p_tx_put_frame() faster than realtime; MTL then transmits ahead
        # of the ST 2110-21 epoch (observed: 38.3 ms frame gaps instead of 40 ms,
        # drifting ~1.7 ms earlier per frame) and VRX goes deeply negative.
        # RxTxApp gets the same rate limiting implicitly from MTL back-pressure.
        return (
            f"{FFMPEG_EXE} -stream_loop -1 -re {framerate_token}"
            f"-video_size {video_size} -f rawvideo -pix_fmt {pix_fmt} "
            f"-i {video_url} {filter_token}"
            f"-p_port {port} -p_sip {sip} -p_tx_ip {mcast} "
            f"-udp_port {udp_port} -payload_type 112 {ptp_token}-f mtl_st20p -"
        )

    def _create_command_and_config(self) -> tuple:
        """Build RX + TX command strings for the requested mode.

        Returns ``(rx_cmd, None)``. TX commands live in ``self._tx_commands``.
        Output files (when produced by RX) live in ``self._output_files`` —
        populated lazily in :meth:`prepare_execution` because file creation
        needs the live ``host`` connection.
        """
        # Resolve mode: explicit ``mode`` kwarg takes precedence; otherwise
        # infer from ``session_type`` (allows unified tests to pass
        # session_type="st22p" without also specifying mode="st22p").
        mode = self._ff_params.get("mode")
        tx_application = self.params.get("tx_application")
        rx_application = self.params.get("rx_application")
        if tx_application is not None or rx_application is not None:
            pair = (tx_application, rx_application)
            if pair == ("rxtxapp", "ffmpeg"):
                mode = _MODE_YUV_H264
                self._ff_params["tx_is_ffmpeg"] = False
            elif pair == ("ffmpeg", "rxtxapp"):
                mode = _MODE_RGB24
            else:
                raise ValueError(f"unsupported cross-application pair: {pair}")
        if mode is None:
            session_type = self.params.get("session_type", "st20p")
            mode = _SESSION_TYPE_TO_MODE.get(session_type, _MODE_YUV_H264)
        self._ff_params["mode"] = mode
        nic_port_list = self.params["nic_port_list"]
        if not nic_port_list:
            raise ValueError("nic_port_list is required")

        if mode == _MODE_YUV_H264:
            return self._build_yuv_h264_cmds(nic_port_list)
        if mode == _MODE_RGB24:
            return self._build_rgb24_cmds(nic_port_list)
        if mode == _MODE_RGB24_MULTI:
            return self._build_rgb24_multiple_cmds(nic_port_list)
        if mode == _MODE_ST22P:
            return self._build_st22p_cmds(nic_port_list)
        if mode == _MODE_ST30P:
            return self._build_st30p_cmds(nic_port_list)
        raise ValueError(f"Unknown FFmpeg mode: {mode}")

    # -- mode: yuv|h264 (FFmpeg RX, FFmpeg-or-RxTxApp TX) ----------------
    def _build_yuv_h264_cmds(self, nic_port_list):
        video_format = self.params["video_format"]
        video_url = self.params["video_url"] or self.params.get("input_file", "")
        # Synthesize legacy video_format from modern params when not provided.
        if not video_format:
            h = self.params.get("height", 1080)
            fr = self.params.get("framerate", "p60")
            fps_num = fr.lstrip("pi")
            video_format = f"i{h}p{fps_num}"
        # Persist resolved values so validate_results() can use them later.
        self.params["video_format"] = video_format
        self.params["video_url"] = video_url
        output_format = self._ff_params.get("output_format", "yuv")
        multiple = bool(self._ff_params.get("multiple_sessions", False))
        tx_is_ffmpeg = bool(self._ff_params.get("tx_is_ffmpeg", True))

        # video_size: prefer explicit width/height (the universal params
        # RxTxApp reads) over video_format, which decode_video_format_16_9
        # can only derive by assuming a 16:9 aspect ratio.
        if self.was_user_provided("width") and self.was_user_provided("height"):
            video_size = f"{self.params['width']}x{self.params['height']}"
        else:
            video_size, _ = ffmpeg_app.decode_video_format_16_9(video_format)

        # fps: prefer explicit framerate over the digits scraped out of
        # video_format, for the same reason.
        if self.was_user_provided("framerate"):
            fps = self.extract_framerate(self.params["framerate"])
        else:
            _, fps = ffmpeg_app.decode_video_format_16_9(video_format)

        # pix_fmt: an explicit override (rgb24/st22p modes, or a test that
        # deliberately mismatches TX/RX) always wins; otherwise derive it from
        # the universal pixel_format param so callers never have to know
        # FFmpeg's own AVPixelFormat naming. Only fall back to the historical
        # yuv422p10le default when neither was given.
        if "pix_fmt" in self._ff_params:
            pix_fmt = self._ff_params["pix_fmt"]
        elif self.was_user_provided("pixel_format"):
            pix_fmt = ffmpeg_pix_fmt(self.params["pixel_format"])
        else:
            pix_fmt = "yuv422p10le"

        rx_f_flag = "-f rawvideo" if output_format == "yuv" else "-c:v libopenh264"

        # IMPORTANT: -init_retry is an AVOption on the mtl_st20p *demuxer*, so
        # it MUST appear BEFORE the corresponding -i. If placed after -i, the
        # ffmpeg CLI binds it to the next output (e.g. rawvideo muxer), which
        # does not recognise it, libavutil silently drops it, and the demuxer
        # falls back to its default value of 5. With only 5 non-blocking
        # retries the RX tears down before TX finishes tv_train_pacing (~8s)
        # and the test fails with EIO + 0-byte output for every pix_fmt.
        if not multiple:
            rx_cmd = (
                f"{FFMPEG_EXE} -p_port {nic_port_list[1]} "
                f"-p_sip {ip_pools.rx[0]} "
                f"-p_rx_ip {ip_pools.rx_multicast[0]} -udp_port 20000 "
                f"-payload_type 112 -fps {fps} -pix_fmt {pix_fmt} "
                f"-video_size {video_size} -init_retry 20 "
                f"-f mtl_st20p -i k "
                f"{rx_f_flag} {{out0}} -y"
            )
        else:
            rx_cmd = (
                f"{FFMPEG_EXE} -p_sip {ip_pools.rx[0]} "
                f"-p_port {nic_port_list[1]} "
                f"-p_rx_ip {ip_pools.rx_multicast[0]} -udp_port 20000 "
                f"-payload_type 112 -fps {fps} -pix_fmt {pix_fmt} "
                f"-video_size {video_size} -init_retry 20 "
                f"-f mtl_st20p -i 1 "
                f"-p_port {nic_port_list[1]} "
                f"-p_rx_ip {ip_pools.rx_multicast[0]} -udp_port 20002 "
                f"-payload_type 112 -fps {fps} -pix_fmt {pix_fmt} "
                f"-video_size {video_size} -init_retry 20 "
                f"-f mtl_st20p -i 2 "
                f"-map 0:0 {rx_f_flag} {{out0}} -y "
                f"-map 1:0 {rx_f_flag} {{out1}} -y"
            )

        if tx_is_ffmpeg:
            # Lock the rate on the rawvideo *demuxer* (-framerate) rather than a
            # post-hoc "fps" filter, which would add a filtergraph frame copy
            # to the thread feeding st20p_tx_put_frame() on its 40ms epoch.
            #
            # TX must use nic_port_list[0] and RX nic_port_list[1], matching
            # RxTxApp's convention: the pcap_capture fixture sniffs the second
            # NIC's PF, so TX must egress a different NIC or netsniff-ng only
            # sees switch-batched packets, making VRX measurement meaningless.
            self._tx_commands = [
                self._ffmpeg_st20p_tx_cmd(
                    video_size=video_size,
                    pix_fmt=pix_fmt,
                    video_url=video_url,
                    port=nic_port_list[0],
                    sip=ip_pools.tx[0],
                    mcast=ip_pools.rx_multicast[0],
                    framerate=fps,
                    ptp_enable=self.params.get("enable_ptp", False),
                    pacing_way=self.params.get("pacing_way"),
                )
            ]
        else:
            # RxTxApp TX — config file generated lazily in prepare_execution()
            # because ffmpeg_app.generate_rxtxapp_tx_config needs the host.
            self._tx_commands = []  # filled in by prepare_execution
        return rx_cmd, None

    # -- mode: rgb24 (FFmpeg TX, RxTxApp RX, single stream) --------------
    def _build_rgb24_cmds(self, nic_port_list):
        video_format = self.params["video_format"]
        if not video_format:
            height = self.params.get("height", 1080)
            fps = self.extract_framerate(self.params.get("framerate", "p60"))
            video_format = f"i{height}p{fps}"
        video_url = self.params["video_url"] or self.params.get("input_file", "")
        if self.was_user_provided("width") and self.was_user_provided("height"):
            video_size = f"{self.params['width']}x{self.params['height']}"
            fps = self.extract_framerate(self.params["framerate"])
        else:
            video_size, fps = ffmpeg_app.decode_video_format_16_9(video_format)
        self.params["video_format"] = video_format
        self.params["video_url"] = video_url
        pix_fmt = ffmpeg_pix_fmt(self.params["pixel_format"])
        self._tx_commands = [
            self._ffmpeg_st20p_tx_cmd(
                video_size=video_size,
                pix_fmt=pix_fmt,
                video_url=video_url,
                port=nic_port_list[1],
                sip=ip_pools.tx[0],
                mcast=ip_pools.rx_multicast[0],
                framerate=fps,
                filter_v="-filter:v format=rgb24",
                pacing_way=self.params.get("pacing_way"),
            )
        ]
        return self._RXTXAPP_RX_TMPL, None

    # -- mode: rgb24_multiple (FFmpeg TX×2, RxTxApp RX, two streams) -----
    def _build_rgb24_multiple_cmds(self, nic_port_list):
        video_format_list = self._ff_params["video_format_list"]
        video_url_list = self._ff_params["video_url_list"]
        if len(nic_port_list) < 4:
            raise ValueError(
                "rgb24_multiple requires 4 NIC ports (2 RX + 2 TX), "
                f"got {len(nic_port_list)}"
            )
        # Two parallel streams: stream i uses TX port [2+i], src ip pool [i],
        # multicast pool [i]. Ports [0]/[1] are the RX side (RxTxApp).
        self._tx_commands = []
        for i in range(2):
            v, f = ffmpeg_app.decode_video_format_16_9(video_format_list[i])
            self._tx_commands.append(
                self._ffmpeg_st20p_tx_cmd(
                    video_size=v,
                    pix_fmt="yuv422p10be",
                    video_url=video_url_list[i],
                    port=nic_port_list[2 + i],
                    sip=ip_pools.tx[i],
                    mcast=ip_pools.rx_multicast[i],
                    framerate=f,
                    filter_v="-filter:v format=rgb24",
                    pacing_way=self.params.get("pacing_way"),
                )
            )
        return self._RXTXAPP_RX_TMPL, None

    # -- mode: st22p (FFmpeg TX via mtl_st22p, FFmpeg RX via mtl_st22p) --
    def _build_st22p_cmds(self, nic_port_list):
        width = int(self.params["width"])
        height = int(self.params["height"])
        video_size = f"{width}x{height}"
        framerate = self.params.get("framerate", "p60")
        # Convert "pXX" string to numeric fps for FFmpeg -framerate flag.
        fps_num = framerate.lstrip("p") if isinstance(framerate, str) else framerate
        pix_fmt = self._ff_params.get("pix_fmt", "yuv422p10le")
        st22_codec = self._ff_params.get("st22_codec", "jpegxs")
        bpp = self._ff_params.get("bpp", 3.0)
        input_file = self.params.get("input_file") or self.params.get("video_url")
        udp_port = 20000

        # Map UNIVERSAL_PARAMS codec names to FFmpeg st22_codec string.
        codec_name = self.params.get("codec", "JPEG-XS")
        codec_map = {"JPEG-XS": "jpegxs", "H264_CBR": "h264", "H265": "h265"}
        st22_codec = codec_map.get(codec_name, st22_codec)

        # TX: raw video → mtl_st22p muxer (encode + transmit)
        self._tx_commands = [
            f"{FFMPEG_EXE} -stream_loop -1 "
            f"-video_size {video_size} -f rawvideo -pix_fmt {pix_fmt} "
            f"-i {input_file} "
            f"-filter:v fps={fps_num} "
            f"-p_port {nic_port_list[1]} -p_sip {ip_pools.tx[0]} "
            f"-p_tx_ip {ip_pools.rx_multicast[0]} "
            f"-udp_port {udp_port} -payload_type 112 "
            f"-st22_codec {st22_codec} -bpp {bpp} "
            f"-f mtl_st22p -"
        ]

        # RX: mtl_st22p demuxer (receive + decode) → raw video output
        rx_cmd = (
            f"{FFMPEG_EXE} -p_port {nic_port_list[0]} "
            f"-p_sip {ip_pools.rx[0]} "
            f"-p_rx_ip {ip_pools.rx_multicast[0]} -udp_port {udp_port} "
            f"-payload_type 112 -fps {fps_num} -pix_fmt {pix_fmt} "
            f"-video_size {video_size} "
            f"-st22_codec {st22_codec} "
            f"-init_retry 20 "
            f"-f mtl_st22p -i k "
            f"-f rawvideo {{out0}} -y"
        )
        return rx_cmd, None

    # -- mode: st30p (FFmpeg TX via mtl_st30p, FFmpeg RX via mtl_st30p) --
    def _build_st30p_cmds(self, nic_port_list):
        audio_format = self.params.get("audio_format", "PCM24")
        audio_sampling = self.params.get("audio_sampling", "48kHz")
        audio_ptime = self.params.get("audio_ptime", "1")
        audio_channels = self.params.get("audio_channels", ["U02"])
        input_file = self.params.get("input_file") or self.params.get("audio_url")
        udp_port = 30000

        # Map RxTxApp format names → FFmpeg pcm codec / format strings / muxer.
        # The mtl_st30p muxer is PCM24-only; for PCM16 use mtl_st30p_pcm16.
        # For PCM8 there is no dedicated muxer, but the mtl_st30p write_header
        # supports AV_CODEC_ID_PCM_S8 — we force it with an explicit -c:a.
        fmt_map = {
            "PCM24": ("s24be", "pcm24", "mtl_st30p", ""),
            "PCM16": ("s16be", "pcm16", "mtl_st30p_pcm16", ""),
            "PCM8": ("s8", "pcm8", "mtl_st30p", "-c:a pcm_s8 "),
        }
        if audio_format not in fmt_map:
            # Falling back to PCM24 would stream a different format than the
            # test asked for and still report a pass.
            raise ValueError(
                f"The FFmpeg mtl_st30p plugin does not support "
                f"audio_format={audio_format!r}; supported: {sorted(fmt_map)}"
            )
        ff_fmt, pcm_fmt, tx_muxer, tx_codec = fmt_map[audio_format]

        sample_rate = audio_sampling_hz(audio_sampling)
        channels = audio_channel_count(audio_channels)
        ptime_str = ffmpeg_ptime(audio_ptime)

        # TX: raw audio → mtl_st30p muxer (format-specific)
        self._tx_commands = [
            f"{FFMPEG_EXE} -stream_loop -1 "
            f"-f {ff_fmt} -ar {sample_rate} -ac {channels} "
            f"-i {input_file} "
            f"{tx_codec}"
            f"-p_port {nic_port_list[1]} -p_sip {ip_pools.tx[0]} "
            f"-p_tx_ip {ip_pools.rx_multicast[0]} "
            f"-udp_port {udp_port} -payload_type 111 "
            f"-ptime {ptime_str} "
            f"-f {tx_muxer} -"
        ]

        # RX: mtl_st30p demuxer → raw audio output
        rx_cmd = (
            f"{FFMPEG_EXE} -p_port {nic_port_list[0]} "
            f"-p_sip {ip_pools.rx[0]} "
            f"-p_rx_ip {ip_pools.rx_multicast[0]} -udp_port {udp_port} "
            f"-payload_type 111 -sample_rate {sample_rate} "
            f"-channels {channels} -pcm_fmt {pcm_fmt} "
            f"-ptime {ptime_str} "
            f"-init_retry 20 "
            f"-f mtl_st30p -i k "
            f"-f {ff_fmt} {{out0}} -y"
        )
        return rx_cmd, None

    # --------------------------------------------------- prepare_execution
    def prepare_execution(self, build: str, host=None, **kwargs):
        """Create per-host artifacts (output files, RxTxApp configs).

        Resolves placeholders in ``self.command`` / ``self._tx_commands``
        (e.g. ``{out0}``, ``{rx_cfg}``) using the live host connection.
        """
        if not host:
            raise ValueError("host required for FFmpeg execution")
        self._build = build
        # Resolve mode the same way as _create_command_and_config.
        mode = self._ff_params.get("mode")
        if mode is None:
            session_type = self.params.get("session_type", "st20p")
            mode = _SESSION_TYPE_TO_MODE.get(session_type, _MODE_YUV_H264)

        if mode == _MODE_YUV_H264:
            output_format = self._ff_params.get("output_format", "yuv")
            multiple = bool(self._ff_params.get("multiple_sessions", False))
            n = 2 if multiple else 1
            # ``output_file`` lets a test place the RX capture on a ramdisk.
            # The default lands in {build}/tests/ on the root ext4 volume, and
            # 1080p25 4:2:2 10-bit needs ~130 MB/s sustained; when the disk
            # cannot keep up the RX FFmpeg stalls, back-pressures MTL's frame
            # pool and starves the co-resident TX producer, which shows up as
            # ST 2110-21 pacing failures that have nothing to do with MTL.
            out_path_param = self.params.get("output_file")
            if out_path_param and not multiple:
                self._output_files = [out_path_param]
                host.connection.path(out_path_param).touch()
            else:
                self._output_files = ffmpeg_app.create_empty_output_files(
                    output_format, n, host, build
                )
            self.command = self.command.replace("{out0}", self._output_files[0])
            if multiple:
                self.command = self.command.replace("{out1}", self._output_files[1])

            # When TX is RxTxApp, the per-test config file must be generated
            # now (helper needs host + build).
            tx_is_ffmpeg = bool(self._ff_params.get("tx_is_ffmpeg", True))
            if not tx_is_ffmpeg:
                nic_port_list = self.params["nic_port_list"]
                # TX takes port[0]; the FFmpeg RX built above already owns
                # port[1]. Pointing both at port[1] made RX lose the EAL probe
                # race and produce a 0-byte output.
                tx_cfg = ffmpeg_app.generate_rxtxapp_tx_config(
                    nic_port_list[0],
                    self.params["video_format"],
                    self.params["video_url"],
                    host,
                    build,
                    multiple,
                )
                self._tx_commands = [f"{RXTXAPP_EXE} --config_file {tx_cfg}"]

        elif mode == _MODE_RGB24:
            nic_port_list = self.params["nic_port_list"]
            rx_cfg = ffmpeg_app.generate_rxtxapp_rx_config(
                nic_port_list[0], self.params["video_format"], host, build
            )
            test_time = self.params.get("test_time") or 30
            self.command = self.command.replace("{rx_cfg}", rx_cfg).replace(
                "{test_time}", str(test_time)
            )

        elif mode == _MODE_RGB24_MULTI:
            nic_port_list = self.params["nic_port_list"]
            rx_cfg = ffmpeg_app.generate_rxtxapp_rx_config_multiple(
                nic_port_list[:2],
                self._ff_params["video_format_list"],
                host,
                build,
                True,
            )
            test_time = self.params.get("test_time") or 30
            self.command = self.command.replace("{rx_cfg}", rx_cfg).replace(
                "{test_time}", str(test_time)
            )

        elif mode in (_MODE_ST22P, _MODE_ST30P):
            # Use output_file from params if provided, else create a temp file.
            out_path_param = self.params.get("output_file")
            if out_path_param:
                self._output_files = [out_path_param]
                # Touch the file so FFmpeg can write to it.
                host.connection.path(out_path_param).touch()
            else:
                ext = "yuv" if mode == _MODE_ST22P else "raw"
                out_path = ffmpeg_app.create_empty_output_files(ext, 1, host, build)
                self._output_files = out_path
            self.command = self.command.replace("{out0}", self._output_files[0])

    # ----------------------------------------------------- execute_test
    def execute_test(  # type: ignore[override]
        self,
        build: str,
        test_time: int = 30,
        host=None,
        sleep_interval: int = 5,
        compliance=NO_COMPLIANCE,
        integrity=NO_INTEGRITY,
        interface_setup=None,
        fail_on_error: bool = True,
        **extra,
    ) -> bool:
        """Single-host RX-then-TX orchestrator.

        FFmpeg tests always run RX and TX on the same host (RX first, due to
        DPDK init latency). All processes are unbounded (``-stream_loop -1``
        on the FFmpeg side; RxTxApp TX has no ``--test_time``), so the
        wall-clock ``test_time`` drives the duration and the base helper
        applies the SIGINT \u2192 SIGKILL stop ladder.

        Dual-host orchestration (``tx_host`` / ``rx_host`` / ``rx_app``) is
        not supported here \u2014 FFmpeg + mtl_st20p loops back through one
        DPDK process group on a single host. Passing those keys is a
        caller bug; fail loudly rather than silently degrade.
        """
        unsupported = sorted(
            k
            for k in ("tx_host", "rx_host", "rx_app", "tx_first")
            if extra.get(k) is not None
        )
        if unsupported:
            raise ValueError(
                f"FFmpeg adapter does not support {unsupported}; "
                f"use a single ``host=`` argument."
            )
        if not host:
            raise ValueError("host required for single-host execution")
        if not self.command:
            raise RuntimeError("create_command() must be called before execute_test()")

        # Stash test_time so prepare_execution / validation can see it.
        self.params["test_time"] = test_time
        self.prepare_execution(build=build, host=host, interface_setup=interface_setup)

        if not self._tx_commands:
            raise RuntimeError("FFmpeg TX command(s) missing after prepare_execution")

        specs = [ProcSpec(cmd=self.command, host=host, label="RX", bounded=False)]
        for idx, tx_cmd in enumerate(self._tx_commands, start=1):
            specs.append(
                ProcSpec(cmd=tx_cmd, host=host, label=f"TX{idx}", bounded=False)
            )

        # FFmpeg starts RX first and TX second; traffic (and thus the pcap
        # capture) only flows once TX is up, unlike the base class default
        # where the single process IS the traffic source -- so this arms
        # after_last_start instead of after_first_start. Built once and
        # reused for _finalize_run()'s evaluate() call so both see the exact
        # same snapshot of self.params.
        intent = self.capture_intent()
        self._run_proc_group(
            specs,
            build=build,
            test_time=test_time,
            sleep_interval=sleep_interval,
            wall_clock_seconds=test_time,
            cleanup_host=host,
            after_last_start=lambda _proc: compliance.arm(intent),
        )
        # RX is always specs[0] (started first); validation reads its output.
        self._rx_output = specs[0].captured_output
        self.last_output = self._rx_output
        self.last_return_code = self._safe_return_code(specs[0].proc)

        integrity_intent = self.integrity_intent(build, host)

        return self._finalize_run(
            compliance,
            intent,
            fail_on_error,
            integrity=integrity,
            integrity_intent=integrity_intent,
        )

    # ----------------------------------------------------- compliance
    def _resolve_capture_dst_ips(self) -> tuple[str, ...]:
        """Destination IPs netsniff filters on.

        Every FFmpeg command built here transmits to ``ip_pools.rx_multicast[0]``
        (see the ``-p_rx_ip`` tokens in the RX/TX builders), so the capture
        filter target is fixed rather than read back from a config file.
        """
        return (ip_pools.rx_multicast[0],)

    # ----------------------------------------------------- validate
    def validate_results(self, fail_on_error: bool = True) -> bool:  # type: ignore[override]
        mode = self._ff_params.get("mode")
        if mode is None:
            session_type = self.params.get("session_type", "st20p")
            mode = _SESSION_TYPE_TO_MODE.get(session_type, _MODE_YUV_H264)
        host = self._host
        build = self._build
        try:
            if mode == _MODE_YUV_H264:
                output_format = self._ff_params.get("output_format", "yuv")
                video_format = self.params["video_format"]
                video_size, _ = ffmpeg_app.decode_video_format_16_9(video_format)
                video_url = self.params["video_url"]
                if output_format == "yuv":
                    passed = ffmpeg_app.check_output_video_yuv(
                        self._output_files[0], host, build, video_url
                    )
                else:
                    passed = ffmpeg_app.check_output_video_h264(
                        self._output_files[0], video_size, host, build, video_url
                    )
                # Best-effort cleanup of generated output files (unless caller
                # asked to keep them via ``keep_output=True`` for a follow-up
                # integrity check).
                self._cleanup_output_files(host)
            elif mode == _MODE_RGB24:
                passed = ffmpeg_app.check_output_rgb24(self._rx_output or "", 1)
            elif mode == _MODE_RGB24_MULTI:
                passed = ffmpeg_app.check_output_rgb24(self._rx_output or "", 2)
            elif mode in (_MODE_ST22P, _MODE_ST30P):
                # Basic validation: output file exists and is non-empty.
                out_file = self._output_files[0] if self._output_files else None
                if not out_file:
                    self._fail_validation(
                        f"{mode}: no output file recorded", fail_on_error
                    )
                    return False
                result = host.connection.execute_command(
                    f"stat -c %s {out_file}", expected_return_codes=None
                )
                file_size = int(result.stdout.strip()) if result.return_code == 0 else 0
                passed = file_size > 0
                if not passed:
                    logger.error(f"{mode}: output file is empty: {out_file}")
                # An integrity session, when the test asked for one, has already
                # read this file: _finalize_run() evaluates it before
                # validate_results() runs. Nothing downstream needs it, and a
                # recording of a full test's traffic is far too large to leave
                # in the workspace -- 1080p yuv422p10le is about 250 MB/s.
                self._cleanup_output_files(host)
            else:
                self._fail_validation(f"Unknown mode {mode}", fail_on_error)
                return False
        except AssertionError:
            raise
        except Exception as e:
            self._fail_validation(f"FFmpeg validation error: {e}", fail_on_error)
            return False

        if not passed:
            self._fail_validation("FFmpeg test failed", fail_on_error)
            return False
        return True
