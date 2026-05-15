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
import signal
import time

from mtl_engine import ffmpeg_app, ip_pools
from mtl_engine.application_base import Application
from mtl_engine.config.mappings import APP_NAME_MAP
from mtl_engine.const import FFMPEG_EXE, RXTXAPP_EXE
from mtl_engine.execute import kill_stale_processes, run

logger = logging.getLogger(__name__)


# Modes selected by tests — each maps onto one of the legacy execute_test_*
# command-generation paths.
_MODE_YUV_H264 = "yuv_h264"  # FFmpeg RX + (FFmpeg or RxTxApp) TX, yuv|h264 output
_MODE_RGB24 = "rgb24"  # FFmpeg TX (rgb24) + RxTxApp RX, single stream
_MODE_RGB24_MULTI = "rgb24_multiple"  # FFmpeg TX×2 (rgb24) + RxTxApp RX, two streams


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
        "mode",  # "yuv_h264" | "rgb24" | "rgb24_multiple"
        "video_format_list",  # list[str]  — rgb24_multiple only
        "video_url_list",  # list[str]   — rgb24_multiple only
        "pix_fmt",  # str — yuv_h264 only; default "yuv422p10le"
        "keep_output",  # bool — skip RX output cleanup (integrity follow-up)
    )

    # Per-process stop ladder timing (seconds): wait this long for SIGINT
    # before escalating to SIGKILL. Matches the legacy ffmpeg_app value.
    _STOP_GRACE_S = 5

    def __init__(self, app_path, config_file_path=None):
        super().__init__(app_path, config_file_path)
        self._ff_params: dict = {}
        self._tx_commands: list[str] = []
        self._build: str | None = None
        self._output_files: list[str] = []
        self._rx_output: str | None = None

    @property
    def output_files(self) -> list[str]:
        """RX-side output file paths produced by the most recent run.

        Tests that pass ``keep_output=True`` can read the path(s) here to
        feed a follow-up integrity check before cleanup.
        """
        return list(self._output_files)

    # ------------------------------------------------------------------ ABCs
    def get_app_name(self) -> str:
        return "FFmpeg"

    def get_executable_name(self) -> str:
        return APP_NAME_MAP["ffmpeg"]

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
    def _create_command_and_config(self) -> tuple:
        """Build RX + TX command strings for the requested mode.

        Returns ``(rx_cmd, None)``. TX commands live in ``self._tx_commands``.
        Output files (when produced by RX) live in ``self._output_files`` —
        populated lazily in :meth:`prepare_execution` because file creation
        needs the live ``host`` connection.
        """
        mode = self._ff_params.get("mode", _MODE_YUV_H264)
        nic_port_list = self.params["nic_port_list"]
        if not nic_port_list:
            raise ValueError("nic_port_list is required")

        if mode == _MODE_YUV_H264:
            return self._build_yuv_h264_cmds(nic_port_list)
        if mode == _MODE_RGB24:
            return self._build_rgb24_cmds(nic_port_list)
        if mode == _MODE_RGB24_MULTI:
            return self._build_rgb24_multiple_cmds(nic_port_list)
        raise ValueError(f"Unknown FFmpeg mode: {mode}")

    # -- mode: yuv|h264 (FFmpeg RX, FFmpeg-or-RxTxApp TX) ----------------
    def _build_yuv_h264_cmds(self, nic_port_list):
        video_format = self.params["video_format"]
        video_url = self.params["video_url"]
        output_format = self._ff_params.get("output_format", "yuv")
        multiple = bool(self._ff_params.get("multiple_sessions", False))
        tx_is_ffmpeg = bool(self._ff_params.get("tx_is_ffmpeg", True))
        pix_fmt = self._ff_params.get("pix_fmt", "yuv422p10le")

        video_size, fps = ffmpeg_app.decode_video_format_16_9(video_format)
        rx_f_flag = "-f rawvideo" if output_format == "yuv" else "-c:v libopenh264"
        # When caller pre-converted the source to a non-default pix_fmt (typical
        # for integrity tests), drop the fps filter so frames stay byte-identical.
        tx_filter = "" if pix_fmt != "yuv422p10le" else f"-filter:v fps={fps} "

        if not multiple:
            rx_cmd = (
                f"{FFMPEG_EXE} -p_port {nic_port_list[0]} "
                f"-p_sip {ip_pools.rx[0]} "
                f"-p_rx_ip {ip_pools.rx_multicast[0]} -udp_port 20000 "
                f"-payload_type 112 -fps {fps} -pix_fmt {pix_fmt} "
                f"-video_size {video_size} -f mtl_st20p -i k "
                f"-init_retry 20 "
                f"{rx_f_flag} {{out0}} -y"
            )
        else:
            rx_cmd = (
                f"{FFMPEG_EXE} -p_sip {ip_pools.rx[0]} "
                f"-p_port {nic_port_list[0]} "
                f"-p_rx_ip {ip_pools.rx_multicast[0]} -udp_port 20000 "
                f"-payload_type 112 -fps {fps} -pix_fmt {pix_fmt} "
                f"-video_size {video_size} -f mtl_st20p -i 1 "
                f"-p_port {nic_port_list[0]} "
                f"-p_rx_ip {ip_pools.rx_multicast[0]} -udp_port 20002 "
                f"-payload_type 112 -fps {fps} -pix_fmt {pix_fmt} "
                f"-video_size {video_size} -f mtl_st20p -i 2 "
                f"-map 0:0 {rx_f_flag} {{out0}} -y "
                f"-map 1:0 {rx_f_flag} {{out1}} -y"
            )

        # TX command (single — both single- and multi-session use one TX).
        # ``-stream_loop -1`` matches the legacy ``ffmpeg_app.execute_test``
        # behaviour: TX must keep streaming for the entire ``test_time`` even
        # when the source YUV file is shorter than the test duration.
        if tx_is_ffmpeg:
            tx_cmd = (
                f"{FFMPEG_EXE} -stream_loop -1 -video_size {video_size} "
                f"-f rawvideo -pix_fmt {pix_fmt} -i {video_url} "
                f"{tx_filter}-p_port {nic_port_list[1]} "
                f"-p_sip {ip_pools.tx[0]} "
                f"-p_tx_ip {ip_pools.rx_multicast[0]} -udp_port 20000 "
                f"-payload_type 112 -f mtl_st20p -"
            )
            self._tx_commands = [tx_cmd]
        else:
            # RxTxApp TX — config file generated lazily in prepare_execution()
            # because ffmpeg_app.generate_rxtxapp_tx_config needs the host.
            self._tx_commands = []  # filled in by prepare_execution
        return rx_cmd, None

    # -- mode: rgb24 (FFmpeg TX, RxTxApp RX, single stream) --------------
    def _build_rgb24_cmds(self, nic_port_list):
        video_format = self.params["video_format"]
        video_url = self.params["video_url"]
        video_size, fps = ffmpeg_app.decode_video_format_16_9(video_format)

        # RX (RxTxApp) cmd has its config_file path filled in prepare_execution.
        rx_cmd = f"{RXTXAPP_EXE} --config_file {{rx_cfg}} --test_time {{test_time}}"
        tx_cmd = (
            f"{FFMPEG_EXE} -stream_loop -1 -framerate {fps} -video_size "
            f"{video_size} -f rawvideo -pix_fmt yuv422p10be "
            f"-i {video_url} -filter:v format=rgb24 "
            f"-p_port {nic_port_list[1]} "
            f"-p_sip {ip_pools.tx[0]} -p_tx_ip {ip_pools.rx_multicast[0]} "
            f"-udp_port 20000 -payload_type 112 -f mtl_st20p -"
        )
        self._tx_commands = [tx_cmd]
        return rx_cmd, None

    # -- mode: rgb24_multiple (FFmpeg TX×2, RxTxApp RX, two streams) -----
    def _build_rgb24_multiple_cmds(self, nic_port_list):
        video_format_list = self._ff_params["video_format_list"]
        video_url_list = self._ff_params["video_url_list"]
        if len(nic_port_list) < 4:
            raise ValueError(
                "rgb24_multiple requires 4 NIC ports (2 RX + 2 TX), "
                f"got {len(nic_port_list)}"
            )
        v1, f1 = ffmpeg_app.decode_video_format_16_9(video_format_list[0])
        v2, f2 = ffmpeg_app.decode_video_format_16_9(video_format_list[1])
        rx_cmd = f"{RXTXAPP_EXE} --config_file {{rx_cfg}} --test_time {{test_time}}"
        tx1 = (
            f"{FFMPEG_EXE} -stream_loop -1 -framerate {f1} -video_size {v1} "
            f"-f rawvideo -pix_fmt yuv422p10be -i {video_url_list[0]} "
            f"-filter:v format=rgb24 -p_port {nic_port_list[2]} "
            f"-p_sip {ip_pools.tx[0]} -p_tx_ip {ip_pools.rx_multicast[0]} "
            f"-udp_port 20000 -payload_type 112 -f mtl_st20p -"
        )
        tx2 = (
            f"{FFMPEG_EXE} -stream_loop -1 -framerate {f2} -video_size {v2} "
            f"-f rawvideo -pix_fmt yuv422p10be -i {video_url_list[1]} "
            f"-filter:v format=rgb24 -p_port {nic_port_list[3]} "
            f"-p_sip {ip_pools.tx[1]} -p_tx_ip {ip_pools.rx_multicast[1]} "
            f"-udp_port 20000 -payload_type 112 -f mtl_st20p -"
        )
        self._tx_commands = [tx1, tx2]
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
        mode = self._ff_params.get("mode", _MODE_YUV_H264)

        if mode == _MODE_YUV_H264:
            output_format = self._ff_params.get("output_format", "yuv")
            multiple = bool(self._ff_params.get("multiple_sessions", False))
            n = 2 if multiple else 1
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
                tx_cfg = ffmpeg_app.generate_rxtxapp_tx_config(
                    nic_port_list[1],
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
            self._output_files = []

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
            self._output_files = []

    # ----------------------------------------------------- execute_test
    def execute_test(  # type: ignore[override]
        self,
        build: str,
        test_time: int = 30,
        host=None,
        sleep_interval: int = 5,
        interface_setup=None,
        fail_on_error: bool = True,
        **_unused,
    ) -> bool:
        """Single-host RX-then-TX orchestrator.

        FFmpeg tests always run RX and TX on the same host (RX first, due to
        DPDK init latency). Extra base-class kwargs (``tx_host`` / ``rx_app``
        / ``netsniff`` / ``tx_first``) are intentionally absorbed by
        ``**_unused`` and ignored \u2014 dual-host orchestration is not
        applicable to the FFmpeg loopback adapter.
        """
        if not host:
            raise ValueError("host required for single-host execution")
        if not self.command:
            raise RuntimeError("create_command() must be called before execute_test()")

        # Stash test_time so prepare_execution / validation can see it.
        self.params["test_time"] = test_time
        self.prepare_execution(build=build, host=host, interface_setup=interface_setup)

        if not self._tx_commands:
            raise RuntimeError("FFmpeg TX command(s) missing after prepare_execution")

        timeout = test_time + self.params.get("process_timeout_buffer", 90)
        rx_proc = None
        tx_procs: list = []
        try:
            logger.info("[FFmpeg] Starting RX: %s", self.command)
            rx_proc = run(
                self.command,
                cwd=build,
                timeout=timeout,
                testcmd=True,
                host=host,
                background=True,
            )
            # Track RX as the primary process for any base-class introspection.
            self._process = rx_proc
            self._host = host
            time.sleep(sleep_interval)

            for idx, tx_cmd in enumerate(self._tx_commands):
                logger.info("[FFmpeg] Starting TX%d: %s", idx + 1, tx_cmd)
                tx_procs.append(
                    run(
                        tx_cmd,
                        cwd=build,
                        timeout=timeout,
                        testcmd=True,
                        host=host,
                        background=True,
                    )
                )

            logger.info(
                "[FFmpeg] Running test for %ds (timeout %ds)", test_time, timeout
            )
            # All TX paths stream indefinitely (FFmpeg ``-stream_loop -1`` or
            # RxTxApp without ``--test_time``); RX is the time-bounded side.
            # Match legacy semantics: wall-clock sleep then tear down. Validation
            # runs against captured RX state in the finally block.
            time.sleep(test_time)
        finally:
            logger.info("[FFmpeg] Stopping processes")
            for idx, p in enumerate(tx_procs):
                self._stop_proc(p, f"TX{idx + 1}")
            self._stop_proc(rx_proc, "RX")
            time.sleep(1)
            # ``kill_stale_processes`` walks ``MTL_APP_NAMES`` (which now
            # includes ``ffmpeg`` and every DPDK-renamed comm alias such as
            # ``RxTxApp_main``), so a single call covers both ffmpeg and
            # RxTxApp orphans \u2014 no per-pattern follow-up needed.
            kill_stale_processes(host)
            self._rx_output = self.capture_stdout(rx_proc, "RX")
            for idx, p in enumerate(tx_procs):
                self.capture_stdout(p, f"TX{idx + 1}")
            self.last_output = self._rx_output
            self.last_return_code = getattr(rx_proc, "return_code", None)
            self._process = None

        try:
            return self.validate_results(fail_on_error=fail_on_error)
        except AssertionError:
            if fail_on_error:
                raise
            logger.info(
                "[FFmpeg] validation failed (fail_on_error=False); returning False"
            )
            return False

    # --------------------------------------------------------- internals
    def _stop_proc(self, proc, label: str) -> None:
        """Stop a single process via SIGINT \u2192 SIGKILL ladder.

        Reuses :meth:`Application._signal_and_wait` so DPDK applications get
        a chance to run ``rte_eal_cleanup()`` before being force-killed.
        """
        if proc is None:
            return
        if self._signal_and_wait(proc, signal.SIGINT, self._STOP_GRACE_S):
            return
        logger.info(
            "[FFmpeg] %s did not exit on SIGINT after %ds; sending SIGKILL",
            label,
            self._STOP_GRACE_S,
        )
        try:
            proc.kill(wait=None, with_signal=signal.SIGKILL)
        except Exception as exc:  # pragma: no cover -- best-effort
            logger.debug("[FFmpeg] SIGKILL for %s failed: %s", label, exc)

    def _cleanup_output_files(self, host) -> None:
        """Delete tracked RX output files unless ``keep_output=True``."""
        if self._ff_params.get("keep_output"):
            return
        if not self._output_files or host is None:
            return
        try:
            run(f"rm -f {' '.join(self._output_files)}", host=host)
        except Exception as exc:  # pragma: no cover -- best-effort
            logger.info("Could not remove RX output files: %s", exc)

    # ----------------------------------------------------- validate
    def validate_results(self, fail_on_error: bool = True) -> bool:  # type: ignore[override]
        mode = self._ff_params.get("mode", _MODE_YUV_H264)
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
