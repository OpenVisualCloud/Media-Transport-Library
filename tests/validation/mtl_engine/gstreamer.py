# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation
"""GStreamer framework adapter (unified Application model).

Wraps the procedural builders and orchestrator in :mod:`mtl_engine.GstreamerApp`,
which stay the single source of truth for pipeline token lists and pass/fail
criteria. ``create_command`` materialises the TX/RX gst-launch token lists;
``execute_test`` delegates to :func:`GstreamerApp.execute_test`.

Because execution is delegated, this adapter intentionally BYPASSES the
base-class run machinery (netsniff hook, PTP extension, SIGINT->SIGKILL stop
ladder, ``_output_files`` cleanup, ``interface_setup``). No GStreamer test uses
those features; the procedural orchestrator owns process lifetime and file
comparison.

``app_path`` is vestigial for GStreamer: ``gst-launch-1.0`` is resolved from
PATH and the builders hardcode it, so callers pass the MTL build directory as
``app_path`` purely to satisfy the base constructor.
"""

from __future__ import annotations

from mtl_engine import GstreamerApp, ip_pools
from mtl_engine.application_base import Application
from mtl_engine.config.mappings import APP_NAME_MAP

_ST40P_REDUNDANT_UDP_PORT = 40001


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
        if "build" not in self._gst:
            raise ValueError("GStreamer create_command requires a 'build' kwarg")
        self._build = self._gst["build"]

        nic_port_list = self.params["nic_port_list"]
        if not nic_port_list:
            raise ValueError("nic_port_list is required")

        session_type = self.params["session_type"]
        if session_type == "st20p":
            self._build_st20p(nic_port_list)
        elif session_type == "st30":
            self._build_st30(nic_port_list)
        elif session_type == "st40p":
            self._build_st40p(nic_port_list)
        else:
            raise ValueError(f"Unsupported GStreamer session_type: {session_type}")

        self._input_file = self.params["input_file"]
        self._output_file = self.params["output_file"]
        return " ".join(self._rx_command), None

    def _build_st20p(self, nic_port_list):
        gst_format = self._gst["gst_format"]
        self._tx_command = GstreamerApp.setup_gstreamer_st20p_tx_pipeline(
            build=self._build,
            nic_port_list=nic_port_list[0],
            input_path=self.params["input_file"],
            width=self.params["width"],
            height=self.params["height"],
            framerate=self.params["framerate"],
            format=gst_format,
            tx_payload_type=self._gst.get("tx_payload_type", 112),
            tx_queues=self._gst.get("tx_queues", 4),
        )
        self._rx_command = GstreamerApp.setup_gstreamer_st20p_rx_pipeline(
            build=self._build,
            nic_port_list=nic_port_list[1],
            output_path=self.params["output_file"],
            width=self.params["width"],
            height=self.params["height"],
            framerate=self.params["framerate"],
            format=gst_format,
            rx_payload_type=self._gst.get("rx_payload_type", 112),
            rx_queues=self._gst.get("rx_queues", 4),
        )

    def _build_st30(self, nic_port_list):
        audio_format = self._gst["audio_format"]
        channels = self._gst["audio_channels"]
        sampling = self._gst["audio_rate"]
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
        skip_file_compare=None,
        log_frame_info: bool = True,
        suppress_fail_logs: bool = False,
        **extra,
    ) -> bool:
        """Delegate execution to :func:`GstreamerApp.execute_test`.

        See the module docstring: this bypasses the base-class run machinery
        because the procedural orchestrator owns process lifetime and output
        comparison. ``suppress_fail_logs`` is forwarded so negative/rejection
        tests can silence the failure log dump they expect.
        """
        if not (self._tx_command and self._rx_command):
            raise RuntimeError("create_command() must be called first")

        if skip_file_compare is None:
            skip_file_compare = self._gst.get("skip_file_compare", False)

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
