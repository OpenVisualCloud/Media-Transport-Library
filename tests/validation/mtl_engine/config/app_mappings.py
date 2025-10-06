# Application name mappings and format conversion utilities

# Map framework names to executable names
APP_NAME_MAP = {
    "rxtxapp": "RxTxApp",
    "ffmpeg": "ffmpeg", 
    "gstreamer": "gst-launch-1.0"
}

# Format conversion mappings
FFMPEG_FORMAT_MAP = {
    "YUV422PLANAR10LE": "yuv422p10le",
    "YUV422PLANAR8": "yuv422p",
    "YUV420PLANAR8": "yuv420p",
    "YUV420PLANAR10LE": "yuv420p10le",
    "RGB24": "rgb24",
    "RGBA": "rgba"
}

SESSION_TYPE_MAP = {
    "ffmpeg": {
        "st20p": "mtl_st20p",
        "st22p": "mtl_st22p", 
        "st30p": "mtl_st30p",
        "video": "rawvideo",
        "audio": "pcm_s24le"
    },
    "gstreamer": {
        "st20p": "mtl_st20p",
        "st22p": "mtl_st22p",
        "st30p": "mtl_st30p",
        "video": "mtl_video",
        "audio": "mtl_audio"
    }
}

FRAMERATE_TO_VIDEO_FORMAT_MAP = {
    "p60": "i1080p60",
    "p59": "i1080p59", 
    "p50": "i1080p50",
    "p30": "i1080p30",
    "p29": "i1080p29",
    "p25": "i1080p25",
    "p24": "i1080p24",
    "p23": "i1080p23"
}
