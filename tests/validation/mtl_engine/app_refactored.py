# Universal Media Transport Library Application Interface
# Provides unified parameter system for RxTxApp, FFmpeg, and GStreamer

import json
import logging
import time
import os
import tempfile

from .config.universal_params import UNIVERSAL_PARAMS
from .config.param_mappings import RXTXAPP_PARAM_MAP, FFMPEG_PARAM_MAP, GSTREAMER_PARAM_MAP
from .config.app_mappings import (
    APP_NAME_MAP, 
    FFMPEG_FORMAT_MAP, 
    SESSION_TYPE_MAP, 
    FRAMERATE_TO_VIDEO_FORMAT_MAP
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

class Application:
    def __init__(self, app_framework, app_path, config_file_path=None):
        """Initialize application with framework type, path to application directory, and optional config file."""
        self.app_framework = app_framework
        self.app_path = app_path  # Path to directory containing the application
        self.config_file_path = config_file_path
        self.command = []
        self.universal_params = UNIVERSAL_PARAMS.copy()

    def create_command(self, **kwargs) -> tuple:
        """
        Set universal parameters and create command line and config files for any application type.
        Combines parameter setting with command/config generation in one call.
        
        Args:
            **kwargs: Universal parameter names and values
            
        Returns:
            Tuple of (command_string, config_dict_or_none)
            - For RxTxApp: (command_line, config_dict)
            - For FFmpeg/GStreamer: (command_line, None)
        """
        # Set universal parameters
        for param, value in kwargs.items():
            if param in self.universal_params:
                self.universal_params[param] = value
            else:
                raise ValueError(f"Unknown universal parameter: {param}")
        
        # Create command and config based on application type
        app_type = self.app_framework.lower()
        
        if app_type == "rxtxapp":
            command, config = self._create_rxtxapp_command_and_config()
            # Auto-save config file when created
            if config:
                # Use absolute path for config file or save in app path
                if self.config_file_path:
                    config_path = self.config_file_path
                else:
                    # Save config in the app directory where RxTxApp will run
                    config_path = os.path.join(self.app_path, "config.json")
                try:
                    with open(config_path, 'w') as f:
                        json.dump(config, f, indent=4)
                except Exception as e:
                    print(f"Warning: Could not save config file {config_path}: {e}")
            return command, config
        elif app_type == "ffmpeg":
            return self._create_ffmpeg_command(), None
        elif app_type == "gstreamer":
            return self._create_gstreamer_command(), None
        else:
            raise ValueError(f"Unsupported application framework: {self.app_framework}")

    def _get_executable_path(self) -> str:
        """Get the full path to the executable based on framework type."""
        app_type = self.app_framework.lower()
        app_name = APP_NAME_MAP.get(app_type, "")
        
        if not app_name:
            raise ValueError(f"Unknown application framework: {self.app_framework}")
        
        # For RxTxApp, combine path with executable name
        if app_type == "rxtxapp":
            if self.app_path.endswith("/"):
                return f"{self.app_path}{app_name}"
            else:
                return f"{self.app_path}/{app_name}"
        else:
            # For ffmpeg and gstreamer, assume they're in system PATH
            # or use provided path if it's a full path to the executable
            return app_name

    def _create_rxtxapp_command_and_config(self) -> tuple:
        """
        Generate RxTxApp command line and JSON configuration from universal parameters.
        Uses config file path from constructor if provided, otherwise defaults to "config.json".
        
        Returns:
            Tuple of (command_string, config_dict)
        """
        # Use config file path from constructor or default (absolute path)
        if self.config_file_path:
            config_file_path = os.path.abspath(self.config_file_path)
        else:
            config_file_path = os.path.abspath(os.path.join(self.app_path, "config.json"))

        # Build command line with all command-line parameters
        executable_path = self._get_executable_path()
        cmd_parts = ["sudo", executable_path]
        cmd_parts.extend(["--config_file", config_file_path])

        # Add command-line parameters from RXTXAPP_PARAM_MAP
        for universal_param, rxtx_param in RXTXAPP_PARAM_MAP.items():
            # Only process command-line flags (those starting with --)
            if isinstance(rxtx_param, str) and rxtx_param.startswith("--"):
                value = self.universal_params.get(universal_param)
                if value is not None:
                    # Boolean parameters: add flag only if True
                    if isinstance(value, bool):
                        if value:
                            cmd_parts.append(rxtx_param)
                    # Value parameters: add flag and value
                    else:
                        cmd_parts.extend([rxtx_param, str(value)])

        # Create JSON configuration
        config_dict = self._create_rxtxapp_config_dict()

        return " ".join(cmd_parts), config_dict

    def _create_rxtxapp_config_dict(self) -> dict:
        """
        Build complete RxTxApp JSON config structure from universal parameters.
        Creates interfaces, sessions, and all session-specific configurations.
        
        Returns:
            Complete RxTxApp configuration dictionary
        """
        # Start with base configuration
        config = {
            "tx_no_chain": self.universal_params.get("tx_no_chain", False),  # Always include tx_no_chain with default False
            "interfaces": [],
            "tx_sessions": [],
            "rx_sessions": []
        }
        
        # Create interface configuration
        nic_port = self.universal_params.get("nic_port", "0000:31:01.0")  # Default to first VF
        nic_port_list = self.universal_params.get("nic_port_list", [nic_port])  # Support both single port and list
        source_ip = self.universal_params.get("source_ip", "192.168.17.101")  # Default TX IP
        test_mode = self.universal_params.get("test_mode", "unicast")
        
        # Use nic_port_list if provided, otherwise use single nic_port
        if isinstance(nic_port_list, list) and len(nic_port_list) > 0:
            vf_list = nic_port_list
        else:
            vf_list = [nic_port]
            
        # For unicast mode with loopback, we need separate TX and RX interfaces
        if test_mode == "unicast":
            # TX interface
            tx_interface = {
                "name": vf_list[0],  # First VF for TX
                "ip": source_ip,  # TX interface IP
            }
            config["interfaces"].append(tx_interface)
            
            # RX interface (for loopback testing)
            if len(vf_list) > 1:
                rx_ip = self.universal_params.get("destination_ip", "192.168.17.102")
                rx_interface = {
                    "name": vf_list[1],  # Second VF for RX
                    "ip": rx_ip,  # RX interface IP (destination)
                }
                config["interfaces"].append(rx_interface)
        # For multicast mode, we need separate TX and RX interfaces with different VFs
        elif test_mode == "multicast":
            # Calculate if we need extra queues for high bandwidth (1080p+)
            width = int(self.universal_params.get("width", 1920))
            height = int(self.universal_params.get("height", 1080))
            is_high_bandwidth = (width >= 1920 and height >= 1080)
            
            if is_high_bandwidth:
                # For 1080p+, use dual interface but with simpler configuration
                # TX interface
                tx_interface = {
                    "name": vf_list[0],  # First VF for TX
                    "ip": "192.168.17.101",  # Standard multicast TX IP
                }
                config["interfaces"].append(tx_interface)
                
                # RX interface (for loopback testing)
                if len(vf_list) > 1:
                    rx_interface = {
                        "name": vf_list[1],  # Second VF for RX
                        "ip": "192.168.17.102",  # Standard multicast RX IP
                    }
                    config["interfaces"].append(rx_interface)
            else:
                # For lower resolutions, use separate TX and RX interfaces
                extra_queues = 1
                
                if vf_list:
                    # TX interface
                    tx_interface = {
                        "name": vf_list[0],  # First VF for TX
                        "ip": "192.168.17.101"  # Standard multicast TX IP
                    }
                    config["interfaces"].append(tx_interface)
                
                    # RX interface (for loopback testing)
                    if len(vf_list) > 1:
                        rx_interface = {
                            "name": vf_list[1],  # Second VF for RX
                            "ip": "192.168.17.102"  # Standard multicast RX IP
                        }
                        config["interfaces"].append(rx_interface)
        else:
            # Single interface for unicast/other modes
            interface = {
                "name": vf_list[0],  # Use first VF from the list
                "ip": source_ip
            }
            
            # Add optional interface parameters based on parse_json.c
            optional_interface_params = [
                "netmask", "gateway", "proto", "tx_queues_cnt", "rx_queues_cnt"
            ]
            for param in optional_interface_params:
                if self.universal_params.get(param):
                    interface[param] = self.universal_params[param]
            
            config["interfaces"].append(interface)
        
        # Create session configuration based on direction
        direction = self.universal_params.get("direction", None)  # Default to None to create both TX and RX
        session_type = self.universal_params.get("session_type", "st20p")
        test_mode = self.universal_params.get("test_mode", "unicast")
        
        # For RxTxApp, create both TX and RX sessions by default (it's a loopback test)
        if direction is None or direction == "tx":  # Create TX session
            # Always use the destination_ip from universal parameters
            tx_session = {
                "dip": [self.universal_params.get("destination_ip", "239.168.48.9")],
                "interface": [0],  # TX interface
                "video": [],
                "st20p": [],
                "st22p": [],
                "st30p": [],
                "audio": [],
                "ancillary": [],
                "fastmetadata": []
            }
            
            # Add session data based on type
            session_data = self._create_session_data(session_type, True)
            if session_data:
                tx_session[session_type].append(session_data)
            
            config["tx_sessions"].append(tx_session)
        
        if direction is None or direction == "rx":  # Create RX session
            # For unicast loopback, RX should filter for packets FROM the TX interface (source IP)
            # For multicast, use the multicast IP
            if test_mode == "unicast":
                # For unicast loopback, RX filters for packets FROM source_ip (TX interface)
                rx_ip = self.universal_params.get("source_ip", "192.168.17.101")
            else:
                # For multicast, use the destination IP (multicast address)
                rx_ip = self.universal_params.get("destination_ip", "239.168.48.9")
            
            # Determine RX interface index
            # If only one interface defined, use index 0 for both TX and RX
            rx_interface_index = 1 if len(config["interfaces"]) > 1 else 0
            
            rx_session = {
                "ip": [rx_ip],  # For unicast: source IP to filter, for multicast: multicast IP
                "interface": [rx_interface_index],  # RX interface
                "video": [],
                "st20p": [],
                "st22p": [],
                "st30p": [],
                "audio": [],
                "ancillary": [],
                "fastmetadata": []
            }
            
            # Add session data based on type
            session_data = self._create_session_data(session_type, False)
            if session_data:
                rx_session[session_type].append(session_data)
            
            config["rx_sessions"].append(rx_session)
        
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
            return {}

    def _create_st20p_session_data(self, is_tx: bool) -> dict:
        """Create ST20p (uncompressed video) session data from universal parameters."""
        session = {
            "replicas": self.universal_params.get("replicas", 1),
            "start_port": int(self.universal_params.get("port", 20000)),
            "payload_type": self.universal_params.get("payload_type", 112),
            "width": int(self.universal_params.get("width", 1920)),
            "height": int(self.universal_params.get("height", 1080)),
            "fps": self.universal_params.get("framerate", "p60"),
            "interlaced": self.universal_params.get("interlaced", False),
            "device": self.universal_params.get("device", "AUTO"),
            "pacing": self.universal_params.get("pacing", "gap"),
            "packing": self.universal_params.get("packing", "BPM"),
            "transport_format": self.universal_params.get("transport_format", "YUV_422_10bit"),
            "display": self.universal_params.get("display", False),
            "enable_rtcp": self.universal_params.get("enable_rtcp", False)
        }
        
        if is_tx:
            session["input_format"] = self.universal_params.get("pixel_format", "YUV422PLANAR10LE")
            session["st20p_url"] = self.universal_params.get("input_file") or ""
        else:
            # Use pixel_format_rx if specified, otherwise use pixel_format
            rx_format = self.universal_params.get("pixel_format_rx") or self.universal_params.get("pixel_format", "YUV422PLANAR10LE")
            session["output_format"] = rx_format
            # For RX, use output_file parameter
            session["st20p_url"] = self.universal_params.get("output_file", "")
            session["measure_latency"] = self.universal_params.get("measure_latency", False)
        
        return session

    def _create_st22p_session_data(self, is_tx: bool) -> dict:
        """Create ST22p (compressed video with JPEG-XS) session data from universal parameters."""
        session = {
            "replicas": self.universal_params.get("replicas", 1),
            "start_port": self.universal_params.get("port", 20000),
            "payload_type": self.universal_params.get("payload_type", 114),
            "width": self.universal_params.get("width", 1920),
            "height": self.universal_params.get("height", 1080),
            "fps": self.universal_params.get("framerate", "p25"),
            "interlaced": self.universal_params.get("interlaced", False),
            "pack_type": "codestream",
            "codec": self.universal_params.get("codec", "JPEG-XS"),
            "device": self.universal_params.get("device", "AUTO"),
            "quality": self.universal_params.get("quality", "speed"),
            "codec_thread_count": self.universal_params.get("codec_threads", 2),
            "enable_rtcp": self.universal_params.get("enable_rtcp", False)
        }

        if is_tx:
            session["input_format"] = self.universal_params.get("pixel_format", "YUV422PLANAR10LE")
            session["st22p_url"] = self.universal_params.get("input_file", "")
        else:
            session["output_format"] = self.universal_params.get("pixel_format", "YUV422PLANAR10LE")
            session["display"] = self.universal_params.get("display", False)
            session["measure_latency"] = self.universal_params.get("measure_latency", False)
        
        return session

    def _create_st30p_session_data(self, is_tx: bool) -> dict:
        """Create ST30p (uncompressed audio) session data from universal parameters."""
        session = {
            "replicas": self.universal_params.get("replicas", 1),
            "start_port": self.universal_params.get("port", 30000),
            "payload_type": self.universal_params.get("payload_type", 111),
            "audio_format": self.universal_params.get("audio_format", "PCM24"),
            "audio_channel": self.universal_params.get("audio_channels", ["U02"]),
            "audio_sampling": self.universal_params.get("audio_sampling", "96kHz"),
            "audio_ptime": self.universal_params.get("audio_ptime", "1"),
            "audio_url": self.universal_params.get("input_file" if is_tx else "output_file", "")
        }
        
        return session

    def _create_video_session_data(self, is_tx: bool) -> dict:
        """Create raw video session data from universal parameters."""
        session = {
            "replicas": self.universal_params.get("replicas", 1),
            "type": "frame",
            "pacing": self.universal_params.get("pacing", "gap"),
            "packing": self.universal_params.get("packing", "BPM"),
            "start_port": self.universal_params.get("port", 20000),
            "payload_type": self.universal_params.get("payload_type", 112),
            "tr_offset": "default",
            "video_format": self._convert_framerate_to_video_format(self.universal_params.get("framerate", "p60")),
            "pg_format": self.universal_params.get("transport_format", "YUV_422_10bit"),
            "enable_rtcp": self.universal_params.get("enable_rtcp", False)
        }
        
        if is_tx:
            session["video_url"] = self.universal_params.get("input_file", "")
        else:
            session["display"] = self.universal_params.get("display", False)
            session["measure_latency"] = self.universal_params.get("measure_latency", False)
        
        return session

    def _create_audio_session_data(self, is_tx: bool) -> dict:
        """Create raw audio session data from universal parameters."""
        session = {
            "replicas": self.universal_params.get("replicas", 1),
            "type": "frame",
            "start_port": self.universal_params.get("port", 30000),
            "payload_type": self.universal_params.get("payload_type", 111),
            "audio_format": self.universal_params.get("audio_format", "PCM24"),
            "audio_channel": self.universal_params.get("audio_channels", ["U02"]),
            "audio_sampling": self.universal_params.get("audio_sampling", "48kHz"),
            "audio_ptime": self.universal_params.get("audio_ptime", "1"),
            "audio_url": self.universal_params.get("input_file" if is_tx else "output_file", ""),
            "enable_rtcp": self.universal_params.get("enable_rtcp", False)
        }
        
        return session

    def _create_ancillary_session_data(self, is_tx: bool) -> dict:
        """Create ancillary data (closed captions, etc.) session from universal parameters."""
        session = {
            "replicas": self.universal_params.get("replicas", 1),
            "start_port": self.universal_params.get("port", 40000),
            "payload_type": self.universal_params.get("payload_type", 113),
            "type": "frame"
        }
        
        if is_tx:
            session["ancillary_format"] = "closed_caption"
            session["ancillary_url"] = self.universal_params.get("input_file", "")
            session["ancillary_fps"] = "p59"
        
        return session

    def _create_fastmetadata_session_data(self, is_tx: bool) -> dict:
        """Create fast metadata session data from universal parameters."""
        session = {
            "replicas": self.universal_params.get("replicas", 1),
            "start_port": self.universal_params.get("port", 40000),
            "payload_type": self.universal_params.get("payload_type", 115),
            "type": "frame",
            "fastmetadata_data_item_type": 1234567,
            "fastmetadata_k_bit": 0
        }
        
        if is_tx:
            session["fastmetadata_fps"] = "p59"
            session["fastmetadata_url"] = self.universal_params.get("input_file", "")
        else:
            session["fastmetadata_url"] = self.universal_params.get("output_file", "")
        
        return session

    def _create_ffmpeg_command(self) -> str:
        """
        Build FFmpeg command line with MTL plugin parameters from universal parameters.
        Handles input files, network settings, and output format configuration.
        
        Returns:
            FFmpeg command string
        """
        executable_path = self._get_executable_path()
        cmd_parts = [executable_path]
        
        direction = self.universal_params.get("direction", "tx")
        
        # Input parameters for TX
        if direction == "tx":
            if self.universal_params.get("input_file"):
                video_size = f"{self.universal_params.get('width', 1920)}x{self.universal_params.get('height', 1080)}"
                cmd_parts.extend(["-video_size", video_size])
                cmd_parts.extend(["-f", "rawvideo"])
                if self.universal_params.get("pixel_format"):
                    ffmpeg_format = self._convert_to_ffmpeg_format(self.universal_params["pixel_format"])
                    cmd_parts.extend(["-pix_fmt", ffmpeg_format])
                cmd_parts.extend(["-i", self.universal_params["input_file"]])
                
                # Add framerate filter
                if self.universal_params.get("fps_numeric"):
                    cmd_parts.extend(["-filter:v", f"fps={self.universal_params['fps_numeric']}"])
        
        # Network parameters
        if self.universal_params.get("nic_port"):
            cmd_parts.extend(["-p_port", self.universal_params["nic_port"]])
        if self.universal_params.get("source_ip"):
            cmd_parts.extend(["-p_sip", self.universal_params["source_ip"]])
        
        if direction == "tx" and self.universal_params.get("destination_ip"):
            cmd_parts.extend(["-p_tx_ip", self.universal_params["destination_ip"]])
        elif direction == "rx" and self.universal_params.get("multicast_ip"):
            cmd_parts.extend(["-p_rx_ip", self.universal_params["multicast_ip"]])
        
        if self.universal_params.get("port"):
            cmd_parts.extend(["-udp_port", str(self.universal_params["port"])])
        if self.universal_params.get("payload_type"):
            cmd_parts.extend(["-payload_type", str(self.universal_params["payload_type"])])
        
        # Output format - convert universal session type to FFmpeg format
        session_type = self.universal_params.get("session_type", "st20p")
        ffmpeg_format = self._convert_to_ffmpeg_session_type(session_type)
        cmd_parts.extend(["-f", ffmpeg_format])
        
        if direction == "tx":
            cmd_parts.append("-")
        elif direction == "rx" and self.universal_params.get("output_file"):
            cmd_parts.append(self.universal_params["output_file"])
        
        return " ".join(cmd_parts)

    def _create_gstreamer_command(self) -> str:
        """
        Build GStreamer pipeline command with MTL elements from universal parameters.
        Creates TX/RX pipelines with appropriate source/sink elements.
        
        Returns:
            GStreamer command string
        """
        executable_path = self._get_executable_path()
        cmd_parts = [executable_path, "-v"]
        
        direction = self.universal_params.get("direction", "tx")
        session_type = self.universal_params.get("session_type", "st20p")
        
        if direction == "tx":
            # Source element
            if self.universal_params.get("input_file"):
                cmd_parts.extend(["filesrc", f"location={self.universal_params['input_file']}"])
                cmd_parts.append("!")
            
            # Add caps if needed
            width = self.universal_params.get("width", 1920)
            height = self.universal_params.get("height", 1080)
            framerate = self.universal_params.get("framerate", "60/1")
            
            # Convert framerate format
            if not "/" in str(framerate):
                framerate = f"{framerate}/1"
            
            cmd_parts.extend([
                "rawvideoparse",
                f"width={width}",
                f"height={height}",
                f"framerate={framerate}",
                "!"
            ])
            
            # MTL TX element - convert universal session type to GStreamer element name
            gst_element = self._convert_to_gstreamer_element(session_type, "tx")
            cmd_parts.append(gst_element)
            
        else:  # RX
            # MTL RX element - convert universal session type to GStreamer element name
            gst_element = self._convert_to_gstreamer_element(session_type, "rx")
            cmd_parts.append(gst_element)
            
            cmd_parts.append("!")
            
            # Sink element
            if self.universal_params.get("output_file"):
                cmd_parts.extend(["filesink", f"location={self.universal_params['output_file']}"])
        
        return " ".join(cmd_parts)

    def _convert_to_ffmpeg_format(self, universal_format: str) -> str:
        """Convert universal pixel format names to FFmpeg pixel format names."""
        return FFMPEG_FORMAT_MAP.get(universal_format, "yuv422p10le")

    def _convert_to_ffmpeg_session_type(self, universal_session_type: str) -> str:
        """Convert universal session type to FFmpeg format specifier."""
        return SESSION_TYPE_MAP["ffmpeg"].get(universal_session_type, "mtl_st20p")

    def _convert_to_gstreamer_element(self, universal_session_type: str, direction: str) -> str:
        """Convert universal session type to GStreamer element name."""
        base_name = SESSION_TYPE_MAP["gstreamer"].get(universal_session_type, "mtl_st20p")
        return f"{base_name}_{direction}"

    def _convert_framerate_to_video_format(self, framerate: str) -> str:
        """Convert framerate string (p60, p59, etc.) to RxTxApp video format names."""
        return FRAMERATE_TO_VIDEO_FORMAT_MAP.get(framerate, "i1080p60")

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
        
        Args:
            build: Build directory path
            test_time: Test duration in seconds
            host: Single host object (for single host tests)
            tx_host: TX host object (for dual host tests)
            rx_host: RX host object (for dual host tests)
            input_file: Input file path (for validation)
            output_file: Output file path (for validation)
            fail_on_error: Whether to fail on errors
            virtio_user: Enable virtio-user mode (RxTxApp only)
            rx_timing_parser: Enable RX timing parser (RxTxApp only)
            ptp: Enable PTP (RxTxApp only)
            capture_cfg: Packet capture configuration
            sleep_interval: Sleep interval between starting processes
            tx_first: Whether to start TX first
            output_format: Output format for validation
            **kwargs: Additional framework-specific arguments
            
        Returns:
            True if test passed, False otherwise
        """
        
        # Determine if this is a dual host test
        is_dual = tx_host is not None and rx_host is not None
        app_type = self.app_framework.lower()
        
        if is_dual:
            logger.info(f"Executing dual host {app_type} test")
            tx_remote_host = tx_host
            rx_remote_host = rx_host
        else:
            logger.info(f"Executing single host {app_type} test")
            tx_remote_host = rx_remote_host = host
        
        # Get test case ID for logging
        case_id = self._get_case_id()
        logger.info(f"Test case: {case_id}")
        
        # Prepare commands based on framework
        if app_type == 'rxtxapp':
            # For RxTxApp, create command and config with test-specific parameters
            original_test_time = self.universal_params.get("test_time")
            original_virtio = self.universal_params.get("virtio_user")
            original_timing = self.universal_params.get("rx_timing_parser")
            original_ptp = self.universal_params.get("enable_ptp")
            
            # Set test parameters
            self.universal_params["test_time"] = test_time
            self.universal_params["virtio_user"] = virtio_user
            self.universal_params["rx_timing_parser"] = rx_timing_parser
            self.universal_params["enable_ptp"] = ptp
            
            # Create command and config
            tx_cmd, config = self.create_command()
            
            rx_cmd = tx_cmd  # RxTxApp uses same command for both TX and RX
            
            # Restore original parameters
            if original_test_time is not None:
                self.universal_params["test_time"] = original_test_time
            if original_virtio is not None:
                self.universal_params["virtio_user"] = original_virtio
            if original_timing is not None:
                self.universal_params["rx_timing_parser"] = original_timing
            if original_ptp is not None:
                self.universal_params["enable_ptp"] = original_ptp
        else:
            # For FFmpeg/GStreamer, create both TX and RX commands
            original_direction = self.universal_params.get("direction")
            
            # Create TX command
            self.universal_params["direction"] = "tx"
            tx_cmd, _ = self.create_command()
            
            # Create RX command
            self.universal_params["direction"] = "rx"
            rx_cmd, _ = self.create_command()
            
            # Restore original direction
            if original_direction is not None:
                self.universal_params["direction"] = original_direction
            
            config = None
            
        logger.info(f"TX Command: {tx_cmd}")
        logger.info(f"RX Command: {rx_cmd}")
        
        # Initialize process variables
        tx_process = None
        rx_process = None
        
        # Prepare packet capture
        if is_dual:
            tcpdump = prepare_tcpdump(capture_cfg, tx_host) if capture_cfg else None
        else:
            tcpdump = prepare_tcpdump(capture_cfg, host)
        
        try:
            # For RxTxApp, use synchronous execution like the working implementation
            if app_type == 'rxtxapp':
                if tcpdump:
                    logger.info("Starting packet capture...")
                    tcpdump.start()
                
                logger.info("Starting RxTxApp process...")
                # Use synchronous execution for RxTxApp (like working implementation)
                tx_process = run(
                    tx_cmd,
                    cwd=build,
                    timeout=test_time + 90,
                    testcmd=True,
                    host=tx_remote_host,
                    background=False  # Synchronous execution
                )
                rx_process = None
                
            else:
                # For FFmpeg/GStreamer, use asynchronous execution
                # Start processes based on tx_first flag
                if tx_first:
                    if tx_cmd:
                        logger.info("Starting TX process...")
                        tx_process = self._start_process(tx_cmd, build, test_time, tx_remote_host)
                        time.sleep(sleep_interval)
                    
                    if rx_cmd:  # RxTxApp handled above
                        logger.info("Starting RX process...")
                        rx_process = self._start_process(rx_cmd, build, test_time, rx_remote_host)
                else:
                    if rx_cmd:
                        logger.info("Starting RX process...")
                        rx_process = self._start_process(rx_cmd, build, test_time, rx_remote_host)
                        time.sleep(sleep_interval)
                    
                    if tx_cmd:
                        logger.info("Starting TX process...")
                        tx_process = self._start_process(tx_cmd, build, test_time, tx_remote_host)
                
                # Start tcpdump after processes are running
                if tcpdump:
                    logger.info("Starting packet capture...")
                    tcpdump.start()
                
                # Let the test run for the specified duration
                logger.info(f"Running test for {test_time} seconds...")
                time.sleep(test_time)
            
            # Terminate processes
            # For RxTxApp with synchronous execution, process should already be completed
            if app_type == 'rxtxapp':
                logger.info("RxTxApp synchronous execution completed")
                # No need to terminate - process already finished naturally
            else:
                # For asynchronous processes, terminate them
                if rx_process:
                    logger.info("Terminating RX process...")
                    try:
                        rx_process.stop(wait=2)
                    except Exception as e:
                        logger.warning(f"Failed to stop RX process gracefully: {e}")
                        try:
                            rx_process.kill(wait=2)
                        except Exception as e2:
                            logger.error(f"Failed to kill RX process: {e2}")

                if tx_process:
                    logger.info("Terminating TX process...")
                    try:
                        tx_process.stop(wait=2)
                    except Exception as e:
                        logger.warning(f"Failed to stop TX process gracefully: {e}")
                        try:
                            tx_process.kill(wait=2)
                        except Exception as e2:
                            logger.error(f"Failed to kill TX process: {e2}")
            
            # Check if processes are still running
            if tx_process and is_process_running(tx_process):
                logger.warning("TX process still running after termination attempt")
            
            if rx_process and is_process_running(rx_process):
                logger.warning("RX process still running after termination attempt")
            
            # Capture outputs
            try:
                tx_output = self._capture_stdout(tx_process, "TX") if tx_process else ""
                rx_output = self._capture_stdout(rx_process, "RX") if rx_process else ""
                # For RxTxApp, use the same output for both TX and RX validation
                if app_type == 'rxtxapp':
                    rx_output = tx_output
                
                # Save full output to files for debugging
                import os
                debug_dir = "/tmp/mtl_debug"
                os.makedirs(debug_dir, exist_ok=True)
                
                with open(f"{debug_dir}/tx_output.log", "w") as f:
                    f.write(tx_output)
                
                if rx_output and app_type != 'rxtxapp':
                    with open(f"{debug_dir}/rx_output.log", "w") as f:
                        f.write(rx_output)
                
                logger.info(f"Full TX output saved to {debug_dir}/tx_output.log")
                if app_type != 'rxtxapp':
                    logger.info(f"Full RX output saved to {debug_dir}/rx_output.log")
                
            except Exception as e:
                logger.warning(f"Error capturing process outputs: {e}")
                tx_output = rx_output = ""
            
        except Exception as e:
            log_fail(f"Error during test execution: {e}")
            if tx_process:
                tx_process.terminate()
            if rx_process:
                rx_process.terminate()
            raise
        finally:
            # Cleanup
            if tx_process:
                try:
                    tx_process.terminate()
                except:
                    pass
            if rx_process:
                try:
                    rx_process.terminate()
                except:
                    pass
            if tcpdump:
                try:
                    tcpdump.stop()
                except:
                    pass
        
        # Validate results based on framework
        if app_type == 'rxtxapp':
            return self._validate_rxtxapp_results(config, tx_output, rx_output, 
                                                fail_on_error, tx_remote_host, build)
        elif app_type == 'ffmpeg':
            return self._validate_ffmpeg_results(input_file, output_file, output_format,
                                                tx_remote_host, rx_remote_host, build)
        elif app_type == 'gstreamer':
            return self._validate_gstreamer_results(input_file, output_file,
                                                   tx_remote_host, rx_remote_host)
        
        return True

    def _start_process(self, command: str, build: str, test_time: int, host):
        """Start a process with the given command."""
        return run(
            command,
            cwd=build,
            timeout=test_time + 60,
            testcmd=True,
            host=host,
            background=True,
        )

    def _capture_stdout(self, process, process_name: str) -> str:
        """Capture stdout from a process."""
        if not process:
            return ""
        
        try:
            if hasattr(process, 'stdout_text'):
                output = process.stdout_text
                if output and output.strip():
                    logger.debug(f"{process_name} output: {output[:500]}...")  # Log first 500 chars
                return output or ""
            else:
                logger.debug(f"No stdout available for {process_name}")
                return ""
        except Exception as e:
            logger.warning(f"Error retrieving {process_name} output: {e}")
            return ""

    def _validate_rxtxapp_results(self, config: dict, tx_output: str, rx_output: str,
                                 fail_on_error: bool, host, build: str) -> bool:
        """Validate RxTxApp test results."""
        try:
            from .RxTxApp import check_tx_output, check_rx_output
        except ImportError:
            from RxTxApp import check_tx_output, check_rx_output
        
        # Determine session type from config
        session_type = self._get_session_type_from_config(config)
        
        # Check TX output - split into lines for proper regex matching
        tx_output_lines = tx_output.split('\n') if tx_output else []
        tx_result = check_tx_output(
            config=config,
            output=tx_output_lines,
            session_type=session_type,
            fail_on_error=fail_on_error,
            host=host,
            build=build
        )
        
        # Check RX output - split into lines for proper regex matching
        rx_output_lines = rx_output.split('\n') if rx_output else []
        rx_result = check_rx_output(
            config=config,
            output=rx_output_lines,
            session_type=session_type,
            fail_on_error=fail_on_error,
            host=host,
            build=build
        )
        
        return tx_result and rx_result

    def _validate_ffmpeg_results(self, input_file: str, output_file: str, output_format: str,
                                tx_host, rx_host, build: str) -> bool:
        """Validate FFmpeg test results."""
        if not output_file:
            logger.warning("No output file specified for validation")
            return True
        
        if output_format == "yuv":
            try:
                from .ffmpeg_app import check_output_video_yuv
            except ImportError:
                from ffmpeg_app import check_output_video_yuv
            return check_output_video_yuv(output_file, rx_host, build, input_file)
        elif output_format == "h264":
            try:
                from .ffmpeg_app import check_output_video_h264
            except ImportError:
                from ffmpeg_app import check_output_video_h264
            # Extract video size from universal params
            video_size = f"{self.universal_params.get('width', 1920)}x{self.universal_params.get('height', 1080)}"
            return check_output_video_h264(output_file, video_size, rx_host, build, input_file)
        else:
            logger.warning(f"Unknown output format: {output_format}")
            return True

    def _validate_gstreamer_results(self, input_file: str, output_file: str,
                                   tx_host, rx_host) -> bool:
        """Validate GStreamer test results."""
        if not input_file or not output_file:
            logger.warning("Input or output file not specified for validation")
            return True
        
        try:
            from .GstreamerApp import compare_files
        except ImportError:
            from GstreamerApp import compare_files
        return compare_files(input_file, output_file, tx_host, rx_host)

    def _get_session_type_from_config(self, config: dict) -> str:
        """Extract session type from RxTxApp config."""
        # Check TX sessions first
        for session in config.get("tx_sessions", []):
            for session_type in ["st20p", "st22p", "st30p", "video", "audio", "ancillary"]:
                if session.get(session_type):
                    return session_type
        
        # Check RX sessions
        for session in config.get("rx_sessions", []):
            for session_type in ["st20p", "st22p", "st30p", "video", "audio", "ancillary"]:
                if session.get(session_type):
                    return session_type
        
        return "st20p"  # Default

    def _get_case_id(self) -> str:
        """Get test case ID from environment."""
        case_id = os.environ.get("PYTEST_CURRENT_TEST", f"{self.app_framework}_test")
        # Extract the test function name and parameters
        full_case = case_id[:case_id.rfind("(") - 1] if "(" in case_id else case_id
        # Get the test name after the last ::
        test_name = full_case.split("::")[-1]
        return test_name


# Usage Examples and Documentation
"""
UNIFIED APPLICATION INTERFACE WITH INTEGRATED TEST EXECUTION

The Application class now provides both command generation AND test execution!

Method 1: Create application, set parameters, and execute test
    app = Application("RxTxApp", "./build", config_file_path="tx_1v.json")
    app.create_command(
        session_type="st20p",
        direction="tx",
        nic_port="0000:31:01.0",
        source_ip="192.168.30.10",
        destination_ip="239.1.1.1",
        input_file="./test.yuv",
        width=1920,
        height=1080,
        framerate="p59"
    )
    
    # Execute the test using the configured application
    result = app.execute_test(
        build="./build",
        test_time=30,
        host=my_host,
        input_file="./test.yuv",
        output_file="./output.yuv"
    )

Method 2: One-liner for simple tests
    app = Application("FFmpeg", "/usr/bin")
    app.create_command(
        session_type="st20p",
        direction="tx",
        input_file="./video.yuv",
        nic_port="0000:31:01.0",
        source_ip="192.168.30.10",
        destination_ip="239.1.1.1"
    )
    result = app.execute_test(build="./build", host=my_host)

Method 3: Dual host testing
    app = Application("GStreamer", "/usr/bin")
    app.create_command(session_type="st20p", input_file="./video.yuv")
    result = app.execute_test(
        build="./build",
        tx_host=tx_host,
        rx_host=rx_host,
        input_file="./video.yuv",
        output_file="./received.yuv"
    )

Method 4: RxTxApp with advanced options
    app = Application("RxTxApp", "./build")
    app.create_command(
        session_type="st20p",
        direction="tx",
        nic_port="0000:31:01.0",
        virtio_user=True,
        ptp=True
    )
    result = app.execute_test(
        build="./build",
        host=my_host,
        virtio_user=True,
        ptp=True,
        capture_cfg={"enable": True, "interface": "eth0"}
    )

Benefits of integrated approach:
- No parameter duplication between command creation and execution
- Type-safe parameter handling through universal parameter system
- Automatic command generation based on framework type
- Built-in validation for each framework type
- Support for single and dual host configurations
- Integrated packet capture and logging
"""
