# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation
"""Base application class providing unified interface for media framework adapters."""

import logging
import re
import signal
import time
from abc import ABC, abstractmethod
from dataclasses import dataclass, field
from typing import Callable, Optional

from .config.universal_params import UNIVERSAL_PARAMS
from .execute import kill_stale_processes, log_fail, run

logger = logging.getLogger(__name__)


# Maximum time (seconds) MTL's mt_ptp_wait_stable() may block before
# --test_time starts counting.  Shell timeout and Python wait must account
# for this worst-case delay when PTP is enabled.
MTL_PTP_INTERNAL_TIMEOUT = 180


# Encoder name -> MTL st22 plugin shared object, for require_encoder()
# pre-flight checks shared across framework adapters.
MTL_ENCODER_PLUGIN_MAP = {
    "libsvt_jpegxs": "libst_plugin_st22_svt_jpeg_xs.so",
    "libopenh264": "libst_plugin_st22_avcodec.so",
}


def mtl_plugin_check_cmd(plugin_so: str) -> str:
    """Return a shell test that succeeds when *plugin_so* is loadable on the host.

    Probes, in order: the dynamic-linker cache, the system plugin directories,
    and the plugin path recorded in the active ``KAHAWAI_CFG_PATH`` registry --
    which in CI points into the cached ``.local_install/plugins`` tree, so a
    locally built (non system-installed) plugin is detected too.
    """
    return (
        f"ldconfig -p 2>/dev/null | grep -q {plugin_so} "
        f"|| test -f /usr/local/lib/x86_64-linux-gnu/{plugin_so} "
        f"|| test -f /usr/local/lib64/{plugin_so} "
        f"|| ( [ -d .local_install/plugins ] && find .local_install/plugins -name "
        f"'{plugin_so}' 2>/dev/null | grep -q '{plugin_so}' ) "
        '|| { cfg="${KAHAWAI_CFG_PATH:-kahawai.json}"; ok=1; '
        f"for p in $(grep -F '{plugin_so}' \"$cfg\" 2>/dev/null | grep -oE '/[^\"]+\\.so'); do "
        '[ -f "$p" ] && ok=0 && break; done; [ "$ok" = 0 ]; }'
    )


@dataclass
class ProcSpec:
    """Specification for one process inside a :meth:`Application._run_proc_group` call.

    Attributes:
        cmd: Shell command to run (already wrapped with ``timeout`` if
            ``bounded`` is True; ``_run_proc_group`` does NOT wrap).
        host: Target host connection.
        label: Human-readable name used in log lines and as a stdout key.
        bounded: True if the command self-terminates (e.g. wrapped in
            ``timeout N`` or has its own ``--test_time``). False for
            indefinitely-running streams that the orchestrator must stop.
        captured_output: Filled in by :meth:`Application._run_proc_group`
            after stdout has been read.
        proc: Filled in by :meth:`Application._run_proc_group` with the
            live process handle (mfd-connect ``Process``).
    """

    cmd: str
    host: object
    label: str
    bounded: bool = True
    captured_output: str = ""
    proc: object = field(default=None, repr=False)


