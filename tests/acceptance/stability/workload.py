# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

"""The stability workload: which processes run, and which streams they send.

A *profile* is a function that gets the host config and gives a ``Plan``: one
``Worker`` for each process and one ``Stream`` for each TX session. Each TX
session has its own multicast group, so a capture filter on the destination
address holds exactly one stream for EBU LIST.
"""

from __future__ import annotations

import dataclasses
import json
import os
from typing import Callable, Optional

from .config import HostConfig

# 59.94 Hz, 50 Hz and 29.97 Hz in the RxTxApp JSON notation, and as rates.
FPS = {"p59": 60000 / 1001, "p50": 50.0, "p29": 30000 / 1001, "p25": 25.0}
FPS_EBU = {"p59": "60000/1001", "p50": "50", "p29": "30000/1001", "p25": "25"}

# Bytes on the wire of one pixel group of each transport format (RFC 4175).
PGROUP = {"YUV_422_10bit": (5, 2), "YUV_422_8bit": (4, 2), "YUV_444_10bit": (15, 4)}

RTP_PAYLOAD_PER_PKT = 1200  # MTL BPM: 1200 bytes of pixel data per packet


@dataclasses.dataclass
class Stream:
    """One TX session on the wire: what EBU LIST must find in its capture."""

    sid: str  # unique name, also the file name of its captures
    media: str  # video | audio | anc | fmd
    api: str  # st20p, st22p, st20, st30p, st30, st40p, st40, st41
    worker: str  # the worker that sends it
    dip: str
    port: int
    user_pacing: bool = False
    width: int = 0
    height: int = 0
    fps: str = ""
    packing: str = ""
    transport_format: str = ""
    compressed: bool = False
    audio_sampling: str = ""
    audio_format: str = ""
    audio_ptime: str = ""
    channels: int = 0
    ebu: bool = True  # False: EBU LIST cannot judge it (ST 2110-41)
    note: str = ""
    sip: str = ""  # the source address, for the SDP of a ST 2110-22 stream
    # The expectations of this stream that differ from the default, each with
    # its reason in the note and in the report: no hidden exception.
    allow_wide: bool = False  # "wide" ST 2110-21 compliance passes
    not_judged: list = dataclasses.field(default_factory=list)  # EBU LIST analyses

    def packets_per_second(self) -> float:
        if self.media == "video" and not self.compressed:
            num, den = PGROUP.get(self.transport_format, (5, 2))
            frame_bytes = self.width * self.height * num / den
            return FPS.get(self.fps, 60.0) * (frame_bytes / RTP_PAYLOAD_PER_PKT + 1)
        if self.media == "video":
            return FPS.get(self.fps, 60.0) * 300  # JPEG XS 1080p: ~300 pkt/frame
        if self.media == "audio":
            return 1000.0 / float(self.audio_ptime or 1)
        return FPS.get(self.fps, 60.0)

    def capture_packets(self, seconds: float) -> int:
        return max(64, int(self.packets_per_second() * seconds))


@dataclasses.dataclass
class Worker:
    """One process of the workload."""

    name: str
    kind: str  # rxtxapp | ffmpeg | gstreamer
    ports: list[str]
    argv: list[str]
    env: dict[str, str] = dataclasses.field(default_factory=dict)
    files: dict[str, str] = dataclasses.field(default_factory=dict)  # name -> text
    lcores: str = ""  # empty: MTL takes free lcores from the manager
    stop_signal: str = "INT"
    sends: list[str] = dataclasses.field(default_factory=list)  # stream ids
    receives: list[str] = dataclasses.field(default_factory=list)
    note: str = ""


@dataclasses.dataclass
class Plan:
    profile: str
    duration_s: int
    workers: list[Worker]
    streams: list[Stream]

    def to_json(self) -> str:
        return json.dumps(dataclasses.asdict(self), indent=2)

    @staticmethod
    def from_json(text: str) -> "Plan":
        d = json.loads(text)
        return Plan(
            profile=d["profile"],
            duration_s=d["duration_s"],
            workers=[Worker(**w) for w in d["workers"]],
            streams=[Stream(**s) for s in d["streams"]],
        )

    def stream(self, sid: str) -> Stream:
        return next(s for s in self.streams if s.sid == sid)


