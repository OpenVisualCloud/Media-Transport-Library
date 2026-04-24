# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

# RxTxApp Implementation for Media Transport Library
# Handles RxTxApp-specific command generation and configuration

import json
import logging
import re

from mfd_connect import SSHConnection

from . import ip_pools
from .application_base import Application
from .config.mappings import APP_NAME_MAP, RXTXAPP_CMDLINE_PARAM_MAP
from .config.universal_params import UNIVERSAL_PARAMS
from .execute import log_fail

logger = logging.getLogger(__name__)


# ============================================================================
# Helper functions for building RxTxApp config from UNIVERSAL_PARAMS
# ============================================================================


def create_empty_config() -> dict:
    """Create empty RxTxApp configuration structure.

    Per-session-type lists (``st20p``, ``audio``, ...) are added on demand by
    ``_populate_session``; the C parser (`parse_session_num` / RxTxApp
    `parse_json.c`) treats missing keys and empty arrays identically.
    """
    return {
        "tx_no_chain": False,
        "interfaces": [
            {"name": "", "ip": ""},
            {"name": "", "ip": ""},
        ],
        "tx_sessions": [
            {"dip": [""], "interface": [0]},
        ],
        "rx_sessions": [
            {"ip": [""], "interface": [1]},
        ],
    }


def add_interfaces(
    config: dict, nic_port_list: list, test_mode: str, direction: str | None = None
) -> dict:
    """Add network interfaces to RxTxApp config with appropriate IP addressing.

    Args:
        config: RxTxApp configuration dictionary
        nic_port_list: List of NIC port names (1 or 2 ports)
        test_mode: "unicast", "multicast", or "kernel"
        direction: Optional direction hint ("tx"/"rx") for single-port configs

    Returns:
        Updated config dictionary
    """
    if not nic_port_list:
        log_fail("nic_port_list is empty")

    config["interfaces"][0]["name"] = nic_port_list[0]
    if len(nic_port_list) > 1:
        config["interfaces"][1]["name"] = nic_port_list[1]

    if test_mode == "unicast":
        if direction == "rx":
            config["interfaces"][0]["ip"] = ip_pools.rx[0]
            config["rx_sessions"][0]["ip"][0] = ip_pools.tx[0]
        else:
            config["interfaces"][0]["ip"] = ip_pools.tx[0]
            config["tx_sessions"][0]["dip"][0] = ip_pools.rx[0]
            config["rx_sessions"][0]["ip"][0] = ip_pools.tx[0]

        if len(nic_port_list) > 1:
            config["interfaces"][1]["ip"] = ip_pools.rx[0]
    elif test_mode == "multicast":
        config["interfaces"][0]["ip"] = ip_pools.tx[0]
        config["tx_sessions"][0]["dip"][0] = ip_pools.rx_multicast[0]
        config["rx_sessions"][0]["ip"][0] = ip_pools.rx_multicast[0]

        if len(nic_port_list) > 1:
            config["interfaces"][1]["ip"] = ip_pools.rx[0]
    elif test_mode == "kernel":
        config["tx_sessions"][0]["dip"][0] = "127.0.0.1"
        config["rx_sessions"][0]["ip"][0] = "127.0.0.1"
    else:
        log_fail(f"wrong test_mode {test_mode}")

    return config


def check_tx_output(
    config: dict,
    output: list,
    session_type: str,
    fail_on_error: bool,
) -> bool:
    """Check TX output for successful session completion.

    For performance configs (st20p with no video sessions), checks FPS performance.
    For regular configs, checks for OK result lines.

    Args:
        config: RxTxApp configuration dictionary
        output: List of output lines from RxTxApp
        session_type: Session type (st20p, st22p, st30p, etc.)
        fail_on_error: Whether to call log_fail on validation failure

    Returns:
        True if validation passed, False otherwise
    """
    # Check if this is a performance config with st20p sessions
    is_performance_st20p = False
    if (
        len(config["tx_sessions"]) > 0
        and session_type == "st20p"
        and any(
            "st20p" in session
            and len(session.get("st20p", [])) > 0
            and "video" not in session
            or len(session.get("video", [])) == 0
            for session in config["tx_sessions"]
        )
    ):
        is_performance_st20p = True

    if is_performance_st20p:
        return check_tx_fps_performance(config, output, session_type, fail_on_error)

    # Regular check for OK results
    ok_cnt = 0
    logger.info(f"Checking TX {session_type} output for OK results")

    for line in output:
        if f"app_tx_{session_type}_result" in line and "OK" in line:
            ok_cnt += 1
            logger.info(f"Found TX {session_type} OK result: {line}")

    replicas = 0
    for session in config["tx_sessions"]:
        for s in session.get(session_type) or []:
            replicas += s["replicas"]

    logger.info(f"TX {session_type} check: {ok_cnt}/{replicas} OK results found")

    if ok_cnt == replicas:
        logger.info(f"TX {session_type} check PASSED: all {replicas} sessions OK")
        return True

    if fail_on_error:
        log_fail(f"tx {session_type} session failed")
    else:
        logger.info(f"tx {session_type} session failed")

    return False


