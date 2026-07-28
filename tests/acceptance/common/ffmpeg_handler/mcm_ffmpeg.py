# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2025 Intel Corporation
# Media Communications Mesh
# based on:
# https://github.com/OpenVisualCloud/Media-Communications-Mesh/blob/main/ffmpeg-plugin/mcm_audio_rx.c
# https://github.com/OpenVisualCloud/Media-Communications-Mesh/blob/main/ffmpeg-plugin/mcm_audio_tx.c

from .ffmpeg_enums import (
    FFmpegAudioRate,
    McmConnectionType,
    McmFAudioFormat,
    McmTransport,
    McmTransportPixelFormat,
    PacketTime,
    matching_sample_rates,
)
from .ffmpeg_io import FFmpegIO


# Memif
class FFmpegMcmMemifAudioIO(FFmpegIO):
    def __init__(
        self,
        channels: int | None = 2,
        sample_rate: int = FFmpegAudioRate.k48.value,
        f: str = McmFAudioFormat.pcm24.value,
        **kwargs,
    ):
        super().__init__(**kwargs)
        self.channels = channels
        self.sample_rate = sample_rate
        self.f = f


class FFmpegMcmMemifVideoIO(FFmpegIO):
    def __init__(
        self,
        f: str = "mcm",
        conn_type: str = McmConnectionType.mpg.value,
        frame_rate: str | None = "25",
        video_size: str | None = "1920x1080",
        pixel_format: str | None = "yuv422p10le",
        **kwargs,
    ):
        super().__init__(**kwargs)
        self.f = f
        self.conn_type = conn_type
        self.video_size = video_size
        self.pixel_format = pixel_format
        self.frame_rate = frame_rate


# ST2110
class FFmpegMcmST2110CommonIO(FFmpegIO):
    def __init__(
        self,
        buf_queue_cap: int | None = 8,
        conn_delay: int | None = 0,
        conn_type: str = McmConnectionType.mpg.value,
        urn: str = "192.168.97.1",
        port: int = 9001,
        socket_name: str | None = None,
        interface_id: int | None = 0,
        **kwargs,
    ):
        super().__init__(**kwargs)
        self.buf_queue_cap = buf_queue_cap
        self.conn_delay = conn_delay
        self.conn_type = conn_type
        self.urn = urn
        self.port = port
        self.socket_name = socket_name
        self.interface_id = interface_id


class FFmpegMcmST2110AudioIO(FFmpegMcmST2110CommonIO):
    def __init__(
        self,
        buf_queue_cap: int | None = 16,
        payload_type: int | None = 111,
        channels: int | None = 2,
        sample_rate: int | None = FFmpegAudioRate.k48.value,
        ptime: str = PacketTime.pt_1ms.value,
        f: str = McmFAudioFormat.pcm24.value,
        **kwargs,
    ):
        super().__init__(**kwargs)
        self.buf_queue_cap = buf_queue_cap
        self.payload_type = payload_type
        self.channels = channels
        if sample_rate and ptime not in matching_sample_rates[sample_rate]:
            raise Exception(
                f"Sample rate {sample_rate} Hz does not work with {ptime} packet time (ptime)."
            )
        self.sample_rate = sample_rate
        self.ptime = ptime
        self.f = f


class FFmpegMcmST2110AudioRx(FFmpegMcmST2110AudioIO):
    def __init__(
        self,
        ip_addr: str = "239.168.68.190",
        mcast_sip_addr: str | None = None,
        **kwargs,
    ):
        super().__init__(**kwargs)
        self.ip_addr = ip_addr
        self.mcast_sip_addr = mcast_sip_addr


class FFmpegMcmST2110AudioTx(FFmpegMcmST2110AudioIO):
    def __init__(self, ip_addr: str = "192.168.96.2", **kwargs):
        super().__init__(**kwargs)
        self.ip_addr = ip_addr


class FFmpegMcmST2110VideoIO(FFmpegMcmST2110CommonIO):
    def __init__(
        self,
        transport: str = McmTransport.st20.value,
        buf_queue_cap: int | None = 8,
        payload_type: int | None = 112,
        transport_pixel_format: str = McmTransportPixelFormat.rfc.value,
        video_size: str | None = "1920x1080",
        pixel_format: str | None = "yuv422p10le",
        frame_rate: str | None = "25",
        f: str = "mcm",
        **kwargs,
    ):
        super().__init__(**kwargs)
        self.transport = transport
        self.buf_queue_cap = buf_queue_cap
        self.payload_type = payload_type
        self.transport_pixel_format = transport_pixel_format
        self.video_size = video_size
        self.pixel_format = pixel_format
        self.frame_rate = frame_rate
        self.f = f


class FFmpegMcmST2110VideoTx(FFmpegMcmST2110VideoIO):
    def __init__(self, ip_addr: str = "192.168.96.2", **kwargs):
        super().__init__(**kwargs)
        self.ip_addr = ip_addr


class FFmpegMcmST2110VideoRx(FFmpegMcmST2110VideoIO):
    def __init__(
        self,
        ip_addr: str = "239.168.68.190",
        mcast_sip_addr: str | None = None,
        **kwargs,
    ):
        super().__init__(**kwargs)
        self.ip_addr = ip_addr
        self.mcast_sip_addr = mcast_sip_addr


# Multipoint Group
class FFmpegMcmMultipointGroupCommonIO(FFmpegIO):
    def __init__(
        self,
        buf_queue_cap: int | None = 64,
        conn_delay: int | None = 0,
        conn_type: str = McmConnectionType.mpg.value,
        **kwargs,
    ):
        super().__init__(**kwargs)
        self.buf_queue_cap = buf_queue_cap
        self.conn_delay = conn_delay
        self.conn_type = conn_type


class FFmpegMcmMultipointGroupAudioIO(FFmpegMcmMultipointGroupCommonIO):
    def __init__(
        self,
        channels: int | None = 2,
        sample_rate: int = FFmpegAudioRate.k48.value,
        ptime: str = PacketTime.pt_1ms.value,
        f: str = McmFAudioFormat.pcm24.value,
        **kwargs,
    ):
        super().__init__(**kwargs)
        self.channels = channels
        self.sample_rate = sample_rate
        self.ptime = ptime
        self.f = f


class FFmpegMcmMultipointGroupVideoIO(FFmpegMcmMultipointGroupCommonIO):
    def __init__(
        self,
        video_size: str = "1920x1080",
        pixel_format: str = "yuv422p10le",
        frame_rate: str = "25",
        f: str = "mcm",
        **kwargs,
    ):
        super().__init__(**kwargs)
        self.video_size = video_size
        self.pixel_format = pixel_format
        self.frame_rate = frame_rate
        self.f = f