# ---------------------------------------------------------------------------
# RxTxApp JSON
# ---------------------------------------------------------------------------


class RxTxAppConfig:
    """The JSON config of one RxTxApp process: one group for each session."""

    def __init__(self, port: str, ip: str):
        self.cfg = {
            "interfaces": [{"name": port, "ip": ip}],
            "tx_sessions": [],
            "rx_sessions": [],
        }

    def tx(self, json_type: str, dip: str, session: dict) -> None:
        self.cfg["tx_sessions"].append(
            {"dip": [dip], "interface": [0], json_type: [session]}
        )

    def rx(self, json_type: str, group: str, session: dict) -> None:
        self.cfg["rx_sessions"].append(
            {"ip": [group], "interface": [0], json_type: [session]}
        )

    def dumps(self) -> str:
        return json.dumps(self.cfg, indent=2)


def _hdr(port: int, payload_type: int) -> dict:
    return {"replicas": 1, "start_port": port, "payload_type": payload_type}


def st20p_session(
    s: Stream, tx: bool, *, pixel_format: str, url: str = "", exact=False
) -> dict:
    d = {
        **_hdr(s.port, 112),
        "width": s.width,
        "height": s.height,
        "fps": s.fps,
        "interlaced": False,
        "device": "AUTO",
        "pacing": "narrow",
        "packing": s.packing or "BPM",
        "transport_format": s.transport_format,
        "display": False,
        "enable_rtcp": False,
    }
    if tx:
        d["input_format"] = pixel_format
        d["st20p_url"] = url
        if s.user_pacing:
            d["user_pacing"] = True
            if exact:
                # The pacing time of an exact frame is its first packet, not its
                # sampling instant: the RTP time comes from the epoch
                # (--timestamp_epoch), not from the user time.
                d["exact_user_pacing"] = True
            else:
                d["user_timestamp"] = True
    else:
        d["output_format"] = pixel_format
        d["measure_latency"] = True
        d["st20p_url"] = ""
    return d


def st22p_session(s: Stream, tx: bool, *, pixel_format: str, url: str = "") -> dict:
    d = {
        **_hdr(s.port, 114),
        "width": s.width,
        "height": s.height,
        "fps": s.fps,
        "interlaced": False,
        "pack_type": "codestream",
        "codec": "JPEG-XS",
        "device": "AUTO",
        "quality": "speed",
        "codec_thread_count": 2,
        "enable_rtcp": False,
    }
    if tx:
        d["input_format"] = pixel_format
        d["st22p_url"] = url
    else:
        d["output_format"] = pixel_format
        d["display"] = False
        d["measure_latency"] = True
        d["st22p_url"] = ""
    return d


def video_session(s: Stream, tx: bool, *, mode: str, url: str = "") -> dict:
    d = {
        **_hdr(s.port, 112),
        "type": mode,
        "pacing": "narrow",
        "packing": s.packing or "BPM",
        "tr_offset": "default",
        "video_format": f"i{s.height}{s.fps}",
        "pg_format": s.transport_format,
    }
    if tx:
        d["video_url"] = url
        if s.user_pacing:
            d["user_pacing"] = True
    else:
        d["type"] = "frame"
        d["display"] = False
    return d


def st30p_session(s: Stream, tx: bool, *, url: str = "") -> dict:
    d = {
        **_hdr(s.port, 111),
        "audio_format": s.audio_format,
        "audio_channel": ["ST"] if s.channels == 2 else [f"U{s.channels:02d}"],
        "audio_sampling": s.audio_sampling,
        "audio_ptime": s.audio_ptime,
        "audio_url": url if tx else "",
    }
    if tx and s.user_pacing:
        d["user_pacing"] = True
    return d


def audio_session(s: Stream, tx: bool, *, url: str = "") -> dict:
    return {
        **_hdr(s.port, 111),
        "type": "frame",
        "audio_format": s.audio_format,
        "audio_channel": ["M"] * s.channels,
        "audio_sampling": s.audio_sampling,
        "audio_ptime": s.audio_ptime,
        "audio_url": url if tx else "",
    }


