# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2025 Intel Corporation
# Media Communications Mesh
# based on:
# https://github.com/OpenVisualCloud/Media-Transport-Library/blob/main/ecosystem/ffmpeg_plugin/mtl_common.h

from .ffmpeg_enums import FFmpegAudioRate, MtlFAudioFormat, MtlPcmFmt, PacketTime
from .ffmpeg_io import FFmpegIO


class FFmpegMtlCommonIO(FFmpegIO):
    def __init__(
        self,
        # MTL_?X_DEV_ARGS
        p_port: str | None = None,
        p_sip: str | None = None,
        dma_dev: str | None = None,
        rx_queues: int | None = -1,
        tx_queues: int | None = -1,
        # MTL_?X_PORT_ARGS
        udp_port: int = 20000,
        payload_type: int = 112,
        **kwargs,
    ):
        super().__init__(**kwargs)
        self.p_port = p_port
        self.p_sip = p_sip
        self.dma_dev = dma_dev
        self.rx_queues = rx_queues
        self.tx_queues = tx_queues
        self.udp_port = udp_port
        self.payload_type = payload_type


class FFmpegMtlCommonRx(FFmpegMtlCommonIO):
    def __init__(
        self,
        p_rx_ip: str | None = None,
        **kwargs,
    ):
        super().__init__(**kwargs)
        self.p_rx_ip = p_rx_ip
        self.f = "mtl_st30p"


class FFmpegMtlCommonTx(FFmpegMtlCommonIO):
    def __init__(
        self,
        p_tx_ip: str | None = None,
        **kwargs,
    ):
        super().__init__(**kwargs)
        self.p_tx_ip = p_tx_ip
        self.pcm_fmt = None


# for Media Transport Library based on AVOption elements from
# https://github.com/OpenVisualCloud/Media-Transport-Library/blob/main/ecosystem/ffmpeg_plugin/mtl_st20p_rx.c
# each has MTL_RX_DEV_ARGS and MTL_RX_PORT_ARGS + the options provided in AVOption elements
class FFmpegMtlSt20pRx(FFmpegMtlCommonRx):
    def __init__(
        self,
        video_size: str = "1920x1080",
        pixel_format: str = "yuv422p10le",
        fps: str = "59.94",
        timeout_s: int = 0,
        init_retry: int = 5,
        fb_cnt: int = 3,
        gpu_direct: bool = False,
        gpu_driver: int = 0,
        gpu_device: int = 0,
        **kwargs,
    ):
        super().__init__(**kwargs)
        self.video_size = video_size
        self.pixel_format = pixel_format
        self.fps = fps
        self.timeout_s = timeout_s
        self.init_retry = init_retry
        self.fb_cnt = fb_cnt
        self.gpu_direct = gpu_direct
        self.gpu_driver = gpu_driver
        self.gpu_device = gpu_device
        self.f = "mtl_st20p"  # constant


# https://github.com/OpenVisualCloud/Media-Transport-Library/blob/main/ecosystem/ffmpeg_plugin/mtl_st20p_tx.c
class FFmpegMtlSt20pTx(FFmpegMtlCommonTx):
    def __init__(self, fb_cnt: int = 3, **kwargs):
        super().__init__(**kwargs)
        self.fb_cnt = fb_cnt
        self.f = "mtl_st20p"  # constant


# for Media Transport Library based on AVOption elements from
# https://github.com/OpenVisualCloud/Media-Transport-Library/blob/main/ecosystem/ffmpeg_plugin/mtl_st22p_rx.c
# each has MTL_RX_DEV_ARGS and MTL_RX_PORT_ARGS + the options provided in AVOption elements
class FFmpegMtlSt22pRx(FFmpegMtlCommonRx):
    def __init__(
        self,
        video_size: str = "1920x1080",
        pixel_format: str = "yuv422p10le",
        fps: float = 59.94,
        timeout_s: int = 0,
        init_retry: int = 5,
        fb_cnt: int = 3,
        codec_thread_cnt: int = 0,
        st22_codec: str | None = None,
        **kwargs,
    ):
        super().__init__(**kwargs)
        self.video_size = video_size
        self.pixel_format = pixel_format
        self.fps = fps
        self.timeout_s = timeout_s
        self.init_retry = init_retry
        self.fb_cnt = fb_cnt
        self.codec_thread_cnt = codec_thread_cnt
        self.st22_codec = st22_codec
        self.f = "mtl_st22p"  # constant


# https://github.com/OpenVisualCloud/Media-Transport-Library/blob/main/ecosystem/ffmpeg_plugin/mtl_st22p_tx.c
class FFmpegMtlSt22pTx(FFmpegMtlCommonTx):
    def __init__(
        self,
        fb_cnt: int = 3,
        bpp: float = 3.0,
        codec_thread_cnt: int = 0,
        st22_codec: str | None = None,
        **kwargs,
    ):
        super().__init__(**kwargs)
        self.fb_cnt = fb_cnt
        self.bpp = bpp
        self.codec_thread_cnt = codec_thread_cnt
        self.st22_codec = st22_codec
        self.f = "mtl_st22p"  # constant


# for Media Transport Library based on AVOption elements from
# https://github.com/OpenVisualCloud/Media-Transport-Library/blob/main/ecosystem/ffmpeg_plugin/mtl_st30p_rx.c
# each has MTL_RX_DEV_ARGS and MTL_RX_PORT_ARGS + the options provided in AVOption elements
class FFmpegMtlSt30pRx(FFmpegMtlCommonRx):
    def __init__(
        self,
        fb_cnt: int = 3,
        timeout_s: int = 0,
        init_retry: int = 5,
        sample_rate: int = FFmpegAudioRate.k48.value,
        channels: int = 2,
        pcm_fmt: str = MtlPcmFmt.pcm24.value,
        ptime: str = PacketTime.pt_1ms.value,
        **kwargs,
    ):
        super().__init__(**kwargs)
        self.fb_cnt = fb_cnt
        self.timeout_s = timeout_s
        self.init_retry = init_retry
        self.sample_rate = sample_rate
        self.channels = channels
        self.pcm_fmt = pcm_fmt
        self.ptime = ptime


# https://github.com/OpenVisualCloud/Media-Transport-Library/blob/main/ecosystem/ffmpeg_plugin/mtl_st30p_tx.c
class FFmpegMtlSt30pTx(FFmpegMtlCommonTx):
    def __init__(
        self,
        fb_cnt: int = 3,
        ptime: str = PacketTime.pt_1ms.value,
        f: str = MtlFAudioFormat.pcm24.value,
        **kwargs,
    ):
        super().__init__(**kwargs)
        self.fb_cnt = fb_cnt
        self.ptime = ptime
        self.f = f
