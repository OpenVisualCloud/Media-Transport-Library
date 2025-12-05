# Media Transport Library Application Classes
# Direct imports for RxTxApp, FFmpeg, and GStreamer implementations

# Import the specific application classes
from .application_base import Application as BaseApplication
from .rxtxapp import RxTxApp
from .ffmpeg import FFmpeg
from .gstreamer import GStreamer

# Export all classes for direct use
__all__ = [
    'BaseApplication',  # Abstract base class
    'RxTxApp',         # RxTxApp implementation
    'FFmpeg',          # FFmpeg implementation  
    'GStreamer',       # GStreamer implementation
]

# For convenience, you can also import Application as the base class
Application = BaseApplication