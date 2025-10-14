# Base Application Class for Media Transport Library
# Provides common interface for all media application frameworks

import json
import logging
import time
import os
from abc import ABC, abstractmethod

from .config.universal_params import UNIVERSAL_PARAMS
from .config.app_mappings import (
    DEFAULT_NETWORK_CONFIG,
    DEFAULT_PORT_CONFIG,
    DEFAULT_PAYLOAD_TYPE_CONFIG,
)

# Import execution utilities with fallback
try:
    from .execute import log_fail, run, is_process_running
    from .RxTxApp import prepare_tcpdump
except ImportError:
    # Fallback for direct execution
    from execute import log_fail, run, is_process_running
    from RxTxApp import prepare_tcpdump

logger = logging.getLogger(__name__)


class Application(ABC):
    """
    Abstract base class for all media application frameworks.
    Provides common functionality and interface that all child classes must implement.
    """
    
    def __init__(self, app_path, config_file_path=None):
        """Initialize application with path to application directory and optional config file."""
        self.app_path = app_path  # Path to directory containing the application
        self.config_file_path = config_file_path
        self.universal_params = UNIVERSAL_PARAMS.copy()
        self._user_provided_params = set()

    @abstractmethod
    def get_framework_name(self) -> str:
        """Return the framework name (e.g., 'RxTxApp', 'FFmpeg', 'GStreamer')."""
        pass

    @abstractmethod
    def get_executable_name(self) -> str:
        """Return the executable name for this framework."""
        pass

    @abstractmethod
    def create_command(self, **kwargs) -> tuple:
        """
        Create command line and config files for the application framework.
        
        Args:
            **kwargs: Universal parameter names and values
            
        Returns:
            Tuple of (command_string, config_dict_or_none)
        """
        pass

    @abstractmethod
    def validate_results(self, *args, **kwargs) -> bool:
        """Validate test results for the specific framework."""
        pass

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
        if self.app_path and not executable_name.startswith('/'):
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
            "fastmetadata": DEFAULT_PORT_CONFIG["fastmetadata_port"]
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
            "fastmetadata": DEFAULT_PAYLOAD_TYPE_CONFIG["fastmetadata_payload_type"]
        }
        return payload_map.get(session_type, DEFAULT_PAYLOAD_TYPE_CONFIG["st20p_payload_type"])

    def get_common_session_params(self, session_type: str) -> dict:
        """Get common session parameters used across all session types."""
        default_port = self.get_session_default_port(session_type)
        default_payload = self.get_session_default_payload_type(session_type)

        return {
            "replicas": self.universal_params.get("replicas", UNIVERSAL_PARAMS["replicas"]),
            "start_port": int(self.universal_params.get("port") if self.was_user_provided("port") else default_port),
            "payload_type": self.universal_params.get("payload_type") if self.was_user_provided("payload_type") else default_payload
        }

    def get_common_video_params(self) -> dict:
        """Get common video parameters used across video session types."""
        return {
            "width": int(self.universal_params.get("width", UNIVERSAL_PARAMS["width"])),
            "height": int(self.universal_params.get("height", UNIVERSAL_PARAMS["height"])),
            "interlaced": self.universal_params.get("interlaced", UNIVERSAL_PARAMS["interlaced"]),
            "device": self.universal_params.get("device", UNIVERSAL_PARAMS["device"]),
            "enable_rtcp": self.universal_params.get("enable_rtcp", UNIVERSAL_PARAMS["enable_rtcp"])
        }

    def execute_test(self,
                    build: str,
                    test_time: int = 30,
                    host=None,
                    tx_host=None,
                    rx_host=None,
                    input_file: str = None,
                    output_file: str = None,
                    fail_on_error: bool = True,
                    virtio_user: bool = False,
                    rx_timing_parser: bool = False,
                    ptp: bool = False,
                    capture_cfg=None,
                    sleep_interval: int = 4,
                    tx_first: bool = True,
                    output_format: str = "yuv",
                    **kwargs) -> bool:
        """
        Universal test execution method that handles all frameworks and test scenarios.
        Uses the current Application instance's commands and configuration.
        """
        # Determine if this is a dual host test
        is_dual = tx_host is not None and rx_host is not None
        framework_name = self.get_framework_name().lower()

        if is_dual:
            logger.info(f"Executing dual host {framework_name} test")
            tx_remote_host = tx_host
            rx_remote_host = rx_host
            return self._execute_dual_host_test(
                build, test_time, tx_remote_host, rx_remote_host, 
                input_file, output_file, fail_on_error, capture_cfg, 
                sleep_interval, tx_first, output_format, **kwargs
            )
        else:
            logger.info(f"Executing single host {framework_name} test")
            remote_host = host
            return self._execute_single_host_test(
                build, test_time, remote_host, input_file, output_file,
                fail_on_error, virtio_user, rx_timing_parser, ptp,
                capture_cfg, **kwargs
            )

    # -------------------------
    # Common helper utilities
    # -------------------------
    def add_timeout(self, command: str, test_time: int, grace: int = None) -> str:
        """Wrap command with timeout if test_time provided (adds a grace period)."""
        if grace is None:
            grace = self.universal_params.get("timeout_grace", 10)
        if test_time:
            if not command.strip().startswith("timeout "):
                return f"timeout {test_time + grace} {command}"
        return command

    def start_and_capture(self, command: str, build: str, test_time: int, host, process_name: str):
        """Start a single process and capture its stdout safely."""
        process = self.start_process(command, build, test_time, host)
        output = self.capture_stdout(process, process_name)
        return process, output

    def start_dual_with_delay(self, tx_command: str, rx_command: str, build: str, test_time: int,
                               tx_host, rx_host, tx_first: bool, sleep_interval: int,
                               tx_name: str, rx_name: str):
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
        if framerate_str.startswith('p') and len(framerate_str) > 1:
            num = framerate_str[1:]
        else:
            num = framerate_str
        try:
            return int(float(num))
        except ValueError:
            logger.warning(f"Could not parse framerate '{framerate_str}', defaulting to {default}")
            return default

    @abstractmethod
    def _execute_single_host_test(self, build: str, test_time: int, host, 
                                 input_file: str, output_file: str, fail_on_error: bool,
                                 virtio_user: bool, rx_timing_parser: bool, ptp: bool,
                                 capture_cfg, **kwargs) -> bool:
        """Execute single host test - implementation specific to each framework."""
        pass

    @abstractmethod
    def _execute_dual_host_test(self, build: str, test_time: int, tx_host, rx_host,
                               input_file: str, output_file: str, fail_on_error: bool,
                               capture_cfg, sleep_interval: int, tx_first: bool,
                               output_format: str, **kwargs) -> bool:
        """Execute dual host test - implementation specific to each framework."""
        pass

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
            if hasattr(process, 'stdout_text') and process.stdout_text:
                output = process.stdout_text
                logger.debug(f"{process_name} output (captured stdout_text): {output[:200]}...")
                return output
            # Local fallback (subprocess) may expose .stdout already consumed elsewhere
            if hasattr(process, 'stdout') and process.stdout:
                try:
                    # Attempt to read if it's a file-like object
                    if hasattr(process.stdout, 'read'):
                        output = process.stdout.read()
                    else:
                        output = str(process.stdout)
                    logger.debug(f"{process_name} output (captured stdout): {output[:200]}...")
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
                if 'test_' in frame.f_code.co_name:
                    return frame.f_code.co_name
                frame = frame.f_back
            return "unknown_test"
        except:
            return "unknown_test"