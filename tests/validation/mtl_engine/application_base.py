"""Base application class providing unified interface for media framework adapters."""

import logging
import re
import time
from abc import ABC, abstractmethod

from .config.universal_params import UNIVERSAL_PARAMS
from .execute import run

logger = logging.getLogger(__name__)


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

    @abstractmethod
    def get_app_name(self) -> str:
        """Return the application name (e.g., 'RxTxApp', 'FFmpeg', 'GStreamer')."""
        pass

    @abstractmethod
    def get_executable_name(self) -> str:
        """Return the executable name for this framework."""
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
    def validate_results(self) -> bool:  # type: ignore[override]
        """Framework-specific validation implemented by subclasses.

        Subclasses should read: self.params, self.config, self.last_output, etc.
        Must return True/False.
        """
        raise NotImplementedError

    def set_params(self, **kwargs):
        """Set parameters from user input and track which were provided."""
        self._user_provided_params = set(kwargs.keys())

        for param, value in kwargs.items():
            if param in self.params:
                self.params[param] = value
            else:
                raise ValueError(f"Unknown parameter: {param}")

    def get_executable_path(self) -> str:
        """Get the full path to the executable based on framework type."""
        executable_name = self.get_executable_name()

        # For applications with specific paths, combine with directory
        if self.app_path and not executable_name.startswith("/"):
            # If app_path already ends with the executable name, use as-is
            if self.app_path.endswith(f"/{executable_name}"):
                return self.app_path
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
        capture_cfg=None,
        netsniff=None,
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
        """
        is_dual = tx_host is not None and rx_host is not None
        if is_dual and not rx_app:
            raise ValueError("rx_app instance required for dual-host execution")
        if not is_dual and not host:
            raise ValueError("host required for single-host execution")

        if not self.command:
            raise RuntimeError("create_command() must be called before execute_test()")
        framework_name = self.get_app_name()

        # Call framework-specific preparation hook
        if not is_dual:
            self.prepare_execution(build=build, host=host)
        else:
            self.prepare_execution(build=build, host=tx_host)
            if rx_app:
                rx_app.prepare_execution(build=build, host=rx_host)

        # Adjust test_time for PTP synchronization
        effective_test_time = test_time
        if self.params.get("enable_ptp", False):
            ptp_sync_time = self.params.get("ptp_sync_time", 50)
            effective_test_time += ptp_sync_time
            logger.info(
                f"PTP enabled: added {ptp_sync_time}s for sync (total: {effective_test_time}s)"
            )

        # Single-host execution
        if not is_dual:
            cmd = self.add_timeout(self.command, effective_test_time)
            logger.info(f"[single] Running {framework_name} command: {cmd}")
            proc = self.start_process(cmd, build, effective_test_time, host)
            if netsniff and hasattr(self, "_start_netsniff_capture"):
                try:
                    # Wait for PTP sync before starting capture
                    if self.params.get("enable_ptp", False):
                        ptp_sync_time = self.params.get("ptp_sync_time", 50)
                        logger.info(
                            f"Waiting {ptp_sync_time}s for PTP sync before netsniff capture"
                        )
                        time.sleep(ptp_sync_time)
                    self._start_netsniff_capture(netsniff)
                except Exception as e:
                    logger.warning(f"netsniff capture setup failed: {e}")
            try:
                proc.wait(
                    timeout=(effective_test_time or 0)
                    + self.params.get("process_timeout_buffer", 90)
                )
            except Exception:
                logger.warning(
                    f"{framework_name} process wait timed out (continuing to capture output)"
                )
            self.last_output = self.capture_stdout(proc, framework_name)
            self.last_return_code = proc.return_code
            return self.validate_results()

        # Dual-host execution (tx self, rx rx_app)
        assert rx_app is not None
        if not rx_app.command:
            raise RuntimeError(
                "rx_app has no prepared command (call create_command first)"
            )
        tx_cmd = self.add_timeout(self.command, effective_test_time)
        rx_cmd = rx_app.add_timeout(rx_app.command, effective_test_time)
        primary_first = tx_first
        first_cmd, first_host, first_label = (
            (tx_cmd, tx_host, f"{framework_name}-TX")
            if primary_first
            else (rx_cmd, rx_host, f"{rx_app.get_app_name()}-RX")
        )
        second_cmd, second_host, second_label = (
            (rx_cmd, rx_host, f"{rx_app.get_app_name()}-RX")
            if primary_first
            else (tx_cmd, tx_host, f"{framework_name}-TX")
        )
        logger.info(f"[dual] Starting first: {first_label} -> {first_cmd}")
        first_proc = self.start_process(
            first_cmd, build, effective_test_time, first_host
        )
        time.sleep(sleep_interval)
        logger.info(f"[dual] Starting second: {second_label} -> {second_cmd}")
        second_proc = self.start_process(
            second_cmd, build, effective_test_time, second_host
        )
        # Wait processes
        total_timeout = (effective_test_time or 0) + self.params.get(
            "process_timeout_buffer", 90
        )
        for p, label in [(first_proc, first_label), (second_proc, second_label)]:
            try:
                p.wait(timeout=total_timeout)
            except Exception:
                logger.warning(
                    f"Process {label} wait timeout; capturing partial output"
                )
        # Capture outputs
        if primary_first:
            self.last_output = self.capture_stdout(first_proc, first_label)
            rx_app.last_output = rx_app.capture_stdout(second_proc, second_label)
        else:
            rx_app.last_output = rx_app.capture_stdout(first_proc, first_label)
            self.last_output = self.capture_stdout(second_proc, second_label)
        self.last_return_code = first_proc.return_code
        rx_app.last_return_code = second_proc.return_code
        tx_ok = self.validate_results()
        rx_ok = rx_app.validate_results()
        return tx_ok and rx_ok

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
        """
        logger.info(f"Starting {self.get_app_name()} process...")
        buffer_val = self.params.get("process_timeout_buffer", 90)
        timeout = (test_time or 0) + buffer_val
        return run(command, host=host, cwd=build, timeout=timeout, background=True)

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
                logger.debug(f"{process_name} output: {output[:200]}...")
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