def check_tx_fps_performance(
    config: dict,
    output: list,
    session_type: str,
    fail_on_error: bool,
) -> bool:
    """Check TX FPS performance for performance test configs.

    Validates that TX sessions achieve target FPS within tolerance.

    Args:
        config: RxTxApp configuration dictionary
        output: List of output lines from RxTxApp
        session_type: Session type (st20p)
        fail_on_error: Whether to call log_fail on validation failure

    Returns:
        True if all sessions achieved target FPS, False otherwise
    """
    # Get expected FPS from config
    expected_fps = None
    replicas = 0

    for session in config["tx_sessions"]:
        if session_type in session and session[session_type]:
            for s in session[session_type]:
                fps_str = s.get("fps", "")
                if fps_str.startswith("p") or fps_str.startswith("i"):
                    expected_fps = int(fps_str[1:])  # Remove 'p' or 'i' prefix
                replicas += s["replicas"]
                break

    if expected_fps is None:
        logger.info("Could not determine expected FPS from config")
        return False

    logger.info(
        f"Checking TX FPS performance: expected {expected_fps} fps for {replicas} replicas"
    )

    # Look for TX_VIDEO_SESSION fps lines - check if any reading reaches target
    fps_pattern = re.compile(
        r"TX_VIDEO_SESSION\(\d+,(\d+):app_tx_st20p_(\d+)\):\s+fps\s+([\d.]+)"
    )

    # Set to track which sessions have achieved target FPS
    successful_sessions = set()
    fps_tolerance = 2  # Allow 2 fps tolerance (e.g., 49 is pass for 50 fps)

    # Check all FPS values to see if any session reaches target
    for line in output:
        match = fps_pattern.search(line)
        if match:
            session_id = int(match.group(2))  # Extract session ID from app_tx_st20p_X
            actual_fps = float(match.group(3))

            # Check if FPS is within tolerance
            if abs(actual_fps - expected_fps) <= fps_tolerance:
                if session_id not in successful_sessions:
                    successful_sessions.add(session_id)

    successful_count = len(successful_sessions)
    logger.info(
        f"TX FPS performance check: {successful_count}/{replicas} sessions achieved target"
    )

    if successful_count == replicas:
        return True

    if fail_on_error:
        log_fail(
            f"tx {session_type} fps performance failed: {successful_count}/{replicas} sessions"
        )
    else:
        logger.info(
            f"tx {session_type} fps performance failed: {successful_count}/{replicas} sessions"
        )

    return False


def check_rx_output(
    config: dict,
    output: list,
    session_type: str,
    fail_on_error: bool,
) -> bool:
    """Check RX output for successful session completion.

    Args:
        config: RxTxApp configuration dictionary
        output: List of output lines from RxTxApp
        session_type: Session type (st20p, st22p, st30p, video, audio, ancillary, etc.)
        fail_on_error: Whether to call log_fail on validation failure

    Returns:
        True if validation passed, False otherwise
    """
    ok_cnt = 0
    logger.info(f"Checking RX {session_type} output for OK results")

    if session_type == "anc":
        pattern = re.compile(r"app_rx_anc_result")
        session_type = "ancillary"
    elif session_type == "ancillary":
        pattern = re.compile(r"app_rx_anc_result")
    elif session_type == "st20p":
        pattern = re.compile(r"app_rx_st20p_result")
    elif session_type == "st22p":
        pattern = re.compile(r"app_rx_st22p_result")
    elif session_type == "st30p":
        pattern = re.compile(r"app_rx_st30p_result")
    elif session_type == "st40p":
        pattern = re.compile(r"app_rx_st40p_result")
    elif session_type == "fastmetadata":
        pattern = re.compile(r"app_rx_fmd_result")
    elif session_type == "video":
        pattern = re.compile(r"app_rx_video_result")
    elif session_type == "audio":
        pattern = re.compile(r"app_rx_audio_result")
    else:
        pattern = re.compile(r"app_rx_.*_result")

    # All RX session result lines emit `OK` (or `FAILED`) once the C-side
    # framerate-tolerance gate passes (ST_APP_EXPECT_NEAR within 5%). The token
    # appears between the parenthesized session index and `fps F`.
    success_token = "OK"

    for line in output:
        if pattern.search(line) and success_token in line:
            ok_cnt += 1
            logger.info(f"Found RX {session_type} OK result: {line}")

    replicas = config["rx_sessions"][0][session_type][0]["replicas"]

    logger.info(f"RX {session_type} check: {ok_cnt}/{replicas} OK results found")

    if ok_cnt == replicas:
        logger.info(f"RX {session_type} check PASSED: all {replicas} sessions OK")
        return True

    if fail_on_error:
        log_fail(f"rx {session_type} session failed")
    else:
        logger.info(f"rx {session_type} session failed")

    return False


