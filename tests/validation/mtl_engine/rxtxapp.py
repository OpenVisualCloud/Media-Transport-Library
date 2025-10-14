# RxTxApp Implementation for Media Transport Library
# Handles RxTxApp-specific command generation and configuration

import json
import logging
import os
import time

from .application_base import Application
from .config.universal_params import UNIVERSAL_PARAMS
from .config.param_mappings import RXTXAPP_PARAM_MAP
from .config.app_mappings import (
    APP_NAME_MAP,
    DEFAULT_NETWORK_CONFIG,
    DEFAULT_ST22P_CONFIG,
)

# Import execution utilities with fallback
try:
    from .execute import log_fail, run, is_process_running
    # Import legacy helpers so we can emit a backward-compatible JSON config
    from .RxTxApp import (
        prepare_tcpdump,
        check_tx_output,
        check_rx_output,
        create_empty_config,
        add_interfaces,
    )
    import copy
    from . import rxtxapp_config as legacy_cfg
except ImportError:
    # Fallback for direct execution (when running this module standalone)
    from execute import log_fail, run, is_process_running
    from RxTxApp import (
        prepare_tcpdump,
        check_tx_output,
        check_rx_output,
        create_empty_config,
        add_interfaces,
    )
    import copy
    import rxtxapp_config as legacy_cfg

logger = logging.getLogger(__name__)