def st40p_session(s: Stream, tx: bool, *, url: str = "") -> dict:
    d = {**_hdr(s.port, 113), "fps": s.fps, "interlaced": False, "enable_rtcp": False}
    if tx:
        d["st40p_url"] = url
        if s.user_pacing:
            d["user_pacing"] = True
            d["user_timestamp"] = True
    return d


def anc_session(s: Stream, tx: bool, *, url: str = "", fmd: bool = False) -> dict:
    d = _hdr(s.port, 115 if fmd else 113)
    if tx:
        d.update(
            type="frame",
            ancillary_format="closed_caption",
            ancillary_url=url,
            ancillary_fps=s.fps,
        )
        if s.user_pacing:
            d["user_pacing"] = True
    if fmd:
        d.update(
            fast_metadata=True,
            fastmetadata_data_item_type=1234567,
            fastmetadata_k_bit=0,
            ancillary_url=url,
        )
    return d


# ---------------------------------------------------------------------------
# Process command lines
# ---------------------------------------------------------------------------


def rxtxapp_argv(
    host: HostConfig,
    cfg_name: str,
    duration_s: int,
    lcores: str,
    sha: bool = False,
    quota: bool = False,
) -> list:
    argv = [
        host.rxtxapp,
        "--config_file",
        cfg_name,
        "--test_time",
        str(duration_s),
        "--log_level",
        "info",
        "--tasklet_time",
        "--rx_separate_lcore",
    ]
    if sha:
        argv += ["--video_sha_check"]
    if quota and host.sch_session_quota:
        argv += ["--sch_session_quota", str(host.sch_session_quota)]
    if host.pacing_way:
        argv += ["--pacing_way", host.pacing_way]
    if host.ptp:
        argv += ["--ptp"]
    if lcores:
        argv += ["--lcores", lcores]
    if host.user_time_offset_ns:
        argv += ["--tx_user_time_offset", str(host.user_time_offset_ns)]
    return argv


def ffmpeg_common(host: HostConfig, port: str, ip: str) -> list:
    argv = ["-p_port", port, "-p_sip", ip]
    if host.pacing_way:
        argv += ["-pacing_way", host.pacing_way]
    return argv


# ---------------------------------------------------------------------------
# Profiles
# ---------------------------------------------------------------------------


class _Addr:
    """Multicast groups 239.30.<worker>.<n> and interface IPs 192.168.30.<n>."""

    def __init__(self):
        self.worker = 0
        self.ip_n = 10

    def iface_ip(self) -> str:
        self.ip_n += 1
        return f"192.168.30.{self.ip_n}"

    def groups(self):
        self.worker += 1
        w = self.worker
        n = 0
        while True:
            n += 1
            yield f"239.30.{w}.{n}", 20000 + 2 * n


def _media(host: HostConfig, name: str) -> str:
    return os.path.join(host.media_dir, name)