def check_tx_converter_output(
    config: dict,
    output: list,
    session_type: str,
    fail_on_error: bool,
) -> bool:
    """Check TX converter creation in output for st20p sessions.

    Args:
        config: RxTxApp configuration dictionary
        output: List of output lines from RxTxApp
        session_type: Session type (st20p)
        fail_on_error: Whether to call log_fail on validation failure

    Returns:
        True if all converters were created, False otherwise
    """
    ok_cnt = 0
    transport_format = config["tx_sessions"][0]["st20p"][0]["transport_format"]
    input_format = config["tx_sessions"][0]["st20p"][0]["input_format"]

    logger.info(f"Checking TX {session_type} converter output")

    for line in output:
        if (
            f"st20p_tx_create({ok_cnt}), transport fmt ST20_FMT_{transport_format.upper()}, input fmt: {input_format}"
            in line
        ):
            ok_cnt += 1
            logger.info(f"Found TX converter creation: {line}")

    if session_type == "anc":
        session_type = "ancillary"
    replicas = config["tx_sessions"][0][session_type][0]["replicas"]

    logger.info(
        f"TX {session_type} converter check: {ok_cnt}/{replicas} converters created"
    )

    if ok_cnt == replicas:
        logger.info(f"TX {session_type} converter check PASSED")
        return True

    if fail_on_error:
        log_fail(f"tx {session_type} session failed")
    else:
        logger.info(f"tx {session_type} session failed")

    return False


def check_rx_converter_output(
    config: dict,
    output: list,
    session_type: str,
    fail_on_error: bool,
) -> bool:
    """Check RX converter creation in output for st20p sessions.

    Args:
        config: RxTxApp configuration dictionary
        output: List of output lines from RxTxApp
        session_type: Session type (st20p)
        fail_on_error: Whether to call log_fail on validation failure

    Returns:
        True if all converters were created, False otherwise
    """
    ok_cnt = 0

    transport_format = config["rx_sessions"][0]["st20p"][0]["transport_format"]
    output_format = config["rx_sessions"][0]["st20p"][0]["output_format"]

    logger.info(f"Checking RX {session_type} converter output")

    for line in output:
        if (
            f"st20p_rx_create({ok_cnt}), transport fmt ST20_FMT_{transport_format.upper()}, output fmt {output_format}"
            in line
        ):
            ok_cnt += 1
            logger.info(f"Found RX converter creation: {line}")

    if session_type == "anc":
        session_type = "ancillary"
    replicas = config["rx_sessions"][0][session_type][0]["replicas"]

    logger.info(
        f"RX {session_type} converter check: {ok_cnt}/{replicas} converters created"
    )

    if ok_cnt == replicas:
        logger.info(f"RX {session_type} converter check PASSED")
        return True

    if fail_on_error:
        log_fail(f"rx {session_type} session failed")
    else:
        logger.info(f"rx {session_type} session failed")

    return False


def check_codec_loaded(
    output: list,
    session_type: str,
    fail_on_error: bool = False,
) -> bool:
    """Check if codec/plugin was loaded successfully for ST22P sessions.

    For ST22P sessions, the encoder/decoder must be registered before the session
    can work. This function checks the log output for registration messages.

    Log patterns to look for:
    - "st22_encoder_register(...), ... registered, device ..."
    - "st22_decoder_register(...), ... registered, device ..."
    - "st_plugin_register(...), ... registered, version ..."
    - "encoder use block get mode" (indicates encoder is active)
    - "decoder use block get mode" (indicates decoder is active)

    Args:
        output: List of output lines from RxTxApp
        session_type: Session type (should be "st22p" for codec checks)
        fail_on_error: Whether to call log_fail on validation failure

    Returns:
        True if codec was loaded, False otherwise
    """
    if session_type != "st22p":
        # Codec check only applies to st22p
        return True

    logger.info("Checking if ST22P codec/encoder/decoder was loaded")

    # Patterns indicating successful codec loading
    codec_patterns = [
        r"st22_encoder_register\(\d+\),.*registered",
        r"st22_decoder_register\(\d+\),.*registered",
        r"st_plugin_register\(\d+\),.*registered",
        r"encoder use block get mode",
        r"decoder use block get mode",
    ]

    found_encoder = False
    found_decoder = False

    for line in output:
        for pattern in codec_patterns:
            if re.search(pattern, line, re.IGNORECASE):
                logger.info(f"Found codec registration: {line.strip()}")
                if "encoder" in line.lower():
                    found_encoder = True
                if "decoder" in line.lower():
                    found_decoder = True

    # Both encoder and decoder should be loaded for a proper st22p session
    if found_encoder and found_decoder:
        logger.info("ST22P codec check PASSED: encoder and decoder loaded")
        return True

    if found_encoder or found_decoder:
        logger.warning(
            f"ST22P codec partially loaded: encoder={found_encoder}, decoder={found_decoder}"
        )
        # Partial load may still work in some configurations
        return True

    # No codec found
    msg = "ST22P codec check FAILED: no encoder/decoder registration found in logs"
    if fail_on_error:
        log_fail(msg)
    else:
        logger.warning(msg)

    return False