class RxTxApp(Application):
    """RxTxApp framework implementation for MTL testing."""
    
    def get_framework_name(self) -> str:
        return "RxTxApp"
    
    def get_executable_name(self) -> str:
        return APP_NAME_MAP["rxtxapp"]
    
    def create_command(self, **kwargs) -> tuple:
        """
        Set universal parameters and create RxTxApp command line and config files.
        
        Args:
            **kwargs: Universal parameter names and values
            
        Returns:
            Tuple of (command_string, config_dict)
        """
        # Set universal parameters
        self.set_universal_params(**kwargs)
        
        # Create RxTxApp command and config
        return self._create_rxtxapp_command_and_config()
    
    def _create_rxtxapp_command_and_config(self) -> tuple:
        """
        Generate RxTxApp command line and JSON configuration from universal parameters.
        Uses config file path from constructor if provided, otherwise defaults to value from DEFAULT_NETWORK_CONFIG.

        Returns:
            Tuple of (command_string, config_dict)
        """
        # Use config file path from constructor or default (absolute path)
        if self.config_file_path:
            config_file_path = self.config_file_path
        else:
            config_file_path = os.path.abspath(DEFAULT_NETWORK_CONFIG["default_config_file"])

        # Build command line with all command-line parameters
        executable_path = self.get_executable_path()
        cmd_parts = ["sudo", executable_path]
        cmd_parts.extend(["--config_file", config_file_path])

        # Add command-line parameters from RXTXAPP_PARAM_MAP
        for universal_param, rxtx_param in RXTXAPP_PARAM_MAP.items():
            if rxtx_param.startswith("--"):  # Command-line parameter
                if universal_param in self.universal_params:
                    value = self.universal_params[universal_param]
                    if value is not None and value is not False:
                        if isinstance(value, bool) and value:
                            cmd_parts.append(rxtx_param)
                        elif not isinstance(value, bool):
                            cmd_parts.extend([rxtx_param, str(value)])

        # Create JSON configuration
        config_dict = self._create_rxtxapp_config_dict()

        return " ".join(cmd_parts), config_dict

    def _create_rxtxapp_config_dict(self) -> dict:
        """
        Build complete RxTxApp JSON config structure from universal parameters.
        Creates interfaces, sessions, and all session-specific configurations.
        This method intentionally recreates the original ("legacy") nested JSON
        structure expected by the existing RxTxApp binary and validation helpers
        (see rxtxapp_config.py). The previous refactored flat structure caused
        validation failures (e.g. could not determine FPS, exit code 244) because
        check_tx_output() and performance detection logic rely on nested lists
        like config['tx_sessions'][0]['st20p'][0].
        
        Returns:
            Complete RxTxApp configuration dictionary
        """
        # Currently only st20p/st22p/st30p/video/audio/ancillary/fastmetadata supported via
        # the refactored path. We rebuild the legacy shell for all session types but only
        # populate the active one.

        session_type = self.universal_params.get("session_type", UNIVERSAL_PARAMS["session_type"])
        direction = self.universal_params.get("direction")  # None means loopback
        test_mode = self.universal_params.get("test_mode", UNIVERSAL_PARAMS["test_mode"])

        # Determine NIC ports list (need at least 2 entries for legacy template)
        nic_port = self.universal_params.get("nic_port", DEFAULT_NETWORK_CONFIG["nic_port"])
        nic_port_list = self.universal_params.get("nic_port_list")
        if not nic_port_list:
            # Duplicate single port to satisfy legacy two-interface expectation
            nic_port_list = [nic_port, nic_port]
        elif len(nic_port_list) == 1:
            nic_port_list = nic_port_list * 2

        # Base legacy structure
        config = create_empty_config()
        config["tx_no_chain"] = self.universal_params.get("tx_no_chain", False)

        # Fill interface names & addressing using legacy helper
        try:
            add_interfaces(config, nic_port_list, test_mode)
        except Exception as e:
            logger.warning(f"Legacy add_interfaces failed ({e}); falling back to direct assignment")
            # Minimal fallback assignment
            config["interfaces"][0]["name"] = nic_port_list[0]
            config["interfaces"][1]["name"] = nic_port_list[1]

        # Helper to populate a nested session list for a given type
        def _populate_session(is_tx: bool):
            if session_type == "st20p":
                template = copy.deepcopy(
                    legacy_cfg.config_tx_st20p_session if is_tx else legacy_cfg.config_rx_st20p_session
                )
                # Map universal params -> legacy field names
                template["width"] = int(self.universal_params.get("width", template["width"]))
                template["height"] = int(self.universal_params.get("height", template["height"]))
                template["fps"] = self.universal_params.get("framerate", template["fps"])
                template["pacing"] = self.universal_params.get("pacing", template["pacing"])
                template["packing"] = self.universal_params.get("packing", template.get("packing", "BPM"))
                # pixel_format becomes input_format or output_format
                pixel_format = self.universal_params.get("pixel_format")
                if is_tx:
                    template["input_format"] = pixel_format or template.get("input_format")
                else:
                    template["output_format"] = pixel_format or template.get("output_format")
                template["transport_format"] = self.universal_params.get("transport_format", template["transport_format"])
                if is_tx and self.universal_params.get("input_file"):
                    template["st20p_url"] = self.universal_params.get("input_file")
                if (not is_tx) and self.universal_params.get("output_file"):
                    template["st20p_url"] = self.universal_params.get("output_file")
                template["replicas"] = self.universal_params.get("replicas", template["replicas"])
                template["start_port"] = int(self.universal_params.get("port", template["start_port"]))
                template["payload_type"] = int(self.universal_params.get("payload_type", template["payload_type"]))
                template["display"] = self.universal_params.get("display", template.get("display", False))
                template["enable_rtcp"] = self.universal_params.get("enable_rtcp", template.get("enable_rtcp", False))
                return template
            elif session_type == "st22p":
                template = copy.deepcopy(
                    legacy_cfg.config_tx_st22p_session if is_tx else legacy_cfg.config_rx_st22p_session
                )
                template["width"] = int(self.universal_params.get("width", template["width"]))
                template["height"] = int(self.universal_params.get("height", template["height"]))
                template["fps"] = self.universal_params.get("framerate", template["fps"])
                template["codec"] = self.universal_params.get("codec", template["codec"])  # JPEG-XS etc.
                template["quality"] = self.universal_params.get("quality", template["quality"])
                template["codec_thread_count"] = self.universal_params.get("codec_threads", template["codec_thread_count"])
                pf = self.universal_params.get("pixel_format")
                if is_tx:
                    template["input_format"] = pf or template.get("input_format")
                else:
                    template["output_format"] = pf or template.get("output_format")
                if is_tx and self.universal_params.get("input_file"):
                    template["st22p_url"] = self.universal_params.get("input_file")
                if (not is_tx) and self.universal_params.get("output_file"):
                    template["st22p_url"] = self.universal_params.get("output_file")
                template["replicas"] = self.universal_params.get("replicas", template["replicas"])
                template["start_port"] = int(self.universal_params.get("port", template["start_port"]))
                template["payload_type"] = int(self.universal_params.get("payload_type", template["payload_type"]))
                template["enable_rtcp"] = self.universal_params.get("enable_rtcp", template.get("enable_rtcp", False))
                return template
            elif session_type == "st30p":
                template = copy.deepcopy(
                    legacy_cfg.config_tx_st30p_session if is_tx else legacy_cfg.config_rx_st30p_session
                )
                template["audio_format"] = self.universal_params.get("audio_format", template["audio_format"])
                template["audio_channel"] = self.universal_params.get("audio_channels", template["audio_channel"])
                template["audio_sampling"] = self.universal_params.get("audio_sampling", template["audio_sampling"])
                template["audio_ptime"] = self.universal_params.get("audio_ptime", template["audio_ptime"])
                if is_tx and self.universal_params.get("input_file"):
                    template["audio_url"] = self.universal_params.get("input_file")
                template["replicas"] = self.universal_params.get("replicas", template["replicas"])
                template["start_port"] = int(self.universal_params.get("port", template["start_port"]))
                template["payload_type"] = int(self.universal_params.get("payload_type", template["payload_type"]))
                return template
            else:
                # Fallback: reuse st20p layout for unknown session types (minimal support)
                template = {"replicas": 1}
                return template

        # Populate TX sessions
        if direction in (None, "tx"):
            st_entry = _populate_session(True)
            if st_entry:
                config["tx_sessions"][0].setdefault(session_type, [])
                config["tx_sessions"][0][session_type].append(st_entry)
                # Add a dummy video list so legacy performance heuristic (which checks absence of video list)
                # does not misclassify this regular functional test as a performance test.
                if "video" not in config["tx_sessions"][0]:
                    config["tx_sessions"][0]["video"] = []

        # Populate RX sessions
        if direction in (None, "rx"):
            st_entry = _populate_session(False)
            if st_entry:
                config["rx_sessions"][0].setdefault(session_type, [])
                config["rx_sessions"][0][session_type].append(st_entry)
                if "video" not in config["rx_sessions"][0]:
                    config["rx_sessions"][0]["video"] = []

        # If only TX or only RX requested, clear the other list to avoid confusing validators
        if direction == "tx":
            config["rx_sessions"] = []
        elif direction == "rx":
            config["tx_sessions"] = []

        return config

    def _create_session_data(self, session_type: str, is_tx: bool) -> dict:
        """
        Factory method to create session data for different session types.
        Routes to specific session data creation methods based on type.

        Args:
            session_type: Type of session (st20p, st22p, st30p, video, audio, ancillary, fastmetadata)
            is_tx: True for TX session, False for RX session

        Returns:
            Session data dictionary
        """
        if session_type == "st20p":
            return self._create_st20p_session_data(is_tx)
        elif session_type == "st22p":
            return self._create_st22p_session_data(is_tx)
        elif session_type == "st30p":
            return self._create_st30p_session_data(is_tx)
        elif session_type == "video":
            return self._create_video_session_data(is_tx)
        elif session_type == "audio":
            return self._create_audio_session_data(is_tx)
        elif session_type == "ancillary":
            return self._create_ancillary_session_data(is_tx)
        elif session_type == "fastmetadata":
            return self._create_fastmetadata_session_data(is_tx)
        else:
            raise ValueError(f"Unsupported session type: {session_type}")

    def _add_tx_rx_specific_params(self, session: dict, session_type: str, is_tx: bool):
        """Add TX/RX specific parameters to session."""
        if is_tx:
            session["ip"] = self.universal_params.get("destination_ip", DEFAULT_NETWORK_CONFIG["unicast_rx_ip"])
            session["type"] = "frame"
            if self.universal_params.get("input_file"):
                session["st20p_url"] = self.universal_params["input_file"]
        else:
            session["ip"] = self.universal_params.get("destination_ip", DEFAULT_NETWORK_CONFIG["unicast_rx_ip"])
            session["type"] = "frame"
            if self.universal_params.get("output_file"):
                session["st20p_url"] = self.universal_params["output_file"]

    def _create_st20p_session_data(self, is_tx: bool) -> dict:
        """Create ST20p (uncompressed video) session data from universal parameters."""
        session = self.get_common_session_params("st20p")
        session.update(self.get_common_video_params())
        session.update({
            "fps": self.universal_params.get("framerate", UNIVERSAL_PARAMS["framerate"]),
            "pacing": self.universal_params.get("pacing", UNIVERSAL_PARAMS["pacing"]),
            "packing": self.universal_params.get("packing", UNIVERSAL_PARAMS["packing"]),
            "transport_format": self.universal_params.get("transport_format", UNIVERSAL_PARAMS["transport_format"]),
            "display": self.universal_params.get("display", UNIVERSAL_PARAMS["display"])
        })

        self._add_tx_rx_specific_params(session, "st20p", is_tx)
        return session

    def _create_st22p_session_data(self, is_tx: bool) -> dict:
        """Create ST22p (compressed video with JPEG-XS) session data from universal parameters."""
        session = self.get_common_session_params("st22p")
        session.update(self.get_common_video_params())
        session.update({
            "fps": self.universal_params.get("framerate", DEFAULT_ST22P_CONFIG["framerate"]),
            "pack_type": DEFAULT_ST22P_CONFIG["pack_type"],
            "codec": self.universal_params.get("codec", DEFAULT_ST22P_CONFIG["codec"]),
            "quality": self.universal_params.get("quality", DEFAULT_ST22P_CONFIG["quality"]),
            "codec_thread_count": self.universal_params.get("codec_threads", DEFAULT_ST22P_CONFIG["codec_threads"])
        })

        self._add_tx_rx_specific_params(session, "st22p", is_tx)
        return session

    def _create_st30p_session_data(self, is_tx: bool) -> dict:
        """Create ST30p (uncompressed audio) session data from universal parameters."""
        session = self.get_common_session_params("st30p")
        session.update({
            "audio_format": self.universal_params.get("audio_format", UNIVERSAL_PARAMS["audio_format"]),
            "audio_channel": self.universal_params.get("audio_channels", UNIVERSAL_PARAMS["audio_channels"]),
            "audio_sampling": self.universal_params.get("audio_sampling", UNIVERSAL_PARAMS["audio_sampling"]),
            "audio_ptime": self.universal_params.get("audio_ptime", UNIVERSAL_PARAMS["audio_ptime"]),
            "audio_url": self.universal_params.get("input_file" if is_tx else "output_file", "")
        })

        return session

    def _create_video_session_data(self, is_tx: bool) -> dict:
        """Create raw video session data from universal parameters."""
        session = self.get_common_session_params("video")
        session.update(self.get_common_video_params())
        session.update({
            "fps": self.universal_params.get("framerate", UNIVERSAL_PARAMS["framerate"]),
            "transport_format": self.universal_params.get("transport_format", UNIVERSAL_PARAMS["transport_format"])
        })
        
        self._add_tx_rx_specific_params(session, "video", is_tx)
        return session

    def _create_audio_session_data(self, is_tx: bool) -> dict:
        """Create audio session data from universal parameters."""
        session = self.get_common_session_params("audio")
        session.update({
            "audio_format": self.universal_params.get("audio_format", UNIVERSAL_PARAMS["audio_format"]),
            "audio_channel": self.universal_params.get("audio_channels", UNIVERSAL_PARAMS["audio_channels"]),
            "audio_sampling": self.universal_params.get("audio_sampling", UNIVERSAL_PARAMS["audio_sampling"])
        })
        
        return session

    def _create_ancillary_session_data(self, is_tx: bool) -> dict:
        """Create ancillary data session data from universal parameters."""
        session = self.get_common_session_params("ancillary")
        session.update({
            "ancillary_format": self.universal_params.get("transport_format", "SMPTE_291M"),
            "ancillary_fps": self.universal_params.get("framerate", UNIVERSAL_PARAMS["framerate"])
        })
        
        return session

    def _create_fastmetadata_session_data(self, is_tx: bool) -> dict:
        """Create fast metadata session data from universal parameters."""
        session = self.get_common_session_params("fastmetadata")
        session.update({
            "metadata_format": "SMPTE_2110_41",
            "metadata_fps": self.universal_params.get("framerate", UNIVERSAL_PARAMS["framerate"])
        })
        
        return session

    def validate_results(self, config: dict, tx_output: str, rx_output: str,
                        fail_on_error: bool, host, build: str) -> bool:
        """Validate RxTxApp test results."""
        try:
            # Get session type from config for proper validation
            session_type = self._get_session_type_from_config(config)
            
            # Validate TX results
            tx_result = check_tx_output(
                config=config, 
                output=tx_output.split('\n') if tx_output else [],
                session_type=session_type, 
                fail_on_error=fail_on_error,
                host=host,
                build=build
            )
            if not tx_result and fail_on_error:
                log_fail(f"TX validation failed for {session_type}")
                return False
                
            # Validate RX results  
            rx_result = check_rx_output(
                config=config,
                output=rx_output.split('\n') if rx_output else [],
                session_type=session_type,
                fail_on_error=fail_on_error,
                host=host,
                build=build
            )
            if not rx_result and fail_on_error:
                log_fail(f"RX validation failed for {session_type}")
                return False
                
            return True
            
        except Exception as e:
            logger.error(f"Error validating RxTxApp results: {e}")
            return not fail_on_error

    def _execute_single_host_test(self, build: str, test_time: int, host, 
                                 input_file: str, output_file: str, fail_on_error: bool,
                                 virtio_user: bool, rx_timing_parser: bool, ptp: bool,
                                 capture_cfg, **kwargs) -> bool:
        """Execute single host RxTxApp test."""
        # Add test time to kwargs before creating command
        if test_time:
            kwargs["test_time"] = test_time
        
        command, config = self.create_command(**kwargs)
        
        # Add test-specific parameters
        if virtio_user:
            command += " --virtio_user"
        if rx_timing_parser:
            command += " --rx_timing_parser"
        if ptp:
            command += " --ptp"
            
        # Write config file
        config_path = self.config_file_path or "config.json"
        with open(config_path, 'w') as f:
            json.dump(config, f, indent=2)
            
        # Setup capture if requested
        if capture_cfg:
            prepare_tcpdump(capture_cfg, host)
            
        # Execute test
        process = self.start_process(command, build, test_time, host)
        output = self.capture_stdout(process, "RxTxApp")
        
        # Validate results
        return self.validate_results(config, output, output, True, host, build)

    def _execute_dual_host_test(self, build: str, test_time: int, tx_host, rx_host,
                               input_file: str, output_file: str, fail_on_error: bool,
                               capture_cfg, sleep_interval: int, tx_first: bool,
                               output_format: str, **kwargs) -> bool:
        """Execute dual host RxTxApp test."""
        # Create TX and RX configurations
        tx_kwargs = kwargs.copy()
        tx_kwargs["direction"] = "tx"
        if test_time:
            tx_kwargs["test_time"] = test_time
        if input_file:
            tx_kwargs["input_file"] = input_file
            
        rx_kwargs = kwargs.copy() 
        rx_kwargs["direction"] = "rx"
        if test_time:
            rx_kwargs["test_time"] = test_time
        if output_file:
            rx_kwargs["output_file"] = output_file
            
        tx_command, tx_config = self.create_command(**tx_kwargs)
        rx_command, rx_config = self.create_command(**rx_kwargs)
        
        # Write config files
        tx_config_path = "tx_config.json"
        rx_config_path = "rx_config.json"
        
        with open(tx_config_path, 'w') as f:
            json.dump(tx_config, f, indent=2)
        with open(rx_config_path, 'w') as f:
            json.dump(rx_config, f, indent=2)
            
        # Setup capture if requested
        if capture_cfg:
            prepare_tcpdump(capture_cfg, rx_host, build)
            
        # Start processes based on tx_first parameter
        if tx_first:
            tx_process = self.start_process(tx_command.replace("config.json", tx_config_path), build, test_time, tx_host)
            time.sleep(sleep_interval)
            rx_process = self.start_process(rx_command.replace("config.json", rx_config_path), build, test_time, rx_host)
        else:
            rx_process = self.start_process(rx_command.replace("config.json", rx_config_path), build, test_time, rx_host)
            time.sleep(sleep_interval)
            tx_process = self.start_process(tx_command.replace("config.json", tx_config_path), build, test_time, tx_host)
        
        # Capture outputs
        tx_output = self.capture_stdout(tx_process, "RxTxApp-TX")
        rx_output = self.capture_stdout(rx_process, "RxTxApp-RX")
        
        # Validate results
        tx_result = self.validate_results(tx_config, tx_output, "", True, tx_host, build)
        rx_result = self.validate_results(rx_config, "", rx_output, True, rx_host, build)
        return tx_result and rx_result

    def _import_with_fallback(self, module_name: str, import_items: list):
        """Import utilities with fallback for direct execution."""
        try:
            if module_name == "RxTxApp":
                globals().update({item: getattr(__import__(f".{module_name}", fromlist=import_items, level=1), item) for item in import_items})
        except ImportError:
            globals().update({item: getattr(__import__(module_name, fromlist=import_items), item) for item in import_items})

    def _get_session_type_from_config(self, config: dict) -> str:
        """Extract session type from RxTxApp config."""
        if config.get("tx_sessions"):
            # Check for specific session type indicators
            session = config["tx_sessions"][0]
            if "fps" in session and "transport_format" in session:
                return "st20p"
            elif "codec" in session:
                return "st22p" 
            elif "audio_format" in session:
                return "st30p"
        return "st20p"  # Default