def profile_full(host: HostConfig, duration_s: int) -> Plan:
    """Every session type MTL has, with every TX API in RxTxApp, FFmpeg and GStreamer.

    RxTxApp "mix" (tx port 0): the pipeline API and the frame/RTP API, all with
    user pacing, the internal converter on the planar and the V210 inputs,
    derive (no converter) on the RFC 4175 input, at 59.94 Hz and at 50 Hz in one
    process. RxTxApp "rx" (rx port 0) receives each stream; RxTxApp "extconv"
    (rx port 1) receives the 1080p59 streams again through the external
    converter plugin. FFmpeg converts its input with swscale (an external
    converter) and sends st20p and st22p; a second FFmpeg receives them.
    GStreamer sends a user-paced st20p, st30p and st40p.
    """
    addr = _Addr()
    tx_ports, rx_ports = host.tx_ports, host.rx_ports
    workers: list[Worker] = []
    streams: list[Stream] = []

    # --- RxTxApp mix: TX -------------------------------------------------
    g = addr.groups()
    mix_ip = addr.iface_ip()
    mix = RxTxAppConfig(tx_ports[0], mix_ip)
    rx = RxTxAppConfig(rx_ports[0], addr.iface_ip())
    ext = RxTxAppConfig(rx_ports[1 % len(rx_ports)], addr.iface_ip())
    mix_sends: list[str] = []

    def add(stream: Stream) -> Stream:
        stream.sip = mix_ip
        streams.append(stream)
        mix_sends.append(stream.sid)
        return stream

    def vid(sid, api, w, h, fps, fmt="YUV_422_10bit", up=True, note=""):
        dip, port = next(g)
        return add(
            Stream(
                sid=sid,
                media="video",
                api=api,
                worker="rxtxapp-mix",
                dip=dip,
                port=port,
                user_pacing=up,
                width=w,
                height=h,
                fps=fps,
                packing="BPM",
                transport_format=fmt,
                note=note,
            )
        )

    # 4K from RFC 4175: the converter runs in the TX thread of the app, and a 4K
    # conversion each 16.7 ms is more than one vCPU of VM 140 does in time
    # ("build timeout frames"). The converters run on the 1080p and 720p streams.
    s = vid(
        "st20p-2160p59-derive", "st20p", 3840, 2160, "p59", note="derive (no converter)"
    )
    mix.tx(
        "st20p",
        s.dip,
        st20p_session(
            s,
            True,
            pixel_format="YUV422RFC4175PG2BE10",
            url=_media(host, "2160p.rfc4175"),
        ),
    )
    rx.rx("st20p", s.dip, st20p_session(s, False, pixel_format="YUV422RFC4175PG2BE10"))

    s = vid("st20p-1080p59-int", "st20p", 1920, 1080, "p59", note="internal converter")
    mix.tx(
        "st20p",
        s.dip,
        st20p_session(
            s,
            True,
            pixel_format="YUV422PLANAR10LE",
            url=_media(host, "1080p.yuv422p10le"),
        ),
    )
    rx.rx("st20p", s.dip, st20p_session(s, False, pixel_format="YUV422PLANAR10LE"))
    ext.rx("st20p", s.dip, st20p_session(s, False, pixel_format="UYVY"))

    s = vid(
        "st20p-1080p59-v210", "st20p", 1920, 1080, "p59", note="internal converter V210"
    )
    mix.tx(
        "st20p",
        s.dip,
        st20p_session(s, True, pixel_format="V210", url=_media(host, "1080p.v210")),
    )
    rx.rx("st20p", s.dip, st20p_session(s, False, pixel_format="V210"))
    ext.rx("st20p", s.dip, st20p_session(s, False, pixel_format="V210"))

    s = vid(
        "st20p-1080p59-exact",
        "st20p",
        1920,
        1080,
        "p59",
        note="derive, exact user pacing",
    )
    mix.tx(
        "st20p",
        s.dip,
        st20p_session(
            s,
            True,
            pixel_format="YUV422RFC4175PG2BE10",
            url=_media(host, "1080p.rfc4175"),
            exact=True,
        ),
    )
    rx.rx("st20p", s.dip, st20p_session(s, False, pixel_format="YUV422RFC4175PG2BE10"))

    s = vid(
        "st20p-1080p50-int", "st20p", 1920, 1080, "p50", note="50 Hz beside 59.94 Hz"
    )
    mix.tx(
        "st20p",
        s.dip,
        st20p_session(
            s,
            True,
            pixel_format="YUV422PLANAR10LE",
            url=_media(host, "1080p.yuv422p10le"),
        ),
    )
    rx.rx("st20p", s.dip, st20p_session(s, False, pixel_format="YUV422PLANAR10LE"))

    s = vid("st20p-720p59-int", "st20p", 1280, 720, "p59", note="internal converter")
    mix.tx(
        "st20p",
        s.dip,
        st20p_session(
            s,
            True,
            pixel_format="YUV422PLANAR10LE",
            url=_media(host, "720p.yuv422p10le"),
        ),
    )
    rx.rx("st20p", s.dip, st20p_session(s, False, pixel_format="YUV422PLANAR10LE"))

    s = vid(
        "st20-1080p59-frame", "st20", 1920, 1080, "p59", note="frame API, user pacing"
    )
    mix.tx(
        "video",
        s.dip,
        video_session(s, True, mode="frame", url=_media(host, "1080p.rfc4175")),
    )
    rx.rx("video", s.dip, video_session(s, False, mode="frame"))

    s = vid("st20-1080p59-rtp", "st20", 1920, 1080, "p59", note="RTP API, user pacing")
    s.not_judged = ["2110_21_vrx", "2110_21_cinst"]
    s.note += (
        "; VRX and Cinst not judged: RxTxApp builds each RTP packet in its own thread on"
        " the shared app CPUs, and a late wake of that thread (up to 374 us on VM 140)"
        " delays the packets; MTL paces what it gets (open item)"
    )
    mix.tx(
        "video",
        s.dip,
        video_session(s, True, mode="rtp", url=_media(host, "1080p.rfc4175")),
    )
    rx.rx("video", s.dip, video_session(s, False, mode="frame"))

    s = vid("st20-720p50-slice", "st20", 1280, 720, "p50", up=False, note="slice API")
    s.not_judged = ["packet_ts_vs_rtp_ts", "2110_21_vrx", "2110_21_cinst"]
    s.note += (
        "; timing not judged: RxTxApp copies the first slice only when MTL asks for"
        " lines (legacy/tx_video_app.c, usleep(1) polling), so the frame starts late"
    )
    mix.tx(
        "video",
        s.dip,
        video_session(s, True, mode="slice", url=_media(host, "720p.rfc4175")),
    )
    rx.rx("video", s.dip, video_session(s, False, mode="frame"))

    if host.st22_codec:
        dip, port = next(g)
        s = add(
            Stream(
                sid="st22p-1080p59-jpegxs",
                media="video",
                api="st22p",
                worker="rxtxapp-mix",
                dip=dip,
                port=port,
                width=1920,
                height=1080,
                fps="p59",
                compressed=True,
                note=f"{host.st22_codec} encoder plugin",
            )
        )
        mix.tx(
            "st22p",
            s.dip,
            st22p_session(
                s,
                True,
                pixel_format="YUV422PLANAR10LE",
                url=_media(host, "1080p.yuv422p10le"),
            ),
        )
        rx.rx("st22p", s.dip, st22p_session(s, False, pixel_format="YUV422PLANAR10LE"))

    for sid, api, fmt, rate, ptime, ch, up in (
        ("st30p-pcm24-48k-1ms", "st30p", "PCM24", "48kHz", "1", 2, True),
        ("st30p-pcm24-96k-0.125", "st30p", "PCM24", "96kHz", "0.125", 2, True),
        ("st30-pcm16-48k-8ch", "st30", "PCM16", "48kHz", "1", 8, False),
    ):
        dip, port = next(g)
        s = add(
            Stream(
                sid=sid,
                media="audio",
                api=api,
                worker="rxtxapp-mix",
                dip=dip,
                port=port,
                user_pacing=up,
                audio_format=fmt,
                audio_sampling=rate,
                audio_ptime=ptime,
                channels=ch,
            )
        )
        url = _media(host, f"audio-{fmt.lower()}-{rate}-{ch}ch.raw")
        if api == "st30p":
            mix.tx("st30p", s.dip, st30p_session(s, True, url=url))
            rx.rx("st30p", s.dip, st30p_session(s, False))
        else:
            mix.tx("audio", s.dip, audio_session(s, True, url=url))
            rx.rx("audio", s.dip, audio_session(s, False))

    dip, port = next(g)
    s = add(
        Stream(
            sid="st40p-p59",
            media="anc",
            api="st40p",
            worker="rxtxapp-mix",
            dip=dip,
            port=port,
            user_pacing=True,
            fps="p59",
        )
    )
    mix.tx("st40p", s.dip, st40p_session(s, True, url=_media(host, "anc.txt")))
    rx.rx("st40p", s.dip, st40p_session(s, False))

    dip, port = next(g)
    s = add(
        Stream(
            sid="st40-p59",
            media="anc",
            api="st40",
            worker="rxtxapp-mix",
            dip=dip,
            port=port,
            user_pacing=True,
            fps="p59",
        )
    )
    mix.tx("ancillary", s.dip, anc_session(s, True, url=_media(host, "anc.txt")))
    rx.rx("ancillary", s.dip, anc_session(s, False))

    dip, port = next(g)
    s = add(
        Stream(
            sid="st41-p59",
            media="fmd",
            api="st41",
            worker="rxtxapp-mix",
            dip=dip,
            port=port,
            fps="p59",
            ebu=False,
            note="EBU LIST does not read ST 2110-41",
        )
    )
    mix.tx(
        "ancillary", s.dip, anc_session(s, True, url=_media(host, "anc.txt"), fmd=True)
    )
    rx.rx(
        "ancillary", s.dip, anc_session(s, False, url=_media(host, "anc.txt"), fmd=True)
    )

    lc = host.lcore_slices
    workers.append(
        Worker(
            name="rxtxapp-mix",
            kind="rxtxapp",
            ports=[tx_ports[0]],
            argv=rxtxapp_argv(
                host, "mix.json", duration_s, lc.get("rxtxapp-mix", ""), quota=True
            )
            + ["--timestamp_epoch"],
            files={
                "mix.json": mix.dumps(),
                "kahawai.json": host.kahawai_json(st22=True),
            },
            lcores=lc.get("rxtxapp-mix", ""),
            sends=list(mix_sends),
            note="TX of every session type, pipeline and frame/RTP API, user pacing",
        )
    )
    workers.append(
        Worker(
            name="rxtxapp-rx",
            kind="rxtxapp",
            ports=[rx_ports[0]],
            argv=rxtxapp_argv(host, "rx.json", duration_s, lc.get("rxtxapp-rx", "")),
            files={"rx.json": rx.dumps(), "kahawai.json": host.kahawai_json(st22=True)},
            lcores=lc.get("rxtxapp-rx", ""),
            receives=list(mix_sends),
            note="RX of each mix session, internal converters",
        )
    )
    workers.append(
        Worker(
            name="rxtxapp-extconv",
            kind="rxtxapp",
            ports=[rx_ports[1 % len(rx_ports)]],
            argv=rxtxapp_argv(
                host, "ext.json", duration_s, lc.get("rxtxapp-extconv", "")
            ),
            files={
                "ext.json": ext.dumps(),
                "kahawai.json": host.kahawai_json(convert=True),
            },
            lcores=lc.get("rxtxapp-extconv", ""),
            receives=["st20p-1080p59-int", "st20p-1080p59-v210"],
            note="RX through the external converter plugin (color_convert_sample)",
        )
    )

    # --- RxTxApp integrity: SHA-256 of each frame, TX -> switch -> RX ------
    if host.integrity and len(tx_ports) > 3:
        g = addr.groups()
        icfg = RxTxAppConfig(tx_ports[3], addr.iface_ip())
        icfg.cfg["interfaces"].append(
            {"name": rx_ports[3 % len(rx_ports)], "ip": addr.iface_ip()}
        )
        isends = []
        for sid, w, h, fmt, url in (
            (
                "integrity-st20p-1080p59",
                1920,
                1080,
                "YUV422PLANAR10LE",
                "1080p.yuv422p10le",
            ),
            (
                "integrity-st20p-1080p59-derive",
                1920,
                1080,
                "YUV422RFC4175PG2BE10",
                "1080p.rfc4175",
            ),
            (
                "integrity-st20p-720p59",
                1280,
                720,
                "YUV422PLANAR10LE",
                "720p.yuv422p10le",
            ),
        ):
            dip, port = next(g)
            s = Stream(
                sid=sid, media="video", api="st20p", worker="rxtxapp-integrity", dip=dip,
                port=port, user_pacing=True, width=w, height=h, fps="p59", packing="BPM",
                transport_format="YUV_422_10bit", ebu=False,
                note="SHA-256 of each frame in a user-meta packet: no EBU LIST verdict",
            )  # fmt: skip
            streams.append(s)
            isends.append(s.sid)
            icfg.tx(
                "st20p",
                s.dip,
                st20p_session(s, True, pixel_format=fmt, url=_media(host, url)),
            )
            icfg.cfg["rx_sessions"].append(
                {
                    "ip": [s.dip],
                    "interface": [1],
                    "st20p": [st20p_session(s, False, pixel_format=fmt)],
                }
            )
        workers.append(
            Worker(
                name="rxtxapp-integrity",
                kind="rxtxapp",
                ports=[tx_ports[3], rx_ports[3 % len(rx_ports)]],
                argv=rxtxapp_argv(
                    host,
                    "integrity.json",
                    duration_s,
                    lc.get("rxtxapp-integrity", ""),
                    sha=True,
                ),
                files={
                    "integrity.json": icfg.dumps(),
                    "kahawai.json": host.kahawai_json(),
                },
                lcores=lc.get("rxtxapp-integrity", ""),
                sends=isends,
                receives=list(isends),
                note="SHA-256 of each frame from TX to RX (--video_sha_check)",
            )
        )

    # --- FFmpeg: swscale (external converter) -> MTL ----------------------
    if host.ffmpeg and len(tx_ports) > 1:
        g = addr.groups()
        tx_ip, rx_ip = addr.iface_ip(), addr.iface_ip()
        dip, port = next(g)
        f20 = Stream(
            sid="ffmpeg-st20p-1080p59",
            media="video",
            api="st20p",
            worker="ffmpeg-tx",
            dip=dip,
            port=port,
            width=1920,
            height=1080,
            fps="p59",
            packing="BPM",
            transport_format="YUV_422_10bit",
            note="FFmpeg swscale rgb24 -> yuv422p10le, MTL internal converter",
        )
        streams.append(f20)
        tx_argv = [
            # No -re: the mtl_* muxers block each frame until MTL takes it.
            host.ffmpeg, "-hide_banner", "-loglevel", "info", "-stats_period", "10",
            # swscale of 1080p RGB -> 4:2:2 10 bit takes about one frame time on one
            # thread: slice threads keep the source at the frame rate.
            "-filter_threads", "4",
            # A raw RGB file, not lavfi testsrc2: the generator, swscale and the
            # JPEG XS encoder in one process made only 0.976x real time on VM 140.
            "-stream_loop", "-1", "-f", "rawvideo", "-pix_fmt", "rgb24",
            "-video_size", "1920x1080", "-framerate", "60000/1001",
            "-i", _media(host, "1080p.rgb24"),
            "-t", str(duration_s),
            "-vf", "format=yuv422p10le",
            *ffmpeg_common(host, tx_ports[1], tx_ip),
            "-p_tx_ip", f20.dip, "-udp_port", str(f20.port), "-payload_type", "112",
            "-f", "mtl_st20p", "-",
        ]  # fmt: skip
        sends = [f20.sid]
        if host.st22_codec and host.ffmpeg_jpegxs:
            dip, port = next(g)
            f22 = Stream(
                sid="ffmpeg-st22p-1080p59",
                media="video",
                api="st22p",
                worker="ffmpeg-tx",
                dip=dip,
                port=port,
                width=1920,
                height=1080,
                fps="p59",
                compressed=True,
                note="FFmpeg libsvtjpegxs encoder, MTL st22 codestream",
            )
            streams.append(f22)
            tx_argv += [
                "-vf", "format=yuv422p10le", "-c:v", "libsvtjpegxs", "-bpp", "3",
                *ffmpeg_common(host, tx_ports[1], tx_ip),
                "-p_tx_ip", f22.dip, "-udp_port", str(f22.port), "-payload_type", "114",
                "-f", "mtl_st22", "-",
            ]  # fmt: skip
            sends.append(f22.sid)
        workers.append(
            Worker(
                name="ffmpeg-tx",
                lcores=lc.get("ffmpeg-tx", ""),
                kind="ffmpeg",
                ports=[tx_ports[1]],
                argv=tx_argv,
                stop_signal="INT",
                sends=sends,
                note="external converter (swscale) and the JPEG XS encoder of FFmpeg",
            )
        )
        rx_argv = [
            host.ffmpeg, "-hide_banner", "-loglevel", "info", "-stats_period", "10",
            *ffmpeg_common(host, rx_ports[2 % len(rx_ports)], rx_ip),
            "-p_rx_ip", f20.dip, "-udp_port", str(f20.port), "-payload_type", "112",
            "-fps", "59.94", "-pix_fmt", "yuv422p10le", "-video_size", "1920x1080",
            "-f", "mtl_st20p", "-i", "k",
            "-t", str(duration_s),
            "-vf", "format=rgb24", "-f", "framemd5", "-y", "ffmpeg-rx-st20p.md5",
        ]  # fmt: skip
        workers.append(
            Worker(
                name="ffmpeg-rx",
                lcores=lc.get("ffmpeg-rx", ""),
                kind="ffmpeg",
                ports=[rx_ports[2 % len(rx_ports)]],
                argv=rx_argv,
                receives=[f20.sid],
                note="RX into swscale (external converter) and framemd5",
            )
        )

    # --- GStreamer: user-paced st20p/st30p/st40p ------------------------
    if host.gst_launch and len(tx_ports) > 2:
        g = addr.groups()
        ip = addr.iface_ip()
        dip, port = next(g)
        g20 = Stream(
            sid="gst-st20p-1080p59",
            media="video",
            api="st20p",
            worker="gst-tx",
            dip=dip,
            port=port,
            user_pacing=True,
            width=1920,
            height=1080,
            fps="p59",
            packing="BPM",
            transport_format="YUV_422_10bit",
            note="GStreamer, user pacing",
        )
        dip, port = next(g)
        g30 = Stream(
            sid="gst-st30p-pcm24-48k",
            media="audio",
            api="st30p",
            worker="gst-tx",
            dip=dip,
            port=port,
            user_pacing=True,
            audio_format="PCM24",
            audio_sampling="48kHz",
            audio_ptime="1",
            channels=2,
            note="GStreamer, user pacing",
        )
        streams += [g20, g30]
        common = f"dev-port={tx_ports[2]} dev-ip={ip}"
        pipeline = (
            "videotestsrc is-live=true pattern=ball "
            "! video/x-raw,format=v210,width=1920,height=1080,framerate=60000/1001 "
            f"! mtl_st20p_tx {common} tx-queues=2 ip={g20.dip} udp-port={g20.port} "
            "payload-type=112 use-pts-for-pacing=true pts-pacing-offset=33366667 "
            f"audiotestsrc is-live=true ! audio/x-raw,format=S24BE,rate=48000,channels=2 "
            f"! mtl_st30p_tx {common} ip={g30.dip} udp-port={g30.port} payload-type=111 "
            "use-pts-for-pacing=true pts-pacing-offset=33366667"
        )
        workers.append(
            Worker(
                name="gst-tx",
                lcores=lc.get("gst-tx", ""),
                kind="gstreamer",
                ports=[tx_ports[2]],
                argv=[host.gst_launch, "-e", *pipeline.split()],
                env=(
                    {"GST_PLUGIN_PATH": host.gst_plugin_path}
                    if host.gst_plugin_path
                    else {}
                ),
                sends=[g20.sid, g30.sid],
                note=(
                    "GStreamer plugin, buffer PTS as the user pacing time; pts-pacing-offset"
                    " 2 frame times: a live buffer reaches the sink after its PTS"
                ),
            )
        )

    return Plan(profile="full", duration_s=duration_s, workers=workers, streams=streams)


def profile_smoke(host: HostConfig, duration_s: int) -> Plan:
    """The RxTxApp part of ``full`` only: a quick check of the host and the tools."""
    plan = profile_full(host, duration_s)
    keep = {"rxtxapp-mix", "rxtxapp-rx", "rxtxapp-extconv", "rxtxapp-integrity"}
    plan.workers = [w for w in plan.workers if w.name in keep]
    plan.streams = [s for s in plan.streams if s.worker in keep]
    plan.profile = "smoke"
    return plan


PROFILES: dict[str, Callable[[HostConfig, int], Plan]] = {
    "full": profile_full,
    "smoke": profile_smoke,
}


def build_plan(
    host: HostConfig, profile: str, duration_s: int, only: Optional[list] = None
) -> Plan:
    plan = PROFILES[profile](host, duration_s)
    owners: dict = {}
    for w in plan.workers:
        for port in w.ports:
            if port in owners and owners[port] != w.name:
                raise ValueError(
                    f"port {port}: both {owners[port]} and {w.name}; give the host more ports"
                )
            owners[port] = w.name
    if only:
        plan.workers = [w for w in plan.workers if w.name in only]
        names = {w.name for w in plan.workers}
        plan.streams = [s for s in plan.streams if s.worker in names]
    return plan