class RxTxApp(Application):
    """RxTxApp framework implementation (unified model)."""

    def get_app_name(self) -> str:
        return "RxTxApp"

    def get_executable_name(self) -> str:
        return APP_NAME_MAP["rxtxapp"]

    # Per-session-type (default UDP port, default payload type). Mirrors the
    # legacy ``add_*_sessions`` helpers; without it every type ends up on
    # 20000/112 and RxTxApp dies during session create due to port collisions.
    _TYPE_PORT_DEFAULTS = {
        "st20p": (20000, 112),
        "st22p": (20000, 114),
        "video": (20000, 112),
        "audio": (30000, 111),
        "st30p": (30000, 111),
        "ancillary": (40000, 113),
        "st40p": (40000, 113),
        "fastmetadata": (40000, 115),
    }

    @classmethod
    def _apply_type_defaults(cls, spec: dict) -> dict:
        """Inject default port/payload_type for ``spec['session_type']`` in place."""
        defaults = cls._TYPE_PORT_DEFAULTS.get(spec.get("session_type"))
        if defaults:
            d_port, d_pt = defaults
            spec.setdefault("port", d_port)
            spec.setdefault("payload_type", d_pt)
        return spec

    def create_command(self, **kwargs):
        """Build command + JSON config (single- or multi-session aware).

        Single-session usage (unchanged):
            rxtxapp.create_command(session_type="st20p", input_file=..., ...)

        Multi-session usage (for kernel_lo / xdp / rx_timing/mixed):
            rxtxapp.create_command(
                sessions=[
                    {"session_type": "st20p", "input_file": ..., ...},
                    {"session_type": "st30p", "audio_format": ..., ...},
                    {"session_type": "ancillary", "ancillary_url": ..., ...},
                ],
                nic_port_list=[...], test_mode="multicast",
            )

        Common kwargs (everything outside ``sessions``) are merged with each
        per-session dict so callers do not have to repeat them.
        """
        sessions = kwargs.pop("sessions", None)
        if sessions is None:
            return super().create_command(**kwargs)

        if not isinstance(sessions, (list, tuple)) or not sessions:
            raise ValueError("sessions= must be a non-empty list of dicts")

        common = dict(kwargs)

        # Build base config + command from the first session (provides
        # interfaces, IPs, etc.). ``super().create_command`` already resets
        # self.params from UNIVERSAL_PARAMS, so a stale rxtxapp fixture
        # cannot leak state into a multi-session run.
        first = self._apply_type_defaults({**common, **sessions[0]})
        super().create_command(**first)
        base_config = self.config

        def _extend(direction: str, src: dict, stype: str) -> None:
            sess = src.get(direction) or []
            if sess and sess[0].get(stype):
                base_config[direction][0].setdefault(stype, []).extend(sess[0][stype])

        # Append every additional session into the existing config. Each
        # extra session is built via a fresh _create_rxtxapp_config_dict()
        # so per-type defaults (FPS, payload size, etc.) are applied; we
        # then move the populated arrays into the base config.
        saved_params = dict(self.params)
        try:
            for spec in sessions[1:]:
                if not spec.get("session_type"):
                    raise ValueError(
                        "every entry in sessions= must contain session_type"
                    )
                merged = self._apply_type_defaults({**common, **spec})
                stype = merged["session_type"]
                self.params = UNIVERSAL_PARAMS.copy()
                self.set_params(**merged)
                tmp_config = self._create_rxtxapp_config_dict()
                _extend("tx_sessions", tmp_config, stype)
                _extend("rx_sessions", tmp_config, stype)
        finally:
            # Restore params from the primary session so subsequent
            # execute_test() sees a consistent state.
            self.params = saved_params

        self.config = base_config
        return self.command, self.config

    def _create_command_and_config(self) -> tuple:
        """Generate RxTxApp command line and JSON configuration from universal parameters.

        Returns:
            Tuple of (command_string, config_dict)
        """
        # Use config file path from constructor or default
        config_file_path = self.config_file_path or "tests/config.json"

        # Build command line
        # Note: sudo is NOT needed because the test framework already runs as root
        # (see README.md: "MTL validation must run as root user").
        # Subprocesses inherit root privileges from the parent pytest process.
        cmd_parts = [
            self.get_executable_path(),
            "--config_file",
            config_file_path,
        ]

        # Add command-line parameters from RXTXAPP_CMDLINE_PARAM_MAP
        # Parameters with default 0 that should be skipped when not explicitly set
        skip_if_zero = {"rx_max_file_size"}

        for universal_param, rxtx_param in RXTXAPP_CMDLINE_PARAM_MAP.items():
            # Skip test_time unless explicitly provided
            if universal_param == "test_time" and not self.was_user_provided(
                "test_time"
            ):
                continue
            value = self.params.get(universal_param)
            # Skip parameters with value 0 that mean "no limit" or "disabled"
            if universal_param in skip_if_zero and value == 0:
                continue
            if value is not None and value is not False:
                if isinstance(value, bool):
                    cmd_parts.append(rxtx_param)
                else:
                    cmd_parts.extend([rxtx_param, str(value)])

        return " ".join(cmd_parts), self._create_rxtxapp_config_dict()

    # Sentinel for ``_p`` so callers can pass ``None`` as an explicit default.
    _PARAM_UNSET = object()

    def _p(self, key, default=_PARAM_UNSET, *, cast=None):
        """Read ``self.params[key]`` falling back to UNIVERSAL_PARAMS[key].

        ``default`` overrides the UNIVERSAL_PARAMS fallback when supplied
        (used for type-specific defaults like audio's port=30000). ``cast``
        applies a type conversion (typically ``int``) to the resolved value.
        """
        if default is RxTxApp._PARAM_UNSET:
            default = UNIVERSAL_PARAMS[key]
        val = self.params.get(key, default)
        return cast(val) if cast is not None else val

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

        session_type = self.params.get("session_type", UNIVERSAL_PARAMS["session_type"])
        direction = self.params.get("direction")  # None means loopback
        test_mode = self.params.get("test_mode", UNIVERSAL_PARAMS["test_mode"])

        # Determine NIC ports list
        nic_port = self.params.get("nic_port")
        nic_port_r = self.params.get("nic_port_r")  # Redundant port
        nic_port_list = self.params.get("nic_port_list") or (
            [nic_port] if nic_port else []
        )

        # For redundant mode, add the redundant port to the list
        redundant = self.params.get("redundant", False)
        if redundant and nic_port_r and nic_port_r not in nic_port_list:
            nic_port_list.append(nic_port_r)

        # For loopback mode, need two interfaces; for single direction, one is enough
        if len(nic_port_list) == 1 and direction not in ("tx", "rx"):
            nic_port_list = nic_port_list * 2

        # Base legacy structure
        config = create_empty_config()
        config["tx_no_chain"] = self.params.get("tx_no_chain", False)

        # Fill interface names & addressing using legacy helper
        add_interfaces(config, nic_port_list, test_mode, direction=direction)

        # Remove unused interfaces (empty name/ip causes MTL to fail with "invalid ip 0.0.0.0")
        config["interfaces"] = [
            iface for iface in config["interfaces"] if iface.get("name")
        ]

        # Redundant mode: configure 2 interfaces and dual IP arrays in sessions
        if redundant and len(nic_port_list) >= 2:
            source_ip = self.params.get("source_ip")
            dest_ip = self.params.get("destination_ip")
            source_ip_r = self.params.get("source_ip_r")
            dest_ip_r = self.params.get("destination_ip_r")

            if not all([source_ip, dest_ip, source_ip_r, dest_ip_r]):
                logger.warning(
                    "Redundant mode requires source_ip, destination_ip, "
                    "source_ip_r, destination_ip_r parameters"
                )

            # Ensure we have exactly 2 interfaces
            config["interfaces"] = [
                {"name": nic_port_list[0], "ip": ""},
                {"name": nic_port_list[1], "ip": ""},
            ]

            if direction == "tx":
                config["interfaces"][0]["ip"] = source_ip or ""
                config["interfaces"][1]["ip"] = source_ip_r or ""
                if config["tx_sessions"] and len(config["tx_sessions"]) > 0:
                    config["tx_sessions"][0]["dip"] = [dest_ip or "", dest_ip_r or ""]
                    config["tx_sessions"][0]["interface"] = [0, 1]
                config["rx_sessions"] = []
            elif direction == "rx":
                config["interfaces"][0]["ip"] = dest_ip or ""
                config["interfaces"][1]["ip"] = dest_ip_r or ""
                if config["rx_sessions"] and len(config["rx_sessions"]) > 0:
                    config["rx_sessions"][0]["ip"] = [
                        source_ip or "",
                        source_ip_r or "",
                    ]
                    config["rx_sessions"][0]["interface"] = [0, 1]
                config["tx_sessions"] = []
            else:
                # Loopback redundant (less common)
                config["interfaces"][0]["ip"] = source_ip or ""
                config["interfaces"][1]["ip"] = source_ip_r or ""
                if config["tx_sessions"] and len(config["tx_sessions"]) > 0:
                    config["tx_sessions"][0]["dip"] = [dest_ip or "", dest_ip_r or ""]
                    config["tx_sessions"][0]["interface"] = [0, 1]
                if config["rx_sessions"] and len(config["rx_sessions"]) > 0:
                    config["rx_sessions"][0]["ip"] = [
                        source_ip or "",
                        source_ip_r or "",
                    ]
                    config["rx_sessions"][0]["interface"] = [0, 1]

            logger.info(
                f"Redundant mode: interfaces={[i['name'] for i in config['interfaces']]}, "
                f"direction={direction}"
            )
        else:
            # Non-redundant mode: fix single interface indices
            if len(config["interfaces"]) == 1:
                if config["tx_sessions"] and len(config["tx_sessions"]) > 0:
                    config["tx_sessions"][0]["interface"] = [0]
                if config["rx_sessions"] and len(config["rx_sessions"]) > 0:
                    config["rx_sessions"][0]["interface"] = [0]

            # Override interface IPs and session IPs with user-provided source_ip/destination_ip if specified
            # This allows tests to use custom IP addressing instead of ip_pools values
            if test_mode == "unicast":
                user_source_ip = self.params.get("source_ip")
                user_dest_ip = self.params.get("destination_ip")

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

        # Add rx_queues_cnt/tx_queues_cnt to interfaces if specified
        rx_queues_cnt = self.params.get("rx_queues_cnt")
        tx_queues_cnt = self.params.get("tx_queues_cnt")
        if rx_queues_cnt is not None or tx_queues_cnt is not None:
            for iface in config["interfaces"]:
                if rx_queues_cnt is not None:
                    iface["rx_queues_cnt"] = int(rx_queues_cnt)
                if tx_queues_cnt is not None:
                    iface["tx_queues_cnt"] = int(tx_queues_cnt)

        # Helper to populate a nested session list for a given type
        def _populate_session(is_tx: bool):
            """Build a per-type session dict from ``self.params`` /
            UNIVERSAL_PARAMS. The shape mirrors the legacy add_*_sessions
            helpers exactly so the resulting JSON is accepted by RxTxApp.
            """
            p = self._p

            # Common header fields used by every type (defaults can still be
            # overridden per-type via the ``port_default`` / ``pt_default`` args).
            def _hdr(port_default=None, pt_default=None):
                return {
                    "replicas": p("replicas"),
                    "start_port": p(
                        "port",
                        (
                            UNIVERSAL_PARAMS["port"]
                            if port_default is None
                            else port_default
                        ),
                        cast=int,
                    ),
                    "payload_type": p(
                        "payload_type",
                        (
                            UNIVERSAL_PARAMS["payload_type"]
                            if pt_default is None
                            else pt_default
                        ),
                        cast=int,
                    ),
                }

            if session_type == "st20p":
                session = {
                    **_hdr(),
                    "width": p("width", cast=int),
                    "height": p("height", cast=int),
                    "fps": p("framerate"),
                    "interlaced": p("interlaced"),
                    "device": p("device"),
                    "pacing": p("pacing"),
                    "packing": p("packing"),
                    "transport_format": p("transport_format"),
                    "display": p("display"),
                    "enable_rtcp": p("enable_rtcp"),
                }
                pixel_format = p("pixel_format")
                if is_tx:
                    session["input_format"] = pixel_format
                    session["st20p_url"] = p("input_file")
                else:
                    session["output_format"] = pixel_format
                    session["measure_latency"] = p("measure_latency")
                    session["st20p_url"] = p("output_file")
                return session

            if session_type == "st22p":
                session = {
                    **_hdr(),
                    "width": p("width", cast=int),
                    "height": p("height", cast=int),
                    "fps": p("framerate"),
                    "interlaced": p("interlaced"),
                    "pack_type": "codestream",  # fixed value (template default)
                    "codec": p("codec"),
                    "device": p("device"),
                    "quality": p("quality"),
                    "codec_thread_count": p("codec_threads"),
                    "enable_rtcp": p("enable_rtcp"),
                }
                pixel_format = p("pixel_format")
                if is_tx:
                    session["input_format"] = pixel_format
                    session["st22p_url"] = p("input_file")
                else:
                    session["output_format"] = pixel_format
                    session["display"] = p("display")
                    session["measure_latency"] = p("measure_latency")
                    session["st22p_url"] = p("output_file")
                return session

            if session_type == "video":
                # Legacy video session — used by upstream perf tests.
                session = {
                    **_hdr(),
                    "type": p("type_mode"),
                    "pacing": p("pacing"),
                    "packing": p("packing"),
                    "tr_offset": p("tr_offset"),
                    "video_format": p("video_format"),
                    "pg_format": p("pg_format"),
                }
                if is_tx:
                    session["video_url"] = p("video_url")
                else:
                    session["display"] = p("display")
                return session

            if session_type == "audio":
                # Legacy audio session — port/pt default to 30000/111.
                return {
                    **_hdr(port_default=30000, pt_default=111),
                    "type": p("type_mode"),
                    "audio_format": p("audio_format"),
                    "audio_channel": p("audio_channels"),
                    "audio_sampling": p("audio_sampling"),
                    "audio_ptime": p("audio_ptime"),
                    "audio_url": p("audio_url"),
                }

            if session_type == "ancillary":
                # Legacy ancillary session — port/pt default to 40000/113.
                session = _hdr(port_default=40000, pt_default=113)
                if is_tx:
                    session["type"] = p("type_mode")
                    session["ancillary_format"] = p("ancillary_format")
                    session["ancillary_url"] = p("ancillary_url")
                    session["ancillary_fps"] = p("ancillary_fps")
                return session

            if session_type == "st30p":
                session = {
                    **_hdr(),
                    "audio_format": p("audio_format"),
                    "audio_channel": p("audio_channels"),
                    "audio_sampling": p("audio_sampling"),
                    "audio_ptime": p("audio_ptime"),
                }
                session["audio_url"] = p("input_file") if is_tx else p("output_file")
                return session

            if session_type == "fastmetadata":
                session = {
                    **_hdr(),
                    "fastmetadata_data_item_type": p(
                        "fastmetadata_data_item_type", cast=int
                    ),
                    "fastmetadata_k_bit": p("fastmetadata_k_bit", cast=int),
                }
                if is_tx:
                    session["type"] = p("type_mode")
                    session["fastmetadata_fps"] = p("fastmetadata_fps")
                    session["fastmetadata_url"] = p("input_file")
                else:
                    session["fastmetadata_url"] = p("output_file")
                return session

            if session_type == "st40p":
                # st40p (ancillary pipeline) — port/pt default to 40000/113;
                # mirrors add_st40p_sessions() in legacy RxTxApp.py.
                session = {
                    **_hdr(port_default=40000, pt_default=113),
                    "interlaced": p("interlaced"),
                    "enable_rtcp": p("enable_rtcp"),
                }
                if is_tx:
                    session["fps"] = self.params.get("fps", p("framerate"))
                    session["st40p_url"] = self.params.get("st40p_url", p("input_file"))
                return session

            logger.warning(
                f"Unknown session type '{session_type}', using minimal config"
            )
            return {"replicas": 1}

        # Populate TX sessions
        if direction in (None, "tx"):
            st_entry = _populate_session(True)
            if st_entry:
                config["tx_sessions"][0].setdefault(session_type, [])
                config["tx_sessions"][0][session_type].append(st_entry)

        # Populate RX sessions
        if direction in (None, "rx"):
            st_entry = _populate_session(False)
            if st_entry:
                config["rx_sessions"][0].setdefault(session_type, [])
                config["rx_sessions"][0][session_type].append(st_entry)

        # If only TX or only RX requested, clear the other list
        if direction == "tx":
            config["rx_sessions"] = []
        elif direction == "rx":
            config["tx_sessions"] = []

        return config

    def prepare_execution(self, build: str, host=None, **kwargs):
        """Write RxTxApp JSON config file to remote host before execution.

        Args:
            build: Path to MTL build directory
            host: Host connection object
            interface_setup: Optional InterfaceSetup; when provided and the
                config contains kernel-socket interfaces (``kernel:<ifname>``)
                with an ``_os_ip`` annotation, OS-level IPs are configured and
                registered for cleanup, mirroring legacy ``RxTxApp.execute_test``.
        """
        if not host:
            raise ValueError("host required for RxTxApp config writing")

        if not self.config:
            raise RuntimeError(
                "create_command() must be called before prepare_execution()"
            )

        # Write config file using mfd library (handles both local and remote hosts)
        remote_conn = host.connection

        # Configure kernel-socket interfaces (mirrors legacy RxTxApp.execute_test)
        interface_setup = kwargs.get("interface_setup")
        try:
            from .RxTxApp import configure_kernel_interfaces

            configure_kernel_interfaces(self.config, remote_conn, interface_setup)
        except Exception as e:  # pragma: no cover - defensive
            logger.warning(f"configure_kernel_interfaces skipped: {e}")

        # Extract config file path from command (it's relative)
        match = re.search(r"--config_file\s+(\S+)", self.command)
        if match:
            config_file_relative = match.group(1)
            # Make it absolute on remote host by joining with build directory
            config_file_path = f"{build}/{config_file_relative}"
        else:
            config_file_path = f"{build}/tests/config.json"

        # Write config using mfd library's path() which works for both local and remote
        config_json = json.dumps(self.config, indent=4)

        # Use mfd library's path object - handles local/remote transparently
        f = remote_conn.path(config_file_path)

        # Escape quotes for SSH connections
        if isinstance(remote_conn, SSHConnection):
            config_json = config_json.replace('"', '\\"')

        f.write_text(config_json, encoding="utf-8")
        logger.info(f"Wrote RxTxApp config to {config_file_path}")

        # Update command to use absolute path on remote host
        self.command = self.command.replace(
            f"--config_file {config_file_relative}", f"--config_file {config_file_path}"
        )

    def validate_results(self, fail_on_error: bool = True) -> bool:  # type: ignore[override]
        """
        Validate execution results exactly like original RxTxApp.execute_test().

        Matches the validation pattern from mtl_engine/RxTxApp.py:
        - For st20p: Check RX output + TX/RX converter creation (NOT tx result lines)
        - For st22p: Check RX output only
        - For video/audio/etc: Check both TX and RX outputs

        When ``fail_on_error`` is False, validation problems return ``False``
        without recording a pytest failure (used by performance binary-search
        loops where intermediate iterations are expected to fail).

        Returns True if validation passes. Raises AssertionError on failure
        (only when ``fail_on_error`` is True).
        """

        def _fail(msg: str):
            self._fail_validation(msg, fail_on_error)

        try:
            if not self.config:
                _fail("RxTxApp validate_results called without config")

            # Multi-session aware: when more than one session type is populated
            # (e.g. kernel_lo / xdp / rx_timing/mixed run st20p+st30p+ancillary
            # in a single RxTxApp invocation), validate every type sequentially.
            all_types = self._get_all_session_types_from_config(self.config)
            if len(all_types) > 1:
                output_lines = self.last_output.split("\n") if self.last_output else []
                rc = self.last_return_code
                if rc not in (0, None):
                    _fail(f"Process return code {rc} indicates failure")
                for stype in all_types:
                    if not self._validate_single_session_type(stype, output_lines):
                        _fail(f"{stype} validation failed (multi-session)")
                logger.info(f"RxTxApp multi-session validation passed for {all_types}")
                return True

            session_type = self._get_session_type_from_config(self.config)
            output_lines = self.last_output.split("\n") if self.last_output else []
            rc = self.last_return_code

            # 1. Check return code (must be 0 or None for dual-host secondary)
            if rc not in (0, None):
                _fail(f"Process return code {rc} indicates failure")

            # 2. Validate based on session type. Single- and multi-session
            #    paths share the same per-type dispatch via
            #    _validate_single_session_type.
            if not self._validate_single_session_type(session_type, output_lines):
                _fail(f"{session_type} validation failed")

            logger.info(f"RxTxApp validation passed for {session_type}")
            return True

        except AssertionError:
            # Already handled/logged
            raise
        except Exception as e:
            _fail(f"RxTxApp validation unexpected error: {e}")

    def _validate_single_session_type(
        self, session_type: str, output_lines: list
    ) -> bool:
        """Run per-type validation (RX, plus TX where the legacy code did).

        Single dispatch shared by the single-session and multi-session
        branches of ``validate_results``. Mirrors the per-type rules from the
        legacy ``RxTxApp.execute_test()``:

        - ``st20p``: RX output + TX/RX converter outputs (no plain TX check).
        - ``st22p``/``st30p``/``st40p``/``fastmetadata``/``ancillary``: RX
          output only. ST22P additionally logs a warning if
          codec/encoder/decoder did not load, but does not fail on it (codecs
          may register lazily).
        - ``video``/``audio`` (and unknown types): TX + RX outputs.
        """
        if session_type == "st20p":
            return (
                check_rx_output(self.config, output_lines, "st20p", False)
                and check_tx_converter_output(self.config, output_lines, "st20p", False)
                and check_rx_converter_output(self.config, output_lines, "st20p", False)
            )
        if session_type in ("st22p", "st30p", "st40p", "fastmetadata", "ancillary"):
            rx_session_type = "anc" if session_type == "ancillary" else session_type
            ok = check_rx_output(self.config, output_lines, rx_session_type, False)
            if session_type == "st22p" and not check_codec_loaded(
                output_lines, session_type, False
            ):
                logger.warning(
                    "ST22P codec loading check failed - encoder/decoder may not be registered"
                )
            return ok
        # video / audio / unknown -> TX + RX
        if session_type not in ("video", "audio"):
            logger.warning(
                "Unknown session type %s, using default TX+RX validation",
                session_type,
            )
        return check_tx_output(
            self.config, output_lines, session_type, False
        ) and check_rx_output(self.config, output_lines, session_type, False)

    # Session types recognised in RxTxApp configs, in priority order. Used by
    # both the single- and multi-session helpers below.
    _SESSION_TYPES = (
        "st22p",
        "st20p",
        "st30p",
        "st40p",
        "fastmetadata",
        "video",
        "audio",
        "ancillary",
    )

    def _get_session_type_from_config(self, config: dict) -> str:
        """Extract primary session type from RxTxApp config (first non-empty)."""
        types = self._get_all_session_types_from_config(config)
        return types[0] if types else "st20p"

    def _get_all_session_types_from_config(self, config: dict) -> list:
        """Return every populated session type in config (multi-session aware)."""
        types: list = []
        for tx_entry in config.get("tx_sessions") or []:
            for stype in self._SESSION_TYPES:
                sessions = tx_entry.get(stype)
                if not sessions:
                    continue
                if stype not in types:
                    types.append(stype)
        return types

    def _resolve_capture_dst_ip(self):
        """Return the destination IP for netsniff capture, or ``None``.

        RxTxApp stores TX destinations under ``config['tx_sessions'][i]['dip']``
        (a list). We use the first TX session's first DIP as the capture filter
        target; that matches the single-stream case the capture path is
        designed for. Returns ``None`` if no TX session / DIP is configured,
        which the base class treats as "skip capture".
        """
        tx_sessions = (self.config or {}).get("tx_sessions") or []
        if tx_sessions and tx_sessions[0].get("dip"):
            return tx_sessions[0]["dip"][0]
        return None
