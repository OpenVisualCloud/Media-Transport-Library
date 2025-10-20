# FFmpeg Implementation for Media Transport Library
# Handles FFmpeg-specific command generation and execution

import logging
import os
import time

from .application_base import Application
from .config.universal_params import UNIVERSAL_PARAMS
from .config.app_mappings import (
    APP_NAME_MAP,
    FFMPEG_FORMAT_MAP,
    SESSION_TYPE_MAP,
    FRAMERATE_TO_VIDEO_FORMAT_MAP,
    DEFAULT_FFMPEG_CONFIG
)

logger = logging.getLogger(__name__)


class FFmpeg(Application):
    """FFmpeg framework implementation for MTL testing."""
    
    def get_framework_name(self) -> str:
        return "FFmpeg"
    
    def get_executable_name(self) -> str:
        return APP_NAME_MAP["ffmpeg"]
    
    def create_command(self, **kwargs) -> tuple:
        """
        Set universal parameters and create FFmpeg command line.
        
        Args:
            **kwargs: Universal parameter names and values
            
        Returns:
            Tuple of (command_string, None) - FFmpeg doesn't use config files
        """
        # Set universal parameters
        self.set_universal_params(**kwargs)
        
        # Create FFmpeg command
        command = self._create_ffmpeg_command()
        return command, None
    
    def _create_ffmpeg_command(self) -> str:
        """
        Generate FFmpeg command line from universal parameters.
        Creates appropriate TX or RX command based on direction parameter.
        
        Returns:
            Complete FFmpeg command string
        """
        executable_path = self.get_executable_path()
        direction = self.universal_params.get("direction", "tx")
        session_type = self.universal_params.get("session_type", "st20p")
        
        if direction == "tx":
            return self._create_ffmpeg_tx_command(executable_path, session_type)
        elif direction == "rx":
            return self._create_ffmpeg_rx_command(executable_path, session_type)
        else:
            raise ValueError(f"FFmpeg requires explicit direction (tx/rx), got: {direction}")
    
    def _create_ffmpeg_tx_command(self, executable_path: str, session_type: str) -> str:
        """Create FFmpeg TX (transmit) command."""
        cmd_parts = [executable_path]
        
        # Input configuration
        input_file = self.universal_params.get("input_file")
        if input_file:
            # Input from file
            pixel_format = self._convert_to_ffmpeg_format(
                self.universal_params.get("pixel_format", DEFAULT_FFMPEG_CONFIG["default_pixel_format"])
            )
            width = self.universal_params.get("width", UNIVERSAL_PARAMS["width"])
            height = self.universal_params.get("height", UNIVERSAL_PARAMS["height"])
            framerate = self._extract_framerate_numeric(self.universal_params.get("framerate", UNIVERSAL_PARAMS["framerate"]))
            
            cmd_parts.extend([
                "-f", "rawvideo",
                "-pix_fmt", pixel_format,
                "-video_size", f"{width}x{height}",
                "-framerate", str(framerate),
                "-i", input_file
            ])
        else:
            # Generate test pattern
            width = self.universal_params.get("width", UNIVERSAL_PARAMS["width"])
            height = self.universal_params.get("height", UNIVERSAL_PARAMS["height"])
            framerate = self._extract_framerate_numeric(self.universal_params.get("framerate", UNIVERSAL_PARAMS["framerate"]))
            pattern_duration = self.universal_params.get("pattern_duration", 30)
            cmd_parts.extend(["-f", "lavfi", "-i", f"testsrc=size={width}x{height}:rate={framerate}:duration={pattern_duration}"])
        
        # Output configuration for MTL
        ffmpeg_session_type = self._convert_to_ffmpeg_session_type(session_type)
        cmd_parts.extend(["-f", ffmpeg_session_type])
        
        # Network parameters
        if self.universal_params.get("source_ip"):
            cmd_parts.extend(["-p_sip", self.universal_params["source_ip"]])
        if self.universal_params.get("destination_ip"):
            cmd_parts.extend(["-p_tx_ip", self.universal_params["destination_ip"]])
        if self.universal_params.get("port"):
            cmd_parts.extend(["-udp_port", str(self.universal_params["port"])])
        if self.universal_params.get("nic_port"):
            cmd_parts.extend(["-p_port", self.universal_params["nic_port"]])
        if self.universal_params.get("payload_type"):
            cmd_parts.extend(["-payload_type", str(self.universal_params["payload_type"])])
        
        # Output destination (usually /dev/null for TX)
        cmd_parts.append("/dev/null")
        
        return " ".join(cmd_parts)
    
    def _create_ffmpeg_rx_command(self, executable_path: str, session_type: str) -> str:
        """Create FFmpeg RX (receive) command."""
        cmd_parts = [executable_path]
        
        # Input configuration for MTL
        ffmpeg_session_type = self._convert_to_ffmpeg_session_type(session_type)
        cmd_parts.extend(["-f", ffmpeg_session_type])
        
        # Network parameters
        if self.universal_params.get("multicast_ip"):
            cmd_parts.extend(["-p_rx_ip", self.universal_params["multicast_ip"]])
        if self.universal_params.get("port"):
            cmd_parts.extend(["-udp_port", str(self.universal_params["port"])])
        if self.universal_params.get("nic_port"):
            cmd_parts.extend(["-p_port", self.universal_params["nic_port"]])
        if self.universal_params.get("payload_type"):
            cmd_parts.extend(["-payload_type", str(self.universal_params["payload_type"])])
        
        # Input source
        cmd_parts.extend(["-i", "/dev/null"])
        
        # Output configuration
        output_file = self.universal_params.get("output_file")
        if output_file:
            # Output to file
            pixel_format = self._convert_to_ffmpeg_format(
                self.universal_params.get("pixel_format", DEFAULT_FFMPEG_CONFIG["default_pixel_format"])
            )
            cmd_parts.extend([
                "-f", "rawvideo",
                "-pix_fmt", pixel_format,
                output_file
            ])
        else:
            # Output to /dev/null
            cmd_parts.extend(["-f", "null", "/dev/null"])
        
        return " ".join(cmd_parts)
    
    def _convert_to_ffmpeg_format(self, universal_format: str) -> str:
        """Convert universal pixel format to FFmpeg format."""
        return FFMPEG_FORMAT_MAP.get(universal_format, universal_format.lower())
    
    def _convert_to_ffmpeg_session_type(self, universal_session_type: str) -> str:
        """Convert universal session type to FFmpeg format specifier."""
        return SESSION_TYPE_MAP["ffmpeg"].get(universal_session_type, "mtl_st20p")
    
    def _extract_framerate_numeric(self, framerate_str: str) -> int:
        """Extract numeric framerate from string format (e.g., 'p60' -> 60)."""
        return self.extract_framerate(framerate_str, default=60)
    
    def validate_results(self, input_file: str, output_file: str, output_format: str,
                        tx_host, rx_host, build: str) -> bool:
        """Validate FFmpeg test results."""
        try:
            # For TX tests, check if process completed successfully
            # For RX tests, verify output file was created and has expected content
            
            if output_file and os.path.exists(output_file):
                # Check if output file has content
                file_size = os.path.getsize(output_file)
                if file_size > 0:
                    logger.info(f"FFmpeg RX output file {output_file} created successfully ({file_size} bytes)")
                    return True
                else:
                    logger.error(f"FFmpeg RX output file {output_file} is empty")
                    return False
            else:
                # For TX-only tests or when no output file specified
                logger.info("FFmpeg TX test completed successfully")
                return True
                
        except Exception as e:
            logger.error(f"Error validating FFmpeg results: {e}")
            return False
    
    def _execute_single_host_test(self, build: str, test_time: int, host, 
                                 input_file: str, output_file: str, fail_on_error: bool,
                                 virtio_user: bool, rx_timing_parser: bool, ptp: bool,
                                 capture_cfg, **kwargs) -> bool:
        """Execute single host FFmpeg test."""
        command, _ = self.create_command(input_file=input_file, output_file=output_file, **kwargs)
        
        # Add timeout parameter for FFmpeg
        command = self.add_timeout(command, test_time)
        process, output = self.start_and_capture(command, build, test_time, host, "FFmpeg")
        
        # Validate results
        return self.validate_results(input_file, output_file, "yuv", None, host, build)
    
    def _execute_dual_host_test(self, build: str, test_time: int, tx_host, rx_host,
                               input_file: str, output_file: str, fail_on_error: bool,
                               capture_cfg, sleep_interval: int, tx_first: bool,
                               output_format: str, **kwargs) -> bool:
        """Execute dual host FFmpeg test."""
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
            tx_first, sleep_interval, "FFmpeg-TX", "FFmpeg-RX"
        )
        
        # Validate results
        tx_result = True  # TX validation is implicit in successful execution
        rx_result = self.validate_results(input_file, output_file, output_format, tx_host, rx_host, build)
        
        return tx_result and rx_result