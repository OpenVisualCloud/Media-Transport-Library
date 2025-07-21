# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2025 Intel Corporation
# Media Communications Mesh

from .ffmpeg_enums import FFmpegAudioFormat, FFmpegAudioRate, FFmpegVideoFormat


class FFmpegIO:
    def __init__(
        self,
        read_at_native_rate: bool = False,  # keep reading with given framerate (novalue, true/false)
        stream_loop: int | None = None,  # how many loops (-1 = inf)
        input_path: str | None = None,  # -i, empty = no -i
        output_path: str | None = None,  # "-" or path at the end
        segment: int | bool = False,  # segment the output (True/False)
        **kwargs,
    ):
        self.re = read_at_native_rate
        # Failsafe mechanism for True/False
        if stream_loop is True:
            self.stream_loop = -1
        elif stream_loop is False:
            self.stream_loop = None
        else:
            self.stream_loop = stream_loop
        self.input_path = input_path
        self.output_path = output_path
        self.segment = segment  # can be True/False or int (segment duration in seconds)
        for k, v in kwargs.items():
            setattr(self, k, v)

    def get_items(self) -> dict:
        return self.__dict__

    def get_command(self) -> str:
        response = ""
        for key, value in self.__dict__.items():
            # print(f"key:{key};value:{value};type:{type(value)}")
            if key not in [
                "input_path",
                "output_path",
                "segment",
            ]:  # make sure they are printed at the end
                if value.isinstance(bool) and value == True:
                    response += f" -{key}"
                if type(value) in [int, str, float] and value:
                    response += f" -{key} {value}"
        if self.input_path:
            response += f" -i {self.input_path}"
        if self.segment is not False:
            response += f" -f segment -segment_time {self.segment}"
        if self.output_path:
            response += f" {self.output_path}"
        return response


class FFmpegVideoIO(FFmpegIO):
    def __init__(
        self,
        video_size: str | None = "1920x1080",
        f: str | None = FFmpegVideoFormat.raw.value,  # video format
        pix_fmt: str | None = "yuv422p10le",
        **kwargs,
    ):
        super().__init__(**kwargs)
        self.video_size = video_size
        self.f = f
        self.pix_fmt = pix_fmt


class FFmpegAudioIO(FFmpegIO):
    def __init__(
        self,
        ar: int | None = FFmpegAudioRate.k48.value,  # audio sample rate
        f: str | None = FFmpegAudioFormat.pcm24.value,  # audio format (bit depth)
        ac: int | None = 2,  # audio channels
        **kwargs,
    ):
        super().__init__(**kwargs)
        self.ar = ar
        self.f = f
        self.ac = ac
