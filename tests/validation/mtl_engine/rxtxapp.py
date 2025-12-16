# RxTxApp Implementation for Media Transport Library
# Handles RxTxApp-specific command generation and configuration

import json
import logging
import os

from .application_base import Application
from .config.app_mappings import APP_NAME_MAP, DEFAULT_NETWORK_CONFIG
from .config.param_mappings import (
    RXTXAPP_CMDLINE_PARAM_MAP,
    RXTXAPP_CONFIG_PARAM_MAP,
)
from .config.universal_params import UNIVERSAL_PARAMS

# Create IP dictionaries for backward compatibility using DEFAULT_NETWORK_CONFIG
unicast_ip_dict = {
    "tx_interfaces": DEFAULT_NETWORK_CONFIG["unicast_tx_ip"],
    "rx_interfaces": DEFAULT_NETWORK_CONFIG["unicast_rx_ip"],
    "tx_sessions": DEFAULT_NETWORK_CONFIG["unicast_rx_ip"],
    "rx_sessions": DEFAULT_NETWORK_CONFIG["unicast_tx_ip"],
}
multicast_ip_dict = {
    "tx_interfaces": DEFAULT_NETWORK_CONFIG["multicast_tx_ip"],
    "rx_interfaces": DEFAULT_NETWORK_CONFIG["multicast_rx_ip"],
    "tx_sessions": DEFAULT_NETWORK_CONFIG["multicast_destination_ip"],
    "rx_sessions": DEFAULT_NETWORK_CONFIG["multicast_destination_ip"],
}
kernel_ip_dict = {
    "tx_sessions": "127.0.0.1",
    "rx_sessions": "127.0.0.1",
}

# Import execution utilities with fallback
try:
    import copy

    from . import rxtxapp_config as legacy_cfg
    from .execute import log_fail

    # Import legacy helpers so we can emit a backward-compatible JSON config
    from .RxTxApp import (
        add_interfaces,
        check_rx_output,
        check_tx_output,
        create_empty_config,
    )
except ImportError:
    # Fallback for direct execution (when running this module standalone)
    import copy

    import rxtxapp_config as legacy_cfg
    from execute import log_fail
    from RxTxApp import (
        add_interfaces,
        check_rx_output,
        check_tx_output,
        create_empty_config,
    )

logger = logging.getLogger(__name__)


class RxTxApp(Application):
    """RxTxApp framework implementation (unified model)."""

    def get_framework_name(self) -> str:
        return "RxTxApp"

    def get_executable_name(self) -> str:
        return APP_NAME_MAP["rxtxapp"]

    def create_command(self, **kwargs):  # type: ignore[override]
        self.set_universal_params(**kwargs)
        cmd, cfg = self._create_rxtxapp_command_and_config()
        self.command = cmd
        self.config = cfg
        # Write config immediately if path known
        config_path = (
            self.config_file_path
            or self.universal_params.get("config_file")
            or "config.json"
        )
        try:
            with open(config_path, "w") as f:
                json.dump(cfg, f, indent=2)
        except Exception as e:
            logger.warning(f"Failed to write RxTxApp config file {config_path}: {e}")
        return self.command, self.config

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
            config_file_path = os.path.abspath(
                DEFAULT_NETWORK_CONFIG["default_config_file"]
            )

        # Build command line with all command-line parameters
        executable_path = self.get_executable_path()
        cmd_parts = ["sudo", executable_path]
        cmd_parts.extend(["--config_file", config_file_path])

        # Add command-line parameters from RXTXAPP_CMDLINE_PARAM_MAP
        for universal_param, rxtx_param in RXTXAPP_CMDLINE_PARAM_MAP.items():
            # Skip test_time unless explicitly provided (for VTune tests, duration is controlled by VTune)
            if universal_param == "test_time" and not self.was_user_provided(
                "test_time"
            ):
                continue
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
        structure expected by the existing RxTxApp binary and validation helpers.
        The previous refactored flat structure caused validation failures because
        check_tx_output() and performance detection logic rely on nested lists
        like config['tx_sessions'][0]['st20p'][0].

        Returns:
            Complete RxTxApp configuration dictionary
        """
        # Currently only st20p/st22p/st30p/video/audio/ancillary/fastmetadata supported
        # We rebuild the legacy shell for all session types but only populate the active one.

        session_type = self.universal_params.get(
            "session_type", UNIVERSAL_PARAMS["session_type"]
        )
        direction = self.universal_params.get("direction")  # None means loopback
        test_mode = self.universal_params.get(
            "test_mode", UNIVERSAL_PARAMS["test_mode"]
        )

        # Determine NIC ports list (need at least 2 entries for legacy loopback template)
        nic_port = self.universal_params.get(
            "nic_port", DEFAULT_NETWORK_CONFIG["nic_port"]
        )
        nic_port_list = self.universal_params.get("nic_port_list")
        replicas = self.universal_params.get("replicas", 1)

        if not nic_port_list:
            # For single-direction (tx-only or rx-only) with replicas on same port,
            # only use one interface to avoid MTL duplicate port error
            # For loopback (direction=None), need two interfaces
            if direction in ("tx", "rx") and replicas >= 1:
                nic_port_list = [nic_port]  # Single interface for single-direction
            else:
                nic_port_list = [nic_port, nic_port]  # Duplicate for loopback
        elif len(nic_port_list) == 1:
            # Same logic: single interface for single-direction, duplicate for loopback
            if direction in ("tx", "rx") and replicas >= 1:
                pass  # Keep single element
            else:
                nic_port_list = nic_port_list * 2

        # Base legacy structure
        config = create_empty_config()
        config["tx_no_chain"] = self.universal_params.get("tx_no_chain", False)

        # Fill interface names & addressing using legacy helper
        try:
            add_interfaces(config, nic_port_list, test_mode)
        except Exception as e:
            logger.warning(
                f"Legacy add_interfaces failed ({e}); falling back to direct assignment"
            )
            # Minimal fallback assignment - handle single or dual interface configs
            config["interfaces"][0]["name"] = nic_port_list[0]
            # Set IP addresses based on test mode
            if test_mode == "unicast":
                config["interfaces"][0]["ip"] = unicast_ip_dict["tx_interfaces"]
                config["tx_sessions"][0]["dip"][0] = unicast_ip_dict["tx_sessions"]
                config["rx_sessions"][0]["ip"][0] = unicast_ip_dict["rx_sessions"]
            elif test_mode == "multicast":
                config["interfaces"][0]["ip"] = multicast_ip_dict["tx_interfaces"]
                config["tx_sessions"][0]["dip"][0] = multicast_ip_dict["tx_sessions"]
                config["rx_sessions"][0]["ip"][0] = multicast_ip_dict["rx_sessions"]
            elif test_mode == "kernel":
                config["tx_sessions"][0]["dip"][0] = kernel_ip_dict["tx_sessions"]
                config["rx_sessions"][0]["ip"][0] = kernel_ip_dict["rx_sessions"]

            if len(nic_port_list) > 1:
                config["interfaces"][1]["name"] = nic_port_list[1]
                if test_mode == "unicast":
                    config["interfaces"][1]["ip"] = unicast_ip_dict["rx_interfaces"]
                elif test_mode == "multicast":
                    config["interfaces"][1]["ip"] = multicast_ip_dict["rx_interfaces"]
            elif direction in ("tx", "rx"):
                # For single-direction single-interface, remove second interface
                if len(config["interfaces"]) > 1:
                    config["interfaces"] = [config["interfaces"][0]]

        # Fix session interface indices when using single interface
        # Template has TX on interface[0] and RX on interface[1], but with single interface both should use [0]
        if len(config["interfaces"]) == 1:
            if config["tx_sessions"] and len(config["tx_sessions"]) > 0:
                config["tx_sessions"][0]["interface"] = [0]
            if config["rx_sessions"] and len(config["rx_sessions"]) > 0:
                config["rx_sessions"][0]["interface"] = [0]

        # Override interface IPs and session IPs with user-provided source_ip/destination_ip if specified
        # This allows tests to use custom IP addressing instead of hardcoded unicast_ip_dict values
        if test_mode == "unicast":
            user_source_ip = self.universal_params.get("source_ip")
            user_dest_ip = self.universal_params.get("destination_ip")

            if direction == "tx" and len(config["interfaces"]) >= 1:
                # TX: interface IP = source_ip (local), session dip = destination_ip (remote RX)
                if user_source_ip:
                    config["interfaces"][0]["ip"] = user_source_ip
                if (
                    user_dest_ip
                    and config["tx_sessions"]
                    and len(config["tx_sessions"]) > 0
                ):
                    config["tx_sessions"][0]["dip"][0] = user_dest_ip
            elif direction == "rx" and len(config["interfaces"]) >= 1:
                # RX: interface IP = destination_ip (local bind), session ip = source_ip (filter for TX)
                if user_dest_ip:
                    config["interfaces"][0]["ip"] = user_dest_ip
                if (
                    user_source_ip
                    and config["rx_sessions"]
                    and len(config["rx_sessions"]) > 0
                ):
                    config["rx_sessions"][0]["ip"][0] = user_source_ip
            elif direction is None and len(config["interfaces"]) >= 2:
                # Loopback: TX interface uses source_ip, RX interface uses destination_ip
                if user_source_ip:
                    config["interfaces"][0]["ip"] = user_source_ip
                if user_dest_ip:
                    config["interfaces"][1]["ip"] = user_dest_ip
                if (
                    user_dest_ip
                    and config["tx_sessions"]
                    and len(config["tx_sessions"]) > 0
                ):
                    config["tx_sessions"][0]["dip"][0] = user_dest_ip
                if (
                    user_source_ip
                    and config["rx_sessions"]
                    and len(config["rx_sessions"]) > 0
                ):
                    config["rx_sessions"][0]["ip"][0] = user_source_ip

        # Helper to populate a nested session list for a given type
        def _populate_session(is_tx: bool):
            if session_type == "st20p":
                template = copy.deepcopy(
                    legacy_cfg.config_tx_st20p_session
                    if is_tx
                    else legacy_cfg.config_rx_st20p_session
                )
                # Map universal params -> legacy field names
                template["width"] = int(
                    self.universal_params.get("width", template["width"])
                )
                template["height"] = int(
                    self.universal_params.get("height", template["height"])
                )
                template["fps"] = self.universal_params.get(
                    "framerate", template["fps"]
                )
                template["pacing"] = self.universal_params.get(
                    "pacing", template["pacing"]
                )
                template["packing"] = self.universal_params.get(
                    "packing", template.get("packing", "BPM")
                )
                # pixel_format becomes input_format or output_format
                pixel_format = self.universal_params.get("pixel_format")
                if is_tx:
                    template["input_format"] = pixel_format or template.get(
                        "input_format"
                    )
                else:
                    template["output_format"] = pixel_format or template.get(
                        "output_format"
                    )
                template["transport_format"] = self.universal_params.get(
                    "transport_format", template["transport_format"]
                )
                if is_tx and self.universal_params.get("input_file"):
                    template["st20p_url"] = self.universal_params.get("input_file")
                if (not is_tx) and self.universal_params.get("output_file"):
                    template["st20p_url"] = self.universal_params.get("output_file")
                template["replicas"] = self.universal_params.get(
                    "replicas", template["replicas"]
                )
                template["start_port"] = int(
                    self.universal_params.get("port", template["start_port"])
                )
                template["payload_type"] = int(
                    self.universal_params.get("payload_type", template["payload_type"])
                )
                template["display"] = self.universal_params.get(
                    "display", template.get("display", False)
                )
                template["enable_rtcp"] = self.universal_params.get(
                    "enable_rtcp", template.get("enable_rtcp", False)
                )
                return template
            elif session_type == "st22p":
                template = copy.deepcopy(
                    legacy_cfg.config_tx_st22p_session
                    if is_tx
                    else legacy_cfg.config_rx_st22p_session
                )
                template["width"] = int(
                    self.universal_params.get("width", template["width"])
                )
                template["height"] = int(
                    self.universal_params.get("height", template["height"])
                )
                template["fps"] = self.universal_params.get(
                    "framerate", template["fps"]
                )
                template["codec"] = self.universal_params.get(
                    "codec", template["codec"]
                )  # JPEG-XS etc.
                template["quality"] = self.universal_params.get(
                    "quality", template["quality"]
                )
                template["codec_thread_count"] = self.universal_params.get(
                    "codec_threads", template["codec_thread_count"]
                )
                pf = self.universal_params.get("pixel_format")
                if is_tx:
                    template["input_format"] = pf or template.get("input_format")
                else:
                    template["output_format"] = pf or template.get("output_format")
                if is_tx and self.universal_params.get("input_file"):
                    template["st22p_url"] = self.universal_params.get("input_file")
                if (not is_tx) and self.universal_params.get("output_file"):
                    template["st22p_url"] = self.universal_params.get("output_file")
                template["replicas"] = self.universal_params.get(
                    "replicas", template["replicas"]
                )
                template["start_port"] = int(
                    self.universal_params.get("port", template["start_port"])
                )
                template["payload_type"] = int(
                    self.universal_params.get("payload_type", template["payload_type"])
                )
                template["enable_rtcp"] = self.universal_params.get(
                    "enable_rtcp", template.get("enable_rtcp", False)
                )
                return template
            elif session_type == "st30p":
                template = copy.deepcopy(
                    legacy_cfg.config_tx_st30p_session
                    if is_tx
                    else legacy_cfg.config_rx_st30p_session
                )
                template["audio_format"] = self.universal_params.get(
                    "audio_format", template["audio_format"]
                )
                template["audio_channel"] = self.universal_params.get(
                    "audio_channels", template["audio_channel"]
                )
                template["audio_sampling"] = self.universal_params.get(
                    "audio_sampling", template["audio_sampling"]
                )
                template["audio_ptime"] = self.universal_params.get(
                    "audio_ptime", template["audio_ptime"]
                )
                if is_tx and self.universal_params.get("input_file"):
                    template["audio_url"] = self.universal_params.get("input_file")
                if (not is_tx) and self.universal_params.get("output_file"):
                    template["audio_url"] = self.universal_params.get("output_file")
                template["replicas"] = self.universal_params.get(
                    "replicas", template["replicas"]
                )
                template["start_port"] = int(
                    self.universal_params.get("port", template["start_port"])
                )
                template["payload_type"] = int(
                    self.universal_params.get("payload_type", template["payload_type"])
                )
                return template

            elif session_type == "fastmetadata":
                template = copy.deepcopy(
                    legacy_cfg.config_tx_st41_session
                    if is_tx
                    else legacy_cfg.config_rx_st41_session
                )
                template["payload_type"] = int(
                    self.universal_params.get("payload_type", template["payload_type"])
                )
                template["fastmetadata_data_item_type"] = int(
                    self.universal_params.get(
                        "fastmetadata_data_item_type",
                        template["fastmetadata_data_item_type"],
                    )
                )
                template["fastmetadata_k_bit"] = int(
                    self.universal_params.get(
                        "fastmetadata_k_bit", template["fastmetadata_k_bit"]
                    )
                )
                if is_tx:
                    template["type"] = self.universal_params.get(
                        "type_mode", template["type"]
                    )
                    template["fastmetadata_fps"] = self.universal_params.get(
                        "fastmetadata_fps", template["fastmetadata_fps"]
                    )
                    template["fastmetadata_url"] = self.universal_params.get(
                        "input_file", template["fastmetadata_url"]
                    )
                else:
                    template["fastmetadata_url"] = self.universal_params.get(
                        "output_file", template.get("fastmetadata_url", "")
                    )
                template["replicas"] = self.universal_params.get(
                    "replicas", template["replicas"]
                )
                template["start_port"] = int(
                    self.universal_params.get("port", template["start_port"])
                )
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
                # Ensure non-empty video list to force functional validation instead of FPS performance path
                placeholder_video = {
                    "type": "placeholder",
                    "video_format": "",
                    "pg_format": "",
                }
                current_video_list = config["tx_sessions"][0].get("video")
                if not current_video_list:
                    config["tx_sessions"][0]["video"] = [placeholder_video]
                elif len(current_video_list) == 0:
                    current_video_list.append(placeholder_video)

        # Populate RX sessions
        if direction in (None, "rx"):
            st_entry = _populate_session(False)
            if st_entry:
                config["rx_sessions"][0].setdefault(session_type, [])
                config["rx_sessions"][0][session_type].append(st_entry)
                placeholder_video = {
                    "type": "placeholder",
                    "video_format": "",
                    "pg_format": "",
                }
                current_video_list = config["rx_sessions"][0].get("video")
                if not current_video_list:
                    config["rx_sessions"][0]["video"] = [placeholder_video]
                elif len(current_video_list) == 0:
                    current_video_list.append(placeholder_video)

        # If only TX or only RX requested, clear the other list
        if direction == "tx":
            config["rx_sessions"] = []
        elif direction == "rx":
            config["tx_sessions"] = []

        return config

    def validate_results(self) -> bool:  # type: ignore[override]
        """
        Validate execution results exactly like original RxTxApp.execute_test().

        Matches the validation pattern from mtl_engine/RxTxApp.py:
        - For st20p: Check RX output + TX/RX converter creation (NOT tx result lines)
        - For st22p: Check RX output only
        - For video/audio/etc: Check both TX and RX outputs

        Returns True if validation passes. Raises AssertionError on failure.
        """

        def _fail(msg: str):
            try:
                log_fail(msg)
            except Exception:
                logger.error(msg)
            raise AssertionError(msg)

        try:
            if not self.config:
                _fail("RxTxApp validate_results called without config")

            session_type = self._get_session_type_from_config(self.config)
            output_lines = self.last_output.split("\n") if self.last_output else []
            rc = getattr(self, "last_return_code", None)

            # 1. Check return code (must be 0 or None for dual-host secondary)
            if rc not in (0, None):
                _fail(f"Process return code {rc} indicates failure")

            # 2. Validate based on session type - match original RxTxApp.execute_test() logic
            passed = True

            if session_type == "st20p":
                # Original validation: check_rx_output + check_tx_converter_output + check_rx_converter_output
                # Note: Original does NOT check check_tx_output for st20p!
                passed = passed and check_rx_output(
                    config=self.config,
                    output=output_lines,
                    session_type="st20p",
                    fail_on_error=False,
                    host=None,
                    build=None,
                )

                # Import converter check functions if available
                try:
                    from .RxTxApp import (
                        check_rx_converter_output,
                        check_tx_converter_output,
                    )

                    passed = passed and check_tx_converter_output(
                        config=self.config,
                        output=output_lines,
                        session_type="st20p",
                        fail_on_error=False,
                        host=None,
                        build="",
                    )

                    passed = passed and check_rx_converter_output(
                        config=self.config,
                        output=output_lines,
                        session_type="st20p",
                        fail_on_error=False,
                        host=None,
                        build="",
                    )
                except ImportError:
                    # Fallback: if converter checks not available, just check RX
                    logger.warning(
                        "Converter check functions not available, using RX check only"
                    )

                if not passed:
                    _fail("st20p validation failed (RX output or converter checks)")

            elif session_type in ("st22p", "st30p", "fastmetadata"):
                # Original validation: check_rx_output only (no TX result line for st22p/st30p/fastmetadata)
                passed = check_rx_output(
                    config=self.config,
                    output=output_lines,
                    session_type=session_type,
                    fail_on_error=False,
                    host=None,
                    build=None,
                )

                if not passed:
                    _fail(f"{session_type} validation failed (RX output check)")

            elif session_type in ("video", "audio", "ancillary"):
                # Original validation: check both TX and RX outputs
                _tx_ok = check_tx_output(
                    config=self.config,
                    output=output_lines,
                    session_type=session_type,
                    fail_on_error=False,
                    host=None,
                    build=None,
                )
                _rx_ok = check_rx_output(
                    config=self.config,
                    output=output_lines,
                    session_type=session_type,
                    fail_on_error=False,
                    host=None,
                    build=None,
                )

                if not (_tx_ok and _rx_ok):
                    _fail(f"{session_type} validation failed (TX or RX output check)")

            else:
                # Unknown session type - default to checking both
                logger.warning(
                    f"Unknown session type {session_type}, using default validation"
                )
                _tx_ok = check_tx_output(
                    config=self.config,
                    output=output_lines,
                    session_type=session_type,
                    fail_on_error=False,
                    host=None,
                    build=None,
                )
                _rx_ok = check_rx_output(
                    config=self.config,
                    output=output_lines,
                    session_type=session_type,
                    fail_on_error=False,
                    host=None,
                    build=None,
                )

                if not (_tx_ok and _rx_ok):
                    _fail(f"{session_type} validation failed")

            logger.info(f"RxTxApp validation passed for {session_type}")
            return True

        except AssertionError:
            # Already handled/logged
            raise
        except Exception as e:
            _fail(f"RxTxApp validation unexpected error: {e}")

    def _get_session_type_from_config(self, config: dict) -> str:
        """Extract session type from RxTxApp config."""
        # Inspect nested lists to identify actual session type; legacy layout nests under tx_sessions[i][type]
        if not config.get("tx_sessions"):
            return "st20p"
        for tx_entry in config["tx_sessions"]:
            for possible in (
                "st22p",
                "st20p",
                "st30p",
                "fastmetadata",
                "video",
                "audio",
                "ancillary",
            ):
                if possible in tx_entry and tx_entry[possible]:
                    return possible
        return "st20p"

    def _start_netsniff_capture(self, netsniff):
        """Start netsniff capture for packet capturing during test execution.

        This method is called by execute_test() when a netsniff object is provided.
        It extracts the destination IP from the config and starts the capture.
        """
        if not self.config:
            logger.warning("No config available for netsniff capture")
            return

        try:
            # Extract destination IP from TX sessions
            if (
                self.config.get("tx_sessions")
                and len(self.config["tx_sessions"]) > 0
                and self.config["tx_sessions"][0].get("dip")
            ):
                dst_ip = self.config["tx_sessions"][0]["dip"][0]
                netsniff.update_filter(dst_ip=dst_ip)
                netsniff.capture()
                logger.info(f"Started netsniff-ng capture for destination IP {dst_ip}")
            else:
                logger.warning("Could not extract destination IP for netsniff capture")
        except Exception as e:
            logger.error(f"Failed to start netsniff capture: {e}")
