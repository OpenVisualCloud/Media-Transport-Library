# Framework mappings and configuration
# All parameter mappings, format conversions, and app configuration for RxTxApp, FFmpeg, and GStreamer

# ============================================================================
# RxTxApp Configuration
# ============================================================================

# Map framework names to executable names
APP_NAME_MAP = {
    "rxtxapp": "RxTxApp",
    "ffmpeg": "ffmpeg",
    "gstreamer": "gst-launch-1.0",
}

# RxTxApp command-line parameter mapping
# These parameters are passed as command-line arguments
RXTXAPP_CMDLINE_PARAM_MAP = {
    "config_file": "--config_file",
    "enable_ptp": "--ptp",
    "lcores": "--lcores",
    "test_time": "--test_time",
    "dma_dev": "--dma_dev",
    "log_level": "--log_level",
    "log_file": "--log_file",
    "arp_timeout_s": "--arp_timeout_s",
    "allow_across_numa_core": "--allow_across_numa_core",
    "no_multicast": "--no_multicast",
    "rx_separate_lcore": "--rx_separate_lcore",
    "rx_mix_lcore": "--rx_mix_lcore",
    "runtime_session": "--runtime_session",
    "rx_timing_parser": "--rx_timing_parser",
    "pcapng_dump": "--pcapng_dump",
    "rx_video_file_frames": "--rx_video_file_frames",
    "framebuffer_count": "--rx_video_fb_cnt",
    "promiscuous": "--promiscuous",
    "cni_thread": "--cni_thread",
    "sch_session_quota": "--sch_session_quota",
    "p_tx_dst_mac": "--p_tx_dst_mac",
    "r_tx_dst_mac": "--r_tx_dst_mac",
    "nb_tx_desc": "--nb_tx_desc",
    "nb_rx_desc": "--nb_rx_desc",
    "tasklet_time": "--tasklet_time",
    "tsc": "--tsc",
    "pacing_way": "--pacing_way",
    "shaping": "--shaping",
    "vrx": "--vrx",
    "ts_first_pkt": "--ts_first_pkt",
    "ts_delta_us": "--ts_delta_us",
    "mono_pool": "--mono_pool",
    "tasklet_thread": "--tasklet_thread",
    "tasklet_sleep": "--tasklet_sleep",
    "tasklet_sleep_us": "--tasklet_sleep_us",
    "app_bind_lcore": "--app_bind_lcore",
    "rxtx_simd_512": "--rxtx_simd_512",
    "rss_mode": "--rss_mode",
    "tx_no_chain": "--tx_no_chain",
    "multi_src_port": "--multi_src_port",
    "audio_fifo_size": "--audio_fifo_size",
    "dhcp": "--dhcp",
    "virtio_user": "--virtio_user",
    "phc2sys": "--phc2sys",
    "ptp_sync_sys": "--ptp_sync_sys",
    "rss_sch_nb": "--rss_sch_nb",
    "log_time_ms": "--log_time_ms",
    "rx_audio_dump_time_s": "--rx_audio_dump_time_s",
    "dedicated_sys_lcore": "--dedicated_sys_lcore",
    "bind_numa": "--bind_numa",
    "force_numa": "--force_numa",
}

# ============================================================================
# FFmpeg Configuration
# ============================================================================

# Format conversion mappings for FFmpeg
FFMPEG_FORMAT_MAP = {
    "YUV422PLANAR10LE": "yuv422p10le",
    "YUV422PLANAR8": "yuv422p",
    "YUV420PLANAR8": "yuv420p",
    "YUV420PLANAR10LE": "yuv420p10le",
    "RGB24": "rgb24",
    "RGBA": "rgba",
    "YUV422RFC4175PG2BE10": "yuv422p10le",  # RFC4175 to planar 10-bit LE
}

# FFmpeg parameter mapping
# Maps universal params to FFmpeg MTL plugin flags.
# Width & height both map to -video_size; command builders coalesce them into WxH format.
# Framerate maps to -fps (distinct from input rawvideo's -framerate).
FFMPEG_PARAM_MAP = {
    # Network parameters
    "source_ip": "-p_sip",
    "destination_ip": "-p_tx_ip",  # TX unicast destination
    "multicast_ip": "-p_rx_ip",  # RX multicast group
    "port": "-udp_port",
    "nic_port": "-p_port",
    # Video parameters (width/height combined externally)
    "width": "-video_size",
    "height": "-video_size",
    "framerate": "-fps",
    "pixel_format": "-pix_fmt",
    # Streaming parameters
    "payload_type": "-payload_type",
    "session_type": "-f",  # Converted via SESSION_TYPE_MAP
    # File I/O
    "input_file": "-i",
    "output_file": "",  # Output appears last (no explicit flag)
}

# Default FFmpeg configuration
DEFAULT_FFMPEG_CONFIG = {
    "default_pixel_format": "yuv422p10le",
    "default_session_type": "mtl_st20p",
}

# ============================================================================
# GStreamer Configuration
# ============================================================================

# GStreamer parameter mapping
# Maps universal params to MTL GStreamer element properties.
# Set as name=value pairs in the pipeline.
GSTREAMER_PARAM_MAP = {
    # Network parameters
    "source_ip": "dev-ip",  # Interface IP
    "destination_ip": "ip",  # Destination (unicast) IP
    "port": "udp-port",  # UDP port
    "nic_port": "dev-port",  # NIC device/PCI identifier
    # Video parameters / caps
    # TOFIX: For RX elements, GStreamer uses rx-width/rx-height instead of width/height.
    # This needs to be unified before enabling RX-side GStreamer tests.
    "width": "width",
    "height": "height",
    "framerate": "framerate",
    "pixel_format": "format",
    # Audio parameters
    "audio_format": "audio-format",
    "audio_channels": "channel",
    "audio_sampling": "sampling",
    # Streaming parameters
    "payload_type": "payload-type",
    "queues": "queues",  # Currently legacy / advanced usage
    "framebuffer_count": "framebuff-cnt",
    # File I/O (filesrc/filesink)
    "input_file": "location",
    "output_file": "location",
}

# ============================================================================
# Common Configuration
# ============================================================================

# Session type mapping for FFmpeg and GStreamer
SESSION_TYPE_MAP = {
    "ffmpeg": {
        "st20p": "mtl_st20p",
        "st22p": "mtl_st22p",
        "st30p": "mtl_st30p",
    },
    "gstreamer": {
        "st20p": "mtl_st20p",
        "st22p": "mtl_st22p",
        "st30p": "mtl_st30p",
    },
}
