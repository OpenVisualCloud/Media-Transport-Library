# Application name mappings and format conversion utilities

# Map framework names to executable names
APP_NAME_MAP = {"rxtxapp": "RxTxApp", "ffmpeg": "ffmpeg", "gstreamer": "gst-launch-1.0"}

# Format conversion mappings
FFMPEG_FORMAT_MAP = {
    "YUV422PLANAR10LE": "yuv422p10le",
    "YUV422PLANAR8": "yuv422p",
    "YUV420PLANAR8": "yuv420p",
    "YUV420PLANAR10LE": "yuv420p10le",
    "RGB24": "rgb24",
    "RGBA": "rgba",
    "YUV422RFC4175PG2BE10": "yuv422p10le",  # RFC4175 to planar 10-bit LE
}

SESSION_TYPE_MAP = {
    "ffmpeg": {
        "st20p": "mtl_st20p",
        "st22p": "mtl_st22p",
        "st30p": "mtl_st30p",
        "video": "rawvideo",
        "audio": "pcm_s24le",
    },
    "gstreamer": {
        "st20p": "mtl_st20p",
        "st22p": "mtl_st22p",
        "st30p": "mtl_st30p",
        "video": "mtl_video",
        "audio": "mtl_audio",
    },
}

FRAMERATE_TO_VIDEO_FORMAT_MAP = {
    "p60": "i1080p60",
    "p59": "i1080p59",
    "p50": "i1080p50",
    "p30": "i1080p30",
    "p29": "i1080p29",
    "p25": "i1080p25",
    "p24": "i1080p24",
    "p23": "i1080p23",
}

# Default network configuration values
DEFAULT_NETWORK_CONFIG = {
    "nic_port": "0000:31:01.0",
    "unicast_tx_ip": "192.168.17.101",
    "unicast_rx_ip": "192.168.17.102",
    "multicast_tx_ip": "192.168.17.101",
    "multicast_rx_ip": "192.168.17.102",
    "multicast_destination_ip": "239.168.48.9",
    "default_config_file": "config.json",
}

# Default port configuration by session type
DEFAULT_PORT_CONFIG = {
    "st20p_port": 20000,
    "st22p_port": 20000,
    "st30p_port": 30000,
    "video_port": 20000,
    "audio_port": 30000,
    "ancillary_port": 40000,
    "fastmetadata_port": 40000,
}

# Default payload type configuration by session type
DEFAULT_PAYLOAD_TYPE_CONFIG = {
    "st20p_payload_type": 112,
    "st22p_payload_type": 114,
    "st30p_payload_type": 111,
    "video_payload_type": 112,
    "audio_payload_type": 111,
    "ancillary_payload_type": 113,
    "fastmetadata_payload_type": 115,
}

# Default ST22p-specific configuration
DEFAULT_ST22P_CONFIG = {
    "framerate": "p25",
    "pack_type": "codestream",
    "codec": "JPEG-XS",
    "quality": "speed",
    "codec_threads": 2,
}

# Default FFmpeg configuration
DEFAULT_FFMPEG_CONFIG = {
    "default_pixel_format": "yuv422p10le",
    "default_session_type": "mtl_st20p",
}

# Default GStreamer configuration
DEFAULT_GSTREAMER_CONFIG = {"default_session_type": "mtl_st20p"}

# DSA (Data Streaming Accelerator) device configuration
# Maps NUMA node to DSA device PCI addresses
DSA_DEVICES = {
    "numa0": "0000:6a:01.0",
    "numa1": "0000:e7:01.0",
}
