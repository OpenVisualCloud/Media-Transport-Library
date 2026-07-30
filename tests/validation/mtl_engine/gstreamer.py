# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation
"""GStreamer framework adapter (unified Application model).

Wraps the procedural builders and orchestrator in :mod:`mtl_engine.GstreamerApp`,
which stay the single source of truth for pipeline token lists and pass/fail
criteria. ``create_command`` materialises the TX/RX gst-launch token lists from
the same UNIVERSAL_PARAMS vocabulary RxTxApp/FFmpeg use (translating to
GStreamer caps internally); ``execute_test`` delegates the TX+RX run to
:func:`GstreamerApp.execute_test`.

Execution is delegated because the procedural orchestrator runs a TX+RX pipeline
pair on a single host and owns md5 output comparison, which the base single-host
run loop (one ``self.command``) does not model. The adapter still honors the
shared ``netsniff=`` capture hook so one parametrized test can drive RxTxApp,
FFmpeg and GStreamer identically. The base PTP extension and SIGINT->SIGKILL
stop ladder are not reused: GstreamerApp owns process teardown.

``app_path`` is the MTL build directory: ``gst-launch-1.0`` is resolved from
PATH and the builders hardcode it, so ``app_path`` is used only as the default
``build`` dir (``--gst-plugin-path``) when a test does not pass ``build=``
explicitly.
"""

from __future__ import annotations

from mtl_engine import GstreamerApp, ip_pools
from mtl_engine.application_base import Application
from mtl_engine.config.mappings import APP_NAME_MAP

_ST40P_REDUNDANT_UDP_PORT = 40001

# UNIVERSAL_PARAMS -> GStreamer caps translation. These let a GStreamer adapter
# be driven by the same ``create_command(**universal_params)`` call that
# RxTxApp/FFmpeg use, so one parametrized test can target all three frameworks.
_AUDIO_FORMAT_TO_GST = {"PCM8": "S8", "PCM16": "S16BE", "PCM24": "S24BE"}
_AUDIO_SAMPLING_TO_HZ = {"44.1kHz": 44100, "48kHz": 48000, "96kHz": 96000}
# Channel-layout label -> count. Mirrors the RxTxApp channel vocabulary; ``U0N``
# is "N user channels".
_AUDIO_CHANNEL_LABEL_TO_COUNT = {
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


class GStreamer(Application):
    """GStreamer framework adapter (single-host TX+RX orchestrator)."""

    # Parameters the GStreamer pipelines understand but which are not part of
    # UNIVERSAL_PARAMS. Stripped in ``set_params`` into ``self._gst`` so the
    # base-class validation does not reject them. ``audio_format`` is also a
    # universal key but carries a GStreamer caps value here, so it is routed
    # into ``self._gst`` rather than ``self.params``.
    _GSTREAMER_ONLY_KEYS = (
        "build",
        "gst_format",
        "tx_queues",
        "rx_queues",
        "tx_payload_type",
        "rx_payload_type",
        "audio_format",
        "audio_channels",
        "audio_rate",
        "redundant",
        "frame_info_path",
        "capture_metadata",
        "skip_file_compare",
        "rx_timeout",
        "tx_framebuff_cnt",
        "rx_framebuff_cnt",
        "tx_fps",
        "tx_did",
        "tx_sdid",
        "tx_rfc8331",
        "tx_user_pacing",
        "tx_user_controlled_pacing",
        "tx_user_controlled_pacing_offset",
        "tx_interlaced",
        "rx_interlaced",
        "tx_split_anc_by_pkt",
        "tx_test_mode",
        "tx_test_pkt_count",
        "tx_test_pacing_ns",
        "rx_disable_auto_detect",
        "rx_rtp_ring_size",
    )

    def __init__(self, app_path, config_file_path=None):
        super().__init__(app_path, config_file_path)
        self._gst: dict = {}
        self._tx_command: list[str] = []
        self._rx_command: list[str] = []
        self._input_file = None
        self._output_file = None
        self._build = None
        self._last_passed = False

    def get_app_name(self) -> str:
        return "GStreamer"

    def get_executable_name(self) -> str:
        return APP_NAME_MAP["gstreamer"]

    def set_params(self, **kwargs):
        """Strip GStreamer-only kwargs into ``self._gst`` first, then defer."""
        self._gst = {}
        for key in self._GSTREAMER_ONLY_KEYS:
            if key in kwargs:
                self._gst[key] = kwargs.pop(key)
        super().set_params(**kwargs)

    def _create_command_and_config(self) -> tuple:
        # ``build`` may be supplied explicitly (legacy GStreamer-only tests) or
        # derived from ``app_path`` (the MTL build dir that ``app_factory``
        # passes), so the same call used by RxTxApp/FFmpeg works unchanged.
        self._build = self._gst.get("build") or self.app_path
        if not self._build:
            raise ValueError("GStreamer requires a build dir (pass build= or app_path)")

        nic_port_list = self.params["nic_port_list"]
        if not nic_port_list:
            raise ValueError("nic_port_list is required")

        session_type = self.params["session_type"]
        if session_type == "st20p":
            self._build_st20p(nic_port_list)
        elif session_type in ("st30", "st30p"):
            self._build_st30(nic_port_list)
        elif session_type == "st40p":
            self._build_st40p(nic_port_list)
        else:
            raise ValueError(f"Unsupported GStreamer session_type: {session_type}")

        self._input_file = self.params["input_file"]
        self._output_file = self.params["output_file"]
        return " ".join(self._rx_command), None

    # ------------------------------------------------ param translation
    def _resolve_gst_video_format(self) -> str:
        """GStreamer caps name from ``gst_format`` or universal transport format."""
        fmt = self._gst.get("gst_format")
        if fmt:
            return fmt
        src = self.params.get("transport_format") or self.params.get("pixel_format")
        return GstreamerApp.video_format_change(src)

    def _resolve_framerate(self) -> str:
        """Numeric framerate string (``"p25"`` -> ``"25"``) for the gst builders."""
        return str(self.extract_framerate(self.params["framerate"]))

    def _resolve_audio_format(self) -> str:
        """GStreamer audio caps (``S8``/``S16BE``/``S24BE``) from any vocabulary."""
        fmt = self._gst.get("audio_format") or self.params.get("audio_format")
        return _AUDIO_FORMAT_TO_GST.get(fmt, fmt)

    def _resolve_audio_channels(self) -> int:
        ch = self._gst.get("audio_channels", self.params.get("audio_channels"))
        if isinstance(ch, (list, tuple)):
            ch = ch[0] if ch else "U02"
        if isinstance(ch, int):
            return ch
        if ch in _AUDIO_CHANNEL_LABEL_TO_COUNT:
            return _AUDIO_CHANNEL_LABEL_TO_COUNT[ch]
        return int(ch)

    def _resolve_audio_rate(self) -> int:
        rate = self._gst.get("audio_rate")
        if rate is not None:
            return int(rate)
        samp = self.params.get("audio_sampling")
        if isinstance(samp, (int, float)):
            return int(samp)
        return _AUDIO_SAMPLING_TO_HZ.get(samp, 48000)

    def _build_st20p(self, nic_port_list):
        gst_format = self._resolve_gst_video_format()
        framerate = self._resolve_framerate()
        enable_ptp = bool(self.params.get("enable_ptp", False))
        self._tx_command = GstreamerApp.setup_gstreamer_st20p_tx_pipeline(
            build=self._build,
            nic_port_list=nic_port_list[0],
            input_path=self.params["input_file"],
            width=self.params["width"],
            height=self.params["height"],
            framerate=framerate,
            format=gst_format,
            tx_payload_type=self._gst.get("tx_payload_type", 112),
            tx_queues=self._gst.get("tx_queues", 4),
            enable_ptp=enable_ptp,
        )
        self._rx_command = GstreamerApp.setup_gstreamer_st20p_rx_pipeline(
            build=self._build,
            nic_port_list=nic_port_list[1],
            output_path=self.params["output_file"],
            width=self.params["width"],
            height=self.params["height"],
            framerate=framerate,
            format=gst_format,
            rx_payload_type=self._gst.get("rx_payload_type", 112),
            rx_queues=self._gst.get("rx_queues", 4),
            enable_ptp=enable_ptp,
        )

    def _build_st30(self, nic_port_list):
        audio_format = self._resolve_audio_format()
        channels = self._resolve_audio_channels()
        sampling = self._resolve_audio_rate()
        self._tx_command = GstreamerApp.setup_gstreamer_st30_tx_pipeline(
            build=self._build,
            nic_port_list=nic_port_list[0],
            input_path=self.params["input_file"],
            tx_payload_type=self._gst.get("tx_payload_type", 111),
            tx_queues=self._gst.get("tx_queues", 4),
            audio_format=audio_format,
            channels=channels,
            sampling=sampling,
        )
        self._rx_command = GstreamerApp.setup_gstreamer_st30_rx_pipeline(
            build=self._build,
            nic_port_list=nic_port_list[1],
            output_path=self.params["output_file"],
            rx_payload_type=self._gst.get("rx_payload_type", 111),
            rx_queues=self._gst.get("rx_queues", 4),
            rx_audio_format=GstreamerApp.audio_format_change(
                audio_format, rx_side=True
            ),
            rx_channels=channels,
            rx_sampling=sampling,
        )

    def _build_st40p(self, nic_port_list):
        tx_payload = self._gst.get("tx_payload_type", 113)
        rx_payload = self._gst.get("rx_payload_type", 113)
        queues = self._gst.get("tx_queues", 4)
        rx_queues = self._gst.get("rx_queues", 4)
        timeout = self._gst.get("rx_timeout", 15)

        tx_red = {}
        rx_red = {}
        rx_primary = nic_port_list[1]
        if self._gst.get("redundant"):
            if len(nic_port_list) < 4:
                raise ValueError("st40p redundant requires at least 4 NIC ports")
            if len(ip_pools.tx) < 2 or len(ip_pools.rx) < 2:
                raise ValueError("st40p redundant requires at least 2 TX and 2 RX IPs")
            rx_primary = nic_port_list[2]
            tx_red = dict(
                dev_port_red=nic_port_list[1],
                dev_ip_red=ip_pools.rx[1],
                ip_red=ip_pools.tx[1],
                udp_port_red=_ST40P_REDUNDANT_UDP_PORT,
            )
            rx_red = dict(
                dev_port_red=nic_port_list[3],
                dev_ip_red=ip_pools.tx[1],
                ip_red=ip_pools.rx[1],
                udp_port_red=_ST40P_REDUNDANT_UDP_PORT,
            )

        self._tx_command = GstreamerApp.setup_gstreamer_st40p_tx_pipeline(
            build=self._build,
            nic_port_list=nic_port_list[0],
            input_path=self.params["input_file"],
            tx_payload_type=tx_payload,
            tx_queues=queues,
            tx_framebuff_cnt=self._gst.get("tx_framebuff_cnt"),
            tx_fps=self._gst.get("tx_fps"),
            tx_did=self._gst.get("tx_did"),
            tx_sdid=self._gst.get("tx_sdid"),
            tx_rfc8331=self._gst.get("tx_rfc8331", False),
            tx_user_pacing=self._gst.get("tx_user_pacing", False),
            tx_user_controlled_pacing=self._gst.get("tx_user_controlled_pacing", False),
            tx_user_controlled_pacing_offset=self._gst.get(
                "tx_user_controlled_pacing_offset", 0
            ),
            tx_interlaced=self._gst.get("tx_interlaced", False),
            tx_split_anc_by_pkt=self._gst.get("tx_split_anc_by_pkt", False),
            tx_test_mode=self._gst.get("tx_test_mode"),
            tx_test_pkt_count=self._gst.get("tx_test_pkt_count", 0),
            tx_test_pacing_ns=self._gst.get("tx_test_pacing_ns", 0),
            **tx_red,
        )
        self._rx_command = GstreamerApp.setup_gstreamer_st40p_rx_pipeline(
            build=self._build,
            nic_port_list=rx_primary,
            output_path=self.params["output_file"],
            rx_payload_type=rx_payload,
            rx_queues=rx_queues,
            timeout=timeout,
            capture_metadata=self._gst.get("capture_metadata", False),
            rx_interlaced=self._gst.get("rx_interlaced", False),
            rx_disable_auto_detect=self._gst.get("rx_disable_auto_detect", False),
            rx_framebuff_cnt=self._gst.get("rx_framebuff_cnt"),
            frame_info_path=self._gst.get("frame_info_path"),
            rx_rtp_ring_size=self._gst.get("rx_rtp_ring_size"),
            **rx_red,
        )

    def execute_test(  # type: ignore[override]
        self,
        build: str,
        test_time: int = 30,
        host=None,
        sleep_interval: int = 4,
        tx_first: bool = False,
        fail_on_error: bool = True,
        netsniff=None,
        interface_setup=None,
        skip_file_compare=None,
        log_frame_info: bool = True,
        suppress_fail_logs: bool = False,
        **extra,
    ) -> bool:
        """Delegate execution to :func:`GstreamerApp.execute_test`.

        The procedural orchestrator owns process lifetime and md5 output
        comparison, so this does not reuse the base single-host run loop (which
        drives only one ``self.command``; GStreamer needs a TX+RX pair on one
        host). It still honors the common ``netsniff=`` capture hook so a single
        parametrized test can drive RxTxApp, FFmpeg and GStreamer identically.
        ``suppress_fail_logs`` is forwarded so negative/rejection tests can
        silence the failure log dump they expect.
        """
        if not (self._tx_command and self._rx_command):
            raise RuntimeError("create_command() must be called first")

        if skip_file_compare is None:
            skip_file_compare = self._gst.get("skip_file_compare", False)

        # Arm a bounded packet capture that overlaps the stream window. The
        # capture runs in the background for ``test_time`` while GstreamerApp
        # drives the TX/RX pipelines synchronously. With PTP enabled the plugin
        # blocks streaming until the clock locks, so the capture window is
        # extended by the PTP sync budget to overlap the steady-state stream.
        if netsniff is not None:
            ptp_budget = (
                self.params.get("ptp_sync_time", 50)
                if self.params.get("enable_ptp", False)
                else 0
            )
            self.params["test_time"] = test_time + ptp_budget
            self._start_netsniff_capture(netsniff)

        passed = GstreamerApp.execute_test(
            build=build,
            tx_command=self._tx_command,
            rx_command=self._rx_command,
            input_file=self._input_file,
            output_file=self._output_file,
            test_time=test_time,
            host=host,
            sleep_interval=sleep_interval,
            tx_first=tx_first,
            suppress_fail_logs=suppress_fail_logs,
            skip_file_compare=skip_file_compare,
            log_frame_info=log_frame_info,
        )
        self._last_passed = bool(passed)
        if not passed:
            if fail_on_error:
                self._fail_validation("GStreamer test failed", fail_on_error)
            return False
        return passed

    def validate_results(self, fail_on_error: bool = True) -> bool:  # type: ignore[override]
        return self._last_passed

    def _resolve_capture_dst_ip(self):  # type: ignore[override]
        """Destination IP the GStreamer TX pipeline streams to (for netsniff)."""
        return ip_pools.rx[0] if ip_pools.rx else None
