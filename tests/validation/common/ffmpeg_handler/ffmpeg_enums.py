# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2025 Intel Corporation
# Media Communications Mesh

from enum import Enum


# Raw
class FFmpegAudioFormat(Enum):
    "-f for FFmpeg audio"

    pcm8 = "s8"
    pcm16 = "s16be"
    pcm24 = "s24be"


class FFmpegAudioRate(Enum):
    """
    -ar for FFmpeg audio
    -sample_rate for Media Communications Mesh
    """

    k48 = 48000
    k44 = 44100
    k96 = 96000


class FFmpegVideoFormat(Enum):
    "-f for FFmpeg video"

    raw = "rawvideo"
    jxs = "jpegxs"


# Media Transport Library
class MtlPcmFmt(Enum):
    "-pcm_fmt for Media Transport Library audio"

    pcm16 = "pcm16"
    pcm24 = "pcm24"


class MtlFAudioFormat(Enum):  # Tx only
    "-f for Media Transport Library audio"

    pcm16 = "mtl_st30p_pcm16"
    pcm24 = "mtl_st30p"


# Media Communications Mesh
class McmFAudioFormat(Enum):
    "-f for Media Communications Mesh audio"

    pcm16 = "mcm_audio_pcm16"
    pcm24 = "mcm_audio_pcm24"


# Helper to detect potential mismatches between -f and -pcm_fmt
# TODO: Refactor to use a generic class with properties for each audio format
matching_audio_formats = {
    FFmpegAudioFormat.pcm16: {
        "ffmpeg_f": FFmpegAudioFormat.pcm16.value,  # s16be
        "mcm_f": McmFAudioFormat.pcm16.value,  # "mcm_audio_pcm16",
        "mtl_pcm_fmt": MtlPcmFmt.pcm16.value,  # "pcm16",
        "mtl_f": MtlFAudioFormat.pcm16.value,  # "mtl_st30p_pcm16", Tx only
    },
    FFmpegAudioFormat.pcm24: {
        "ffmpeg_f": FFmpegAudioFormat.pcm24.value,  # s24be
        "mcm_f": McmFAudioFormat.pcm24.value,  # "mcm_audio_pcm24",
        "mtl_pcm_fmt": MtlPcmFmt.pcm24.value,  # "pcm24",
        "mtl_f": MtlFAudioFormat.pcm24.value,  # "mtl_st30p",
    },
}


def audio_file_format_to_format_dict(audio_format: str) -> dict:
    if audio_format == "pcm_s16be":
        return matching_audio_formats.get(FFmpegAudioFormat.pcm16, {})
    elif audio_format == "pcm_s24be":
        return matching_audio_formats.get(FFmpegAudioFormat.pcm24, {})
    elif audio_format == "pcm_s8":
        raise Exception(
            "PCM 8 is not supported by Media Communications Mesh FFmpeg plugin!"
        )
    else:
        raise Exception(f"Not expected audio format {audio_format}")


audio_channel_number_to_layout_map = {
    1: "mono",  # FC
    2: "stereo",  # FL+FR
    3: "3.0",  # FL+FR+FC
    4: "quad",  # FL+FR+BL+BR
    5: "5.0",  # FL+FR+FC+BL+BR
    6: "5.1",  # FL+FR+FC+LFE+BL+BR
    7: "7.0",  # FL+FR+FC+BL+BR+SL+SR
    8: "7.1",  # FL+FR+FC+LFE+BL+BR+SL+SR
    16: "hexadecagonal",  # FL+FR+FC+BL+BR+BC+SL+SR+TFL+TFC+TFR+TBL+TBC+TBR+WL+WR
    24: "22.2",  # FL+FR+FC+LFE+BL+BR+FLC+FRC+BC+SL+SR+TC+TFL+TFC+TFR+TBL+TBC+TBR+LFE2+TSL+TSR+BFC+BFL+BFR
    # source: https://trac.ffmpeg.org/wiki/AudioChannelManipulation#Listchannelnamesandstandardchannellayouts
}


def audio_channel_number_to_layout(channels: int) -> str:
    """
    Convert number of audio channels to FFmpeg channel layout.
    """
    return audio_channel_number_to_layout_map.get(channels, "")


# Media Communications Mesh
class PacketTime(Enum):
    "-ptime for Media Communications Mesh audio"

    # sampling rate: 48000/96000 Hz
    pt_1ms = "1ms"
    pt_125us = "125us"
    pt_250us = "250us"
    pt_333us = "333us"
    pt_4ms = "4ms"
    pt_80us = "80us"
    # sampling rate: 44100 Hz
    pt_1_09ms = "1.09ms"
    pt_0_14ms = "0.14ms"
    pt_0_09ms = "0.09ms"


class McmConnectionType(Enum):
    "-conn_type for Media Communications Mesh"

    mpg = "multipoint-group"
    st = "st2110"


class McmTransport(Enum):
    "-transport for Media Communications Mesh"

    st20 = "st2110-20"
    st22 = "st2110-22"
    st30 = "st2110-30"
    # st40 = "st2110-40" # Not implemented


class McmTransportPixelFormat(Enum):
    "-transport_pixel_format for Media Communications Mesh video"

    rfc = "yuv422p10rfc4175"
    yuv422p10le = "yuv422p10le"
    v210 = "v210"


# Helper to detect potential mismatch betweeen -sample_rate/-ar and -ptime
matching_sample_rates = {
    FFmpegAudioRate.k48.value: {
        PacketTime.pt_1ms.value,
        PacketTime.pt_125us.value,
        PacketTime.pt_250us.value,
        PacketTime.pt_333us.value,
        PacketTime.pt_4ms.value,
        PacketTime.pt_80us.value,
    },
    FFmpegAudioRate.k96.value: {
        PacketTime.pt_1ms.value,
        PacketTime.pt_125us.value,
        PacketTime.pt_250us.value,
        PacketTime.pt_333us.value,
        PacketTime.pt_4ms.value,
        PacketTime.pt_80us.value,
    },
    FFmpegAudioRate.k44.value: {
        PacketTime.pt_1_09ms.value,
        PacketTime.pt_0_14ms.value,
        PacketTime.pt_0_09ms.value,
    },
}

video_format_matches = {
    # file_format : payload format
    "YUV422PLANAR10LE": "yuv422p10le",
    "YUV422RFC4175PG2BE10": "yuv422p10rfc4175",
}


def video_file_format_to_payload_format(pixel_format: str) -> str:
    return video_format_matches.get(
        pixel_format, pixel_format
    )  # matched if matches, else original
