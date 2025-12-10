# Base Application Class for Media Transport Library
# Provides common interface for all media application frameworks

import logging
import re
import time
from abc import ABC, abstractmethod

from .config.app_mappings import DEFAULT_PAYLOAD_TYPE_CONFIG, DEFAULT_PORT_CONFIG
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
         (self.universal_params, self.config, self.last_output, and any produced files).
    """

    def __init__(self, app_path, config_file_path=None):
        self.app_path = app_path
        self.config_file_path = config_file_path
        self.universal_params = UNIVERSAL_PARAMS.copy()
        self._user_provided_params = set()
        self.command: str | None = None
        self.config: dict | None = None
        self.last_output: str | None = None
        self.last_return_code: int | None = None

    @abstractmethod
    def get_framework_name(self) -> str:
        """Return the framework name (e.g., 'RxTxApp', 'FFmpeg', 'GStreamer')."""
        pass

    @abstractmethod
    def get_executable_name(self) -> str:
        """Return the executable name for this framework."""
        pass

    @abstractmethod
    def create_command(self, **kwargs):
        """Populate self.command (+ self.config for frameworks that need it).

        Implementations MUST:
        - call self.set_universal_params(**kwargs)
        - set self.command (string)
        - optionally set self.config
        - write config file immediately if applicable
        They MAY return (self.command, self.config) for backward compatibility with existing tests.
        """
        raise NotImplementedError

    @abstractmethod
    def validate_results(self) -> bool:  # type: ignore[override]
        """Framework-specific validation implemented by subclasses.

        Subclasses should read: self.universal_params, self.config, self.last_output, etc.
        Must return True/False.
        """
        raise NotImplementedError

    def set_universal_params(self, **kwargs):
        """Set universal parameters and track which were provided by user."""
        self._user_provided_params = set(kwargs.keys())

        for param, value in kwargs.items():
            if param in self.universal_params:
                self.universal_params[param] = value
            else:
                raise ValueError(f"Unknown universal parameter: {param}")

    def get_executable_path(self) -> str:
        """Get the full path to the executable based on framework type."""
        executable_name = self.get_executable_name()

        # For applications with specific paths, combine with directory
        if self.app_path and not executable_name.startswith("/"):
            if self.app_path.endswith("/"):
                return f"{self.app_path}{executable_name}"
            else:
                return f"{self.app_path}/{executable_name}"
        else:
            # For system executables or full paths
            return executable_name

    def was_user_provided(self, param_name: str) -> bool:
        """Check if a parameter was explicitly provided by the user."""
        return param_name in self._user_provided_params

    def get_session_default_port(self, session_type: str) -> int:
        """Get default port for a specific session type."""
        port_map = {
            "st20p": DEFAULT_PORT_CONFIG["st20p_port"],
            "st22p": DEFAULT_PORT_CONFIG["st22p_port"],
            "st30p": DEFAULT_PORT_CONFIG["st30p_port"],
            "video": DEFAULT_PORT_CONFIG["video_port"],
            "audio": DEFAULT_PORT_CONFIG["audio_port"],
            "ancillary": DEFAULT_PORT_CONFIG["ancillary_port"],
            "fastmetadata": DEFAULT_PORT_CONFIG["fastmetadata_port"],
        }
        return port_map.get(session_type, DEFAULT_PORT_CONFIG["st20p_port"])

    def get_session_default_payload_type(self, session_type: str) -> int:
        """Get default payload type for a specific session type."""
        payload_map = {
            "st20p": DEFAULT_PAYLOAD_TYPE_CONFIG["st20p_payload_type"],
            "st22p": DEFAULT_PAYLOAD_TYPE_CONFIG["st22p_payload_type"],
            "st30p": DEFAULT_PAYLOAD_TYPE_CONFIG["st30p_payload_type"],
            "video": DEFAULT_PAYLOAD_TYPE_CONFIG["video_payload_type"],
            "audio": DEFAULT_PAYLOAD_TYPE_CONFIG["audio_payload_type"],
            "ancillary": DEFAULT_PAYLOAD_TYPE_CONFIG["ancillary_payload_type"],
            "fastmetadata": DEFAULT_PAYLOAD_TYPE_CONFIG["fastmetadata_payload_type"],
        }
        return payload_map.get(
            session_type, DEFAULT_PAYLOAD_TYPE_CONFIG["st20p_payload_type"]
        )

    def get_common_session_params(self, session_type: str) -> dict:
        """Get common session parameters used across all session types."""
        default_port = self.get_session_default_port(session_type)
        default_payload = self.get_session_default_payload_type(session_type)

        return {
            "replicas": self.universal_params.get(
                "replicas", UNIVERSAL_PARAMS["replicas"]
            ),
            "start_port": int(
                self.universal_params.get("port")
                if self.was_user_provided("port")
                else default_port
            ),
            "payload_type": (
                self.universal_params.get("payload_type")
                if self.was_user_provided("payload_type")
                else default_payload
            ),
        }

    def get_common_video_params(self) -> dict:
        """Get common video parameters used across video session types."""
        return {
            "width": int(self.universal_params.get("width", UNIVERSAL_PARAMS["width"])),
            "height": int(
                self.universal_params.get("height", UNIVERSAL_PARAMS["height"])
            ),
            "interlaced": self.universal_params.get(
                "interlaced", UNIVERSAL_PARAMS["interlaced"]
            ),
            "device": self.universal_params.get("device", UNIVERSAL_PARAMS["device"]),
            "enable_rtcp": self.universal_params.get(
                "enable_rtcp", UNIVERSAL_PARAMS["enable_rtcp"]
            ),
        }

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
        framework_name = self.get_framework_name()

        # Single-host execution
        if not is_dual:
            cmd = self.add_timeout(self.command, test_time)
            logger.info(f"[single] Running {framework_name} command: {cmd}")
            # Optional tcpdump capture hook retained for RxTxApp compatibility
            if (
                capture_cfg
                and capture_cfg.get("enable")
                and "prepare_tcpdump" in globals()
            ):
                try:
                    # prepare_tcpdump not yet implemented; left to change in the future
                    # prepare_tcpdump(capture_cfg, host)
                    pass
                except Exception as e:
                    logger.warning(f"capture setup failed: {e}")
            proc = self.start_process(cmd, build, test_time, host)
            try:
                proc.wait(
                    timeout=(test_time or 0)
                    + self.universal_params.get("process_timeout_buffer", 90)
                )
            except Exception:
                logger.warning(
                    f"{framework_name} process wait timed out (continuing to capture output)"
                )
            self.last_output = self.capture_stdout(proc, framework_name)
            self.last_return_code = getattr(proc, "returncode", None)
            return self.validate_results()

        # Dual-host execution (tx self, rx rx_app)
        assert rx_app is not None
        if not rx_app.command:
            raise RuntimeError(
                "rx_app has no prepared command (call create_command first)"
            )
        tx_cmd = self.add_timeout(self.command, test_time)
        rx_cmd = rx_app.add_timeout(rx_app.command, test_time)
        primary_first = tx_first
        first_cmd, first_host, first_label = (
            (tx_cmd, tx_host, f"{framework_name}-TX")
            if primary_first
            else (rx_cmd, rx_host, f"{rx_app.get_framework_name()}-RX")
        )
        second_cmd, second_host, second_label = (
            (rx_cmd, rx_host, f"{rx_app.get_framework_name()}-RX")
            if primary_first
            else (tx_cmd, tx_host, f"{framework_name}-TX")
        )
        logger.info(f"[dual] Starting first: {first_label} -> {first_cmd}")
        first_proc = self.start_process(first_cmd, build, test_time, first_host)
        time.sleep(sleep_interval)
        logger.info(f"[dual] Starting second: {second_label} -> {second_cmd}")
        second_proc = self.start_process(second_cmd, build, test_time, second_host)
        # Wait processes
        total_timeout = (test_time or 0) + self.universal_params.get(
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
        self.last_return_code = getattr(first_proc, "returncode", None)
        rx_app.last_return_code = getattr(second_proc, "returncode", None)
        tx_ok = self.validate_results()
        rx_ok = rx_app.validate_results()
        return tx_ok and rx_ok

    # -------------------------
    # Common helper utilities
    # -------------------------
    def add_timeout(self, command: str, test_time: int, grace: int = None) -> str:
        """Wrap command with timeout if test_time provided (adds a grace period)."""
        if grace is None:
            grace = self.universal_params.get("timeout_grace", 10)
        # If the command already has an internal --test_time X argument, ensure the wrapper
        # timeout is >= that internal value + grace to avoid premature SIGTERM (RC 124).
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
                f"Mismatch between execute_test test_time={test_time} and command --test_time {internal_test_time}; "
                f"using max"
            )
            effective_test_time = max(internal_test_time, test_time)
        elif internal_test_time and not test_time:
            effective_test_time = internal_test_time
        if effective_test_time and not command.strip().startswith("timeout "):
            return f"timeout {effective_test_time + grace} {command}"
        return command

    def start_and_capture(
        self, command: str, build: str, test_time: int, host, process_name: str
    ):
        """Start a single process and capture its stdout safely."""
        process = self.start_process(command, build, test_time, host)
        output = self.capture_stdout(process, process_name)
        return process, output

    def start_dual_with_delay(
        self,
        tx_command: str,
        rx_command: str,
        build: str,
        test_time: int,
        tx_host,
        rx_host,
        tx_first: bool,
        sleep_interval: int,
        tx_name: str,
        rx_name: str,
    ):
        """Start two processes with an optional delay ordering TX/RX based on tx_first flag."""
        if tx_first:
            tx_process = self.start_process(tx_command, build, test_time, tx_host)
            time.sleep(sleep_interval)
            rx_process = self.start_process(rx_command, build, test_time, rx_host)
        else:
            rx_process = self.start_process(rx_command, build, test_time, rx_host)
            time.sleep(sleep_interval)
            tx_process = self.start_process(tx_command, build, test_time, tx_host)
        tx_output = self.capture_stdout(tx_process, tx_name)
        rx_output = self.capture_stdout(rx_process, rx_name)
        return (tx_process, rx_process, tx_output, rx_output)

    def extract_framerate(self, framerate_str, default: int = None) -> int:
        """Extract numeric framerate from various string or numeric forms (e.g. 'p25', '60')."""
        if default is None:
            default = self.universal_params.get("default_framerate_numeric", 60)
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

    # Legacy execute_* abstract methods removed; unified execute_test used instead.

    def start_process(self, command: str, build: str, test_time: int, host):
        """Start a process on the specified host."""
        logger.info(f"Starting {self.get_framework_name()} process...")
        buffer_val = self.universal_params.get("process_timeout_buffer", 90)
        timeout = (test_time or 0) + buffer_val
        return run(command, host=host, cwd=build, timeout=timeout)

    def capture_stdout(self, process, process_name: str) -> str:
        """Capture stdout from a process."""
        try:
            # Remote process objects (from mfd_connect) expose stdout via 'stdout_text'
            if hasattr(process, "stdout_text") and process.stdout_text:
                output = process.stdout_text
                logger.debug(
                    f"{process_name} output (captured stdout_text): {output[:200]}..."
                )
                return output
            # Local fallback (subprocess) may expose .stdout already consumed elsewhere
            if hasattr(process, "stdout") and process.stdout:
                try:
                    # Attempt to read if it's a file-like object
                    if hasattr(process.stdout, "read"):
                        output = process.stdout.read()
                    else:
                        output = str(process.stdout)
                    logger.debug(
                        f"{process_name} output (captured stdout): {output[:200]}..."
                    )
                    return output
                except Exception:
                    pass
            logger.warning(f"No stdout available for {process_name}")
            return ""
        except Exception as e:
            logger.error(f"Error capturing {process_name} output: {e}")
            return ""

    def get_case_id(self) -> str:
        """Generate a case ID for logging/debugging purposes."""
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
