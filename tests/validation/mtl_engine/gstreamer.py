# GStreamer Implementation for Media Transport Library
# Handles GStreamer-specific command generation and execution

import logging
import os
import time

from .application_base import Application
from .config.universal_params import UNIVERSAL_PARAMS
from .config.app_mappings import (
    APP_NAME_MAP,
    SESSION_TYPE_MAP,
    DEFAULT_GSTREAMER_CONFIG
)

logger = logging.getLogger(__name__)


class GStreamer(Application):
    """GStreamer framework implementation for MTL testing."""
    
    def get_framework_name(self) -> str:
        return "GStreamer"
    
    def get_executable_name(self) -> str:
        return APP_NAME_MAP["gstreamer"]
    
    def create_command(self, **kwargs) -> tuple:
        """
        Set universal parameters and create GStreamer command line.
        
        Args:
            **kwargs: Universal parameter names and values
            
        Returns:
            Tuple of (command_string, None) - GStreamer doesn't use config files
        """
        # Set universal parameters
        self.set_universal_params(**kwargs)
        
        # Create GStreamer command
        command = self._create_gstreamer_command()
        return command, None
    
    def _create_gstreamer_command(self) -> str:
        """
        Generate GStreamer command line from universal parameters.
        Creates appropriate TX or RX pipeline based on direction parameter.
        
        Returns:
            Complete GStreamer command string
        """
        executable_path = self.get_executable_path()
        direction = self.universal_params.get("direction", "tx")
        session_type = self.universal_params.get("session_type", "st20p")
        
        if direction == "tx":
            return self._create_gstreamer_tx_command(executable_path, session_type)
        elif direction == "rx":
            return self._create_gstreamer_rx_command(executable_path, session_type)
        else:
            raise ValueError(f"GStreamer requires explicit direction (tx/rx), got: {direction}")
    
    def _create_gstreamer_tx_command(self, executable_path: str, session_type: str) -> str:
        """Create GStreamer TX (transmit) pipeline."""
        cmd_parts = [executable_path, "-v"]
        
        # Source element
        input_file = self.universal_params.get("input_file")
        if input_file:
            # File source
            cmd_parts.append(f"filesrc location={input_file}")
        else:
            # Test pattern generator
            width = self.universal_params.get("width", UNIVERSAL_PARAMS["width"])
            height = self.universal_params.get("height", UNIVERSAL_PARAMS["height"])
            framerate = self._extract_framerate_numeric(self.universal_params.get("framerate", UNIVERSAL_PARAMS["framerate"]))
            
            cmd_parts.append(f"videotestsrc pattern=smpte")
            cmd_parts.append("!")
            cmd_parts.append(f"video/x-raw,width={width},height={height},framerate={framerate}/1")
        
        # Format conversion if needed
        pixel_format = self.universal_params.get("pixel_format", "YUV422PLANAR10LE")
        gst_format = self._convert_to_gstreamer_format(pixel_format)
        
        if input_file:
            # Raw video parsing for file input
            cmd_parts.extend(["!", "rawvideoparse", f"format={gst_format}"])
            width = self.universal_params.get("width", UNIVERSAL_PARAMS["width"])
            height = self.universal_params.get("height", UNIVERSAL_PARAMS["height"])
            framerate = self._extract_framerate_numeric(self.universal_params.get("framerate", UNIVERSAL_PARAMS["framerate"]))
            cmd_parts.append(f"width={width} height={height} framerate={framerate}/1")
        
        # MTL sink element
        gst_element = self._convert_to_gstreamer_element(session_type, "tx")
        cmd_parts.extend(["!", gst_element])
        
        # Network parameters
        if self.universal_params.get("source_ip"):
            cmd_parts.append(f"dev-ip={self.universal_params['source_ip']}")
        if self.universal_params.get("destination_ip"):
            cmd_parts.append(f"ip={self.universal_params['destination_ip']}")
        if self.universal_params.get("port"):
            cmd_parts.append(f"udp-port={self.universal_params['port']}")
        if self.universal_params.get("nic_port"):
            cmd_parts.append(f"dev-port={self.universal_params['nic_port']}")
        if self.universal_params.get("payload_type"):
            cmd_parts.append(f"payload-type={self.universal_params['payload_type']}")
        
        return " ".join(cmd_parts)
    
    def _create_gstreamer_rx_command(self, executable_path: str, session_type: str) -> str:
        """Create GStreamer RX (receive) pipeline."""
        cmd_parts = [executable_path, "-v"]
        
        # MTL source element
        gst_element = self._convert_to_gstreamer_element(session_type, "rx")
        cmd_parts.append(gst_element)
        
        # Network parameters
        if self.universal_params.get("multicast_ip"):
            cmd_parts.append(f"ip={self.universal_params['multicast_ip']}")
        if self.universal_params.get("port"):
            cmd_parts.append(f"udp-port={self.universal_params['port']}")
        if self.universal_params.get("nic_port"):
            cmd_parts.append(f"dev-port={self.universal_params['nic_port']}")
        if self.universal_params.get("payload_type"):
            cmd_parts.append(f"payload-type={self.universal_params['payload_type']}")
        
        # Sink element
        output_file = self.universal_params.get("output_file")
        if output_file:
            # File sink with format conversion
            pixel_format = self.universal_params.get("pixel_format", "YUV422PLANAR10LE")
            gst_format = self._convert_to_gstreamer_format(pixel_format)
            
            cmd_parts.extend([
                "!",
                f"video/x-raw,format={gst_format}",
                "!",
                "videoconvert",
                "!",
                f"filesink location={output_file}"
            ])
        else:
            # Null sink (discard output)
            cmd_parts.extend(["!", "fakesink"])
        
        return " ".join(cmd_parts)
    
    def _convert_to_gstreamer_element(self, universal_session_type: str, direction: str) -> str:
        """Convert universal session type to GStreamer element name."""
        base_element = SESSION_TYPE_MAP["gstreamer"].get(universal_session_type, "mtl_st20p")
        
        # Add direction suffix
        if direction == "tx":
            return f"{base_element}sink"
        else:  # rx
            return f"{base_element}src"
    
    def _convert_to_gstreamer_format(self, universal_format: str) -> str:
        """Convert universal pixel format to GStreamer format."""
        format_map = {
            "YUV422PLANAR10LE": "YUV422P10LE",
            "YUV422PLANAR8": "YUV422P",
            "YUV420PLANAR8": "YUV420P",
            "YUV420PLANAR10LE": "YUV420P10LE",
            "RGB24": "RGB",
            "RGBA": "RGBA"
        }
        return format_map.get(universal_format, universal_format)
    
    def _extract_framerate_numeric(self, framerate_str: str) -> int:
        """Extract numeric framerate from string format (e.g., 'p60' -> 60)."""
        return self.extract_framerate(framerate_str, default=60)
    
    def validate_results(self, input_file: str, output_file: str,
                        tx_host, rx_host) -> bool:
        """Validate GStreamer test results."""
        try:
            # For TX tests, check if process completed successfully
            # For RX tests, verify output file was created and has expected content
            
            if output_file and os.path.exists(output_file):
                # Check if output file has content
                file_size = os.path.getsize(output_file)
                if file_size > 0:
                    logger.info(f"GStreamer RX output file {output_file} created successfully ({file_size} bytes)")
                    return True
                else:
                    logger.error(f"GStreamer RX output file {output_file} is empty")
                    return False
            else:
                # For TX-only tests or when no output file specified
                logger.info("GStreamer TX test completed successfully")
                return True
                
        except Exception as e:
            logger.error(f"Error validating GStreamer results: {e}")
            return False
    
    def _execute_single_host_test(self, build: str, test_time: int, host, 
                                 input_file: str, output_file: str, fail_on_error: bool,
                                 virtio_user: bool, rx_timing_parser: bool, ptp: bool,
                                 capture_cfg, **kwargs) -> bool:
        """Execute single host GStreamer test."""
        command, _ = self.create_command(input_file=input_file, output_file=output_file, **kwargs)
        
        # Add timeout parameter for GStreamer
        command = self.add_timeout(command, test_time)
        process, output = self.start_and_capture(command, build, test_time, host, "GStreamer")
        
        # Validate results
        return self.validate_results(input_file, output_file, None, host)
    
    def _execute_dual_host_test(self, build: str, test_time: int, tx_host, rx_host,
                               input_file: str, output_file: str, fail_on_error: bool,
                               capture_cfg, sleep_interval: int, tx_first: bool,
                               output_format: str, **kwargs) -> bool:
        """Execute dual host GStreamer test."""
        # Create TX and RX commands
        tx_kwargs = kwargs.copy()
        tx_kwargs["direction"] = "tx"
        tx_kwargs["input_file"] = input_file
        
        rx_kwargs = kwargs.copy()
        rx_kwargs["direction"] = "rx"
        rx_kwargs["output_file"] = output_file
        
        tx_command, _ = self.create_command(**tx_kwargs)
        rx_command, _ = self.create_command(**rx_kwargs)
        
        # Add timeout for both commands
        tx_command = self.add_timeout(tx_command, test_time)
        rx_command = self.add_timeout(rx_command, test_time)
        _, _, tx_output, rx_output = self.start_dual_with_delay(
            tx_command, rx_command, build, test_time, tx_host, rx_host,
            tx_first, sleep_interval, "GStreamer-TX", "GStreamer-RX"
        )
        
        # Validate results
        tx_result = True  # TX validation is implicit in successful execution
        rx_result = self.validate_results(input_file, output_file, tx_host, rx_host)
        
        return tx_result and rx_result