class Application(ABC):
    """Abstract base class shared by all framework adapters (RxTxApp / FFmpeg / GStreamer).

    Unified model:
      1. create_command(...) MUST be called first. It populates:
         - self.command: full shell command string ready to run
         - self.config: optional dict (RxTxApp) written immediately if a config_file_path is supplied
      2. execute_test(...) ONLY executes already prepared command(s); it NEVER builds commands.
         - Single-host: call execute_test with host=...
         - Dual-host: create TWO application objects (tx_app, rx_app) each with its own create_command();
           then call tx_app.execute_test(tx_host=..., rx_host=..., rx_app=rx_app).
      3. validate_results() now has a uniform no-argument signature and consumes internal state
         (self.params, self.config, self.last_output, and any produced files).
    """

    def __init__(self, app_path, config_file_path=None):
        self.app_path = app_path
        self.config_file_path = config_file_path
        self.params = UNIVERSAL_PARAMS.copy()
        self._user_provided_params = set()
        self.command: str | None = None
        self.config: dict | None = None
        self.last_output: str | None = None
        self.last_return_code: int | None = None
        # Process lifecycle tracking (set by start_process / stop_process)
        self._process = None
        self._host = None
        # Per-test output artifacts on the remote host. Subclasses populate
        # this in ``prepare_execution``; the base class deletes the files
        # after ``validate_results`` unless ``params['keep_output']`` is True.
        self._output_files: list[str] = []

    @property
    def output_files(self) -> list[str]:
        """Per-test output artifacts produced by the most recent run.

        Tests that pass ``keep_output=True`` can read the path(s) here to
        feed a follow-up integrity check before cleanup.
        """
        return list(self._output_files)

    @abstractmethod
    def get_app_name(self) -> str:
        """Return the application name (e.g., 'RxTxApp', 'FFmpeg', 'GStreamer')."""
        pass

    @abstractmethod
    def get_executable_name(self) -> str:
        """Return the executable name for this framework."""
        pass

    def require_encoder(self, host, encoder: str, use_mtl_plugin: bool = False) -> None:
        """Check that *encoder* is available. No-op by default; override in subclasses."""
        pass

    def create_command(self, **kwargs):
        """Populate self.command (and optionally self.config) from user parameters.

        This method handles common setup, then delegates to the framework-specific
        _create_command_and_config() abstract method.

        Args:
            **kwargs: Universal parameters to set before building command

        Returns:
            Tuple of (command_string, config_dict_or_None) for backward compatibility
        """
        # Reset params to defaults on every invocation. The application instances
        # (e.g. the ``rxtxapp`` fixture) are session-scoped and reused across
        # tests; without this reset, parameters set by an earlier test
        # (rx_timing_parser, tx_no_chain, replicas, enable_ptp, framerate, …)
        # silently leak into the next test and cause spurious failures.
        self.params = UNIVERSAL_PARAMS.copy()
        self.set_params(**kwargs)
        self.command, self.config = self._create_command_and_config()
        return self.command, self.config

    @abstractmethod
    def _create_command_and_config(self) -> tuple:
        """Framework-specific command and config generation.

        Subclasses must implement this to build their specific command line
        and configuration from self.params.

        Returns:
            Tuple of (command_string, config_dict_or_None)
        """
        raise NotImplementedError

    @abstractmethod
    def validate_results(self, fail_on_error: bool = True) -> bool:  # type: ignore[override]
        """Framework-specific validation implemented by subclasses.

        Subclasses should read: self.params, self.config, self.last_output, etc.
        Must return True/False. When ``fail_on_error`` is False, subclasses
        should suppress side effects such as ``log_fail`` and simply return
        False / raise AssertionError so the caller can decide.
        """
        raise NotImplementedError

    def _fail_validation(self, msg: str, fail_on_error: bool) -> None:
        """Uniform soft/hard failure path for ``validate_results`` subclasses.

        - ``fail_on_error=True``: record a pytest failure via ``log_fail`` and
          raise ``AssertionError`` so the test stops.
        - ``fail_on_error=False``: log at INFO and raise ``AssertionError``
          without recording a pytest failure. Callers (e.g. performance
          binary-search loops) catch the exception and treat it as a soft
          ``False`` without aborting the run.

        Always raises; never returns.
        """
        if fail_on_error:
            log_fail(msg)
        else:
            logger.info("validate_results soft-fail (fail_on_error=False): %s", msg)
        raise AssertionError(msg)

    def _resolve_capture_dst_ip(self):
        """Return the destination IP that netsniff should filter on, or ``None``.

        Subclasses override this when their config schema exposes the TX
        destination(s). The default returns ``None``, which causes
        ``_start_netsniff_capture`` to skip the capture with a warning rather
        than raising.
        """
        return None

    def _start_netsniff_capture(self, netsniff) -> None:
        """Configure ``netsniff`` and start a bounded capture.

        Generic across frameworks: the only app-specific bit is *where* the
        destination IP comes from, which is delegated to
        :meth:`_resolve_capture_dst_ip`. The capture window matches the
        ``test_time`` param so capture and process lifetimes line up.
        """
        dst_ip = self._resolve_capture_dst_ip()
        if not dst_ip:
            logger.warning("No destination IP available for netsniff capture")
            return
        capture_time = self.params.get("test_time", 30)
        try:
            netsniff.update_filter(dst_ip=dst_ip)
            netsniff.capture(capture_time=capture_time)
            logger.info("Started netsniff-ng capture for destination IP %s", dst_ip)
        except Exception as e:
            logger.error("Failed to start netsniff capture: %s", e)

    def set_params(self, **kwargs):
        """Set parameters from user input and track which were provided."""
        self._user_provided_params = set(kwargs.keys())

        for param, value in kwargs.items():
            if param in self.params:
                self.params[param] = value
            else:
                raise ValueError(f"Unknown parameter: {param}")

    def get_executable_path(self) -> str:
        """Get the full path to the executable based on framework type.

        ``app_path`` must be a directory-only path.  ``get_executable_name()``
        must return the bare executable name.  A ``ValueError`` is raised when
        ``app_path`` already contains the executable name — callers should fix
        the constant / variable they pass as ``app_path``.
        """
        executable_name = self.get_executable_name()

        # For applications with specific paths, combine with directory
        if self.app_path and not executable_name.startswith("/"):
            if self.app_path.endswith(f"/{executable_name}"):
                raise ValueError(
                    f"app_path must be a directory, not a full executable path. "
                    f"Got '{self.app_path}' which already ends with "
                    f"'/{executable_name}'. Pass the directory only."
                )
            if self.app_path.endswith("/"):
                return f"{self.app_path}{executable_name}"
            return f"{self.app_path}/{executable_name}"
        else:
            # For system executables or full paths
            return executable_name

    def was_user_provided(self, param_name: str) -> bool:
        """Check if a parameter was explicitly provided by the user."""
        return param_name in self._user_provided_params

    def prepare_execution(self, build: str, host=None, **kwargs):
        """Hook method called before execution to perform framework-specific setup.

        Subclasses can override this to write config files, set up environment, etc.
        Default implementation does nothing.

        Args:
            build: Build directory path
            host: Host connection object
            **kwargs: Additional arguments passed from execute_test
        """
        pass

    def execute_test(
        self,
        build: str,
        test_time: int = 30,
        host=None,
        tx_host=None,
        rx_host=None,
        rx_app=None,
        sleep_interval: int = 4,
        tx_first: bool = True,
        netsniff=None,
        interface_setup=None,
        fail_on_error: bool = True,
    ) -> bool:
        """Execute a prepared command (or two for dual host).

        Usage patterns:
          # Single host
          app.create_command(...)
          app.execute_test(build=..., host=my_host, test_time=10)

          # Dual host
          tx_app.create_command(direction='tx', ...)
          rx_app.create_command(direction='rx', ...)
          tx_app.execute_test(build=..., tx_host=hostA, rx_host=hostB, rx_app=rx_app)

        Args:
            interface_setup: Optional InterfaceSetup helper. Forwarded to framework
                ``prepare_execution`` so frameworks (e.g. RxTxApp) can configure
                kernel-socket interfaces and register IPs for cleanup.
            fail_on_error: When True (default) propagate validation failures
                (AssertionError) to the caller. When False, swallow validation
                failures and return ``False`` instead. Used by performance/binary
                search tests that drive the call site based on the boolean.
        """
        is_dual = tx_host is not None and rx_host is not None
        if is_dual and not rx_app:
            raise ValueError("rx_app instance required for dual-host execution")
        if not is_dual and not host:
            raise ValueError("host required for single-host execution")

        if not self.command:
            raise RuntimeError("create_command() must be called before execute_test()")
        framework_name = self.get_app_name()

        # Framework-specific preparation
        if not is_dual:
            self.prepare_execution(
                build=build, host=host, interface_setup=interface_setup
            )
        else:
            self.prepare_execution(
                build=build, host=tx_host, interface_setup=interface_setup
            )
            rx_app.prepare_execution(
                build=build, host=rx_host, interface_setup=interface_setup
            )

        effective_test_time, ptp_timeout_budget = self._apply_ptp_extension(test_time)
        wait_timeout = (effective_test_time or 0) + self.params.get(
            "process_timeout_buffer", 90
        )
        wait_timeout += ptp_timeout_budget
        cmd_test_time = effective_test_time + ptp_timeout_budget

        # Build the netsniff hook (single-host only, fired once after the
        # primary process starts) so the helper stays oblivious to capture.
        after_first_start = self._make_netsniff_hook(netsniff) if netsniff else None

        if not is_dual:
            specs = [
                ProcSpec(
                    cmd=self.add_timeout(self.command, cmd_test_time),
                    host=host,
                    label=framework_name,
                    bounded=True,
                )
            ]
            self._run_proc_group(
                specs,
                build=build,
                test_time=effective_test_time,
                proc_wait_timeout=wait_timeout,
                after_first_start=after_first_start,
            )
            self.last_output = specs[0].captured_output
            self.last_return_code = self._safe_return_code(specs[0].proc)
            return self._dispatch_validate(fail_on_error)

        # Dual-host: 2 bounded procs across 2 hosts.
        if not rx_app.command:
            raise RuntimeError(
                "rx_app has no prepared command (call create_command first)"
            )
        tx_spec = ProcSpec(
            cmd=self.add_timeout(self.command, cmd_test_time),
            host=tx_host,
            label=f"{framework_name}-TX",
            bounded=True,
        )
        rx_spec = ProcSpec(
            cmd=rx_app.add_timeout(rx_app.command, cmd_test_time),
            host=rx_host,
            label=f"{rx_app.get_app_name()}-RX",
            bounded=True,
        )
        ordered = [tx_spec, rx_spec] if tx_first else [rx_spec, tx_spec]
        self._run_proc_group(
            specs=ordered,
            build=build,
            test_time=effective_test_time,
            proc_wait_timeout=wait_timeout,
            sleep_interval=sleep_interval,
        )
        self.last_output = tx_spec.captured_output
        self.last_return_code = self._safe_return_code(tx_spec.proc)
        rx_app.last_output = rx_spec.captured_output
        rx_app.last_return_code = self._safe_return_code(rx_spec.proc)
        try:
            tx_ok = self.validate_results()
            rx_ok = rx_app.validate_results()
        except AssertionError:
            if fail_on_error:
                raise
            logger.info(
                f"{framework_name} validation failed (fail_on_error=False); returning False"
            )
            return False
        return tx_ok and rx_ok

    # ------------------------------------------------- shared orchestration
    def _apply_ptp_extension(self, test_time: int) -> tuple[int, int]:
        """Extend ``test_time`` and ``self.command`` for PTP sync overhead.

        Returns ``(effective_test_time, ptp_timeout_budget)``. When PTP is
        disabled both pass-through; when enabled, ``test_time`` is bumped by
        ``ptp_sync_time`` and the in-process ``--test_time N`` (if any) is
        rewritten so the application's data window survives PTP startup
        (otherwise the wrapper kills it with rc=124 mid-sync).
        """
        if not self.params.get("enable_ptp", False):
            return test_time, 0
        ptp_sync_time = self.params.get("ptp_sync_time", 50)
        effective = test_time + ptp_sync_time
        logger.info(
            "PTP enabled: added %ds for sync (total: %ds)", ptp_sync_time, effective
        )
        new_cmd, n_subs = re.subn(
            r"(--test_time\s+)\d+",
            rf"\g<1>{effective}",
            self.command,
            count=1,
        )
        if n_subs:
            self.command = new_cmd
        return effective, MTL_PTP_INTERNAL_TIMEOUT

    def _make_netsniff_hook(self, netsniff) -> Callable:
        """Return a ``after_first_start`` callback that arms netsniff capture.

        Waits for PTP sync (when enabled) before starting capture so the
        capture window aligns with the steady-state stream.
        """

        def _hook(_first_proc) -> None:
            try:
                if self.params.get("enable_ptp", False):
                    ptp_sync_time = self.params.get("ptp_sync_time", 50)
                    logger.info(
                        "Waiting %ds for PTP sync before netsniff capture",
                        ptp_sync_time,
                    )
                    time.sleep(ptp_sync_time)
                self._start_netsniff_capture(netsniff)
            except Exception as e:
                logger.warning("netsniff capture setup failed: %s", e)

        return _hook

    def _dispatch_validate(self, fail_on_error: bool) -> bool:
        """Run :meth:`validate_results` with consistent soft-fail semantics."""
        try:
            return self.validate_results(fail_on_error=fail_on_error)
        except AssertionError:
            if fail_on_error:
                raise
            logger.info(
                "%s validation failed (fail_on_error=False); returning False",
                self.get_app_name(),
            )
            return False

    def _run_proc_group(
        self,
        specs: list[ProcSpec],
        *,
        build: str,
        test_time: int,
        sleep_interval: float = 0,
        wall_clock_seconds: Optional[float] = None,
        proc_wait_timeout: Optional[float] = None,
        cleanup_host=None,
        after_first_start: Optional[Callable] = None,
    ) -> list[ProcSpec]:
        """Start a group of processes, wait, stop, capture stdout. Generic.

        Two waiting strategies, picked by the caller:

        * ``wall_clock_seconds`` set: the orchestrator sleeps that long, then
          tears every spec down (used by FFmpeg whose RX/TX streams never
          self-terminate).
        * ``proc_wait_timeout`` set: each ``bounded=True`` spec is awaited
          with that timeout (used by RxTxApp/Gstreamer whose commands carry
          their own ``timeout N`` shell wrapper).

        For each spec, ``cmd`` must already be timeout-wrapped if ``bounded``.
        Stdout is read inside ``finally`` and assigned to ``spec.captured_output``;
        the live process handle is left on ``spec.proc``.

        ``after_first_start(first_proc)`` runs once after the first spec
        starts — used to arm netsniff capture without the helper having to
        know about it.

        Side effects on ``self``: ``self._process`` is set to the first
        spec's process while the group runs (so legacy hooks that introspect
        it still work) and cleared in the finally block.
        """
        framework_name = self.get_app_name()
        try:
            for idx, spec in enumerate(specs):
                if idx and sleep_interval:
                    time.sleep(sleep_interval)
                logger.info(
                    "[%s] Starting %s: %s", framework_name, spec.label, spec.cmd
                )
                spec.proc = self.start_process(spec.cmd, build, test_time, spec.host)
                if idx == 0:
                    self._process = spec.proc
                    self._host = spec.host
                    if after_first_start is not None:
                        after_first_start(spec.proc)

            if wall_clock_seconds is not None:
                logger.info(
                    "[%s] Running for %ds (wall clock)",
                    framework_name,
                    wall_clock_seconds,
                )
                time.sleep(wall_clock_seconds)
            else:
                for spec in specs:
                    if not spec.bounded:
                        continue
                    try:
                        spec.proc.wait(timeout=proc_wait_timeout)
                    except Exception:
                        logger.warning(
                            "[%s] %s wait timed out; capturing partial output",
                            framework_name,
                            spec.label,
                        )
        finally:
            # Unbounded specs need an explicit stop ladder; bounded specs
            # already exited (timeout wrapper or proc.wait above).
            for spec in specs:
                if not spec.bounded and spec.proc is not None:
                    self._stop_unbounded_proc(spec.proc, spec.label)
            if cleanup_host is not None:
                # Drains every comm alias in MTL_APP_NAMES (incl. DPDK-renamed
                # RxTxApp_main). Single call covers ffmpeg + RxTxApp orphans.
                kill_stale_processes(cleanup_host)
            for spec in specs:
                spec.captured_output = self.capture_stdout(spec.proc, spec.label)
            self._process = None
        return specs

    def _stop_unbounded_proc(self, proc, label: str) -> None:
        """SIGINT → SIGKILL ladder for indefinitely-running processes.

        Reuses :meth:`_signal_and_wait` so DPDK applications get a chance to
        run ``rte_eal_cleanup()`` before being force-killed (otherwise the
        VFIO group fd leaks and ``nicctl disable_vf`` blocks).

        After SIGKILL, the kernel takes a moment to reap the process and the
        SSH-side ``proc.running`` poll may still return True briefly; we wait
        a few seconds so subsequent ``return_code`` reads do not raise
        :class:`mfd_connect.exceptions.RemoteProcessInvalidState`.
        """
        if proc is None:
            return
        graceful_s = self.params.get("stop_graceful_s", 10)
        if self._signal_and_wait(proc, signal.SIGINT, graceful_s):
            return
        logger.info(
            "[%s] %s did not exit on SIGINT after %ds; sending SIGKILL",
            self.get_app_name(),
            label,
            graceful_s,
        )
        # _signal_and_wait already polls until the process exits, so use it
        # for SIGKILL as well rather than fire-and-forget kill().
        if not self._signal_and_wait(
            proc, signal.SIGKILL, self.params.get("stop_term_s", 5)
        ):
            logger.warning(
                "[%s] %s still running after SIGKILL; return_code may be unavailable",
                self.get_app_name(),
                label,
            )

    def _cleanup_output_files(self, host) -> None:
        """Delete tracked per-test output files unless ``keep_output=True``.

        Subclasses populate ``self._output_files`` (e.g. in
        ``prepare_execution``) and call this from ``validate_results`` when
        the artifacts are no longer needed for downstream checks.
        """
        if self.params.get("keep_output"):
            return
        if not self._output_files or host is None:
            return
        try:
            run(f"rm -f {' '.join(self._output_files)}", host=host)
        except Exception as exc:  # pragma: no cover -- best-effort
            logger.info("Could not remove RX output files: %s", exc)

    def add_timeout(self, command: str, test_time: int, grace: int = None) -> str:
        """Wrap command with timeout if test_time provided.

        Args:
            command: Shell command to wrap
            test_time: Test duration in seconds
            grace: Grace period to add (default from params)

        Returns:
            Command wrapped with timeout
        """
        if grace is None:
            grace = self.params.get("timeout_grace", 10)

        # Extract internal --test_time to prevent premature timeout termination
        internal_test_time = None
        m = re.search(r"--test_time\s+(\d+)", command)
        if m:
            try:
                internal_test_time = int(m.group(1))
            except ValueError:
                internal_test_time = None

        effective_test_time = test_time or internal_test_time
        if internal_test_time and test_time and internal_test_time != test_time:
            logger.debug(
                f"Test time mismatch (execute={test_time}, command={internal_test_time}); using max"
            )
            effective_test_time = max(internal_test_time, test_time)
        elif internal_test_time and not test_time:
            effective_test_time = internal_test_time

        if effective_test_time and not command.strip().startswith("timeout "):
            return f"timeout {effective_test_time + grace} {command}"
        return command

    def extract_framerate(self, framerate_str, default: int = None) -> int:
        """Extract numeric framerate from various string or numeric forms (e.g. 'p25', '60')."""
        if default is None:
            default = self.params.get("default_framerate_numeric", 60)
        if isinstance(framerate_str, (int, float)):
            try:
                return int(framerate_str)
            except Exception:
                return default
        if not isinstance(framerate_str, str) or not framerate_str:
            return default
        if framerate_str.startswith("p") and len(framerate_str) > 1:
            num = framerate_str[1:]
        else:
            num = framerate_str
        try:
            return int(float(num))
        except ValueError:
            logger.warning(
                f"Could not parse framerate '{framerate_str}', defaulting to {default}"
            )
            return default

    def start_process(self, command: str, build: str, test_time: int, host):
        """Start a process on the specified host using mfd_connect.

        Returns a background process handle. The caller is responsible for
        calling proc.wait() after any concurrent work (e.g. netsniff capture).
        The handle is also stored as ``self._process`` for :meth:`stop_process`.
        """
        logger.info(f"Starting {self.get_app_name()} process...")
        buffer_val = self.params.get("process_timeout_buffer", 90)
        timeout = (test_time or 0) + buffer_val
        proc = run(command, host=host, cwd=build, timeout=timeout, background=True)
        self._process = proc
        self._host = host
        return proc

    def stop_process(self, host=None) -> None:
        """Stop this application's tracked process gracefully.

        Sends SIGINT first so that DPDK applications run their atexit
        handler and call ``rte_eal_cleanup()`` — which closes the VFIO
        group fd and releases the kernel-side refcount on the VFs.
        Skipping this step is the root cause of the long-standing
        ``nicctl disable_vf`` hang in ``vfio_unregister_group_dev``.

        Ladder:
            1. SIGINT  → wait up to ``graceful_s`` (default 10s) for clean exit.
            2. SIGTERM → wait up to additional ``term_s`` (default 5s).
            3. SIGKILL → last resort.
            4. :func:`kill_stale_processes` for any leftovers, then poll
               ``/dev/vfio/*`` until no process holds it (max ``vfio_idle_s``).

        Safe to call multiple times or when no process was started.
        """
        graceful_s = self.params.get("stop_graceful_s", 10)
        term_s = self.params.get("stop_term_s", 5)
        vfio_idle_s = self.params.get("stop_vfio_idle_s", 15)

        proc = self._process
        if proc is not None:
            try:
                if not self._signal_and_wait(proc, signal.SIGINT, graceful_s):
                    logger.info(
                        "Process did not exit on SIGINT after %ds, sending SIGTERM",
                        graceful_s,
                    )
                    if not self._signal_and_wait(proc, signal.SIGTERM, term_s):
                        logger.warning(
                            "Process did not exit on SIGTERM after %ds, sending SIGKILL "
                            "— VFIO refcount may leak",
                            term_s,
                        )
                        try:
                            proc.kill(wait=None, with_signal=signal.SIGKILL)
                        except Exception:
                            pass
            except Exception as e:
                logger.warning("Error during graceful stop: %s", e)
            self._process = None

        target_host = host or self._host
        if target_host is not None:
            kill_stale_processes(target_host)
            self._wait_vfio_idle(target_host, vfio_idle_s)

    @staticmethod
    def _safe_return_code(proc) -> int | None:
        """Return ``proc.return_code`` or ``None`` if it is not yet available.

        ``mfd_connect``'s ``return_code`` property raises
        :class:`RemoteProcessInvalidState` while the process is still running,
        and ``getattr(proc, 'return_code', None)`` does *not* swallow it.
        """
        if proc is None:
            return None
        try:
            return proc.return_code
        except Exception:  # incl. RemoteProcessInvalidState
            return None

    def _signal_and_wait(self, proc, sig, timeout_s: float) -> bool:
        """Send signal to *proc* and poll until exit or timeout. Returns True if exited."""
        try:
            proc.kill(wait=None, with_signal=sig)
        except Exception as e:
            logger.debug("Signal %s send failed: %s", sig, e)
            return False
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            try:
                if not proc.running:
                    return True
            except Exception:
                # If we can't query state, assume it's still running.
                pass
            time.sleep(0.25)
        try:
            return not proc.running
        except Exception:
            return False

    @staticmethod
    def _wait_vfio_idle(host, timeout_s: float) -> bool:
        """Poll until no process holds /dev/vfio/<group> fds. Returns True if idle."""
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            try:
                res = host.connection.execute_command(
                    "sudo lsof /dev/vfio/* 2>/dev/null | "
                    "awk 'NR>1 && $1 != \"vfio-pci\" {print}' | wc -l",
                    shell=True,
                    timeout=5,
                    expected_return_codes=None,
                )
                if (res.stdout or "0").strip() == "0":
                    return True
            except Exception:
                return False
            time.sleep(0.5)
        logger.warning(
            "VFIO still busy after %ds — disable_vf may block in kernel", timeout_s
        )
        return False

    def capture_stdout(self, process, process_name: str) -> str:
        """Capture stdout from mfd_connect process.

        Note: Must be called after process.wait() completes, as stdout_text
        is only available after the process finishes.

        Args:
            process: mfd_connect process object with stdout_text attribute
            process_name: Name for logging purposes

        Returns:
            Process stdout as string, or empty string if unavailable
        """
        try:
            output = process.stdout_text or ""
            if output:
                logger.debug(
                    f"{process_name} output (full, {len(output)} chars):\n{output}"
                )
            return output
        except AttributeError:
            logger.error(
                f"Process object missing stdout_text attribute for {process_name}"
            )
            return ""
        except Exception as e:
            logger.error(f"Error capturing output from {process_name}: {e}")
            return ""

    def get_case_id(self) -> str:
        """Generate test case identifier from call stack for logging."""
        try:
            import inspect

            frame = inspect.currentframe()
            while frame:
                if "test_" in frame.f_code.co_name:
                    return frame.f_code.co_name
                frame = frame.f_back
            return "unknown_test"
        except Exception:
            return "unknown_test"
