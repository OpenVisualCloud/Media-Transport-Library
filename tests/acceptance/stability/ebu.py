# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

"""Capture one stream after the other and get an EBU LIST verdict for each.

One pass of the rotation, for each stream that EBU LIST can read:

1. join the multicast group on the capture netdev (the switch snoops IGMP),
   and drop the group in tc ingress, after the capture tap, so that the IP
   stack gets no packet of it;
2. capture a fixed packet count with netsniff-ng (hardware RX timestamps,
   nanosecond pcap) and read the kernel drop counters of the netdev;
3. upload the pcap, wait for the report, and judge it with the checks of
   ``mtl_engine.pcap_compliance``;
4. delete the pcap from EBU LIST, and on this host unless the verdict failed.

A capture that the netdev dropped packets of gets the verdict ``capture-loss``,
not ``fail``: the fault is in the capture, not in the stream.
"""

from __future__ import annotations

import dataclasses
import json
import logging
import math
import os
import re
import socket
import struct
import subprocess
import time
from typing import Optional

from compliance.compliance_client import PcapComplianceClient, no_verdict_reason
from create_pcap_file.netsniff import _ring_size_mib
from mtl_engine.pcap_compliance import (
    _framerate_mismatch_streams,
    _packing_mismatch_streams,
    _resolution_mismatch_streams,
    _wide_video_streams,
)

from .config import HostConfig
from .workload import FPS, Stream

logger = logging.getLogger(__name__)

CMD_MAX_S = 30  # mtl-capture takes at most 30 s
TC_PREF = 2111  # not 2110: the acceptance suite sweeps that one
JOIN_SETTLE_S = 1.5  # IGMP report -> switch -> port
EBU_AUDIO_ENCODING = {"PCM16": "L16", "PCM24": "L24"}
EBU_SAMPLING = {"48kHz": 48000, "96kHz": 96000}
RX_DROP_COUNTERS = ("rx_dropped", "rx_missed_errors", "rx_fifo_errors")


@dataclasses.dataclass
class Verdict:
    sid: str
    time: float
    verdict: str  # pass | pass-wide | fail | capture-loss | no-stream | error
    reasons: list
    packets: int = 0
    drops: int = 0
    pcap: str = ""
    metrics: dict = dataclasses.field(default_factory=dict)


def _ifindex(dev: str) -> int:
    with open(f"/sys/class/net/{dev}/ifindex", encoding="ascii") as f:
        return int(f.read())


def _drops(dev: str) -> int:
    total = 0
    for c in RX_DROP_COUNTERS:
        try:
            with open(f"/sys/class/net/{dev}/statistics/{c}", encoding="ascii") as f:
                total += int(f.read())
        except OSError:
            pass
    return total


class Join:
    """An IGMP join of one group on one netdev, for the life of the object."""

    def __init__(self, group: str, dev: str):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
        mreqn = struct.pack(
            "4s4si", socket.inet_aton(group), socket.inet_aton("0.0.0.0"), _ifindex(dev)
        )
        self.sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreqn)

    def close(self):
        self.sock.close()


def _tc(dev: str, *args: str) -> subprocess.CompletedProcess:
    return subprocess.run(["tc", *args], capture_output=True, text=True, check=False)


def drop_after_tap(dev: str, group: str) -> None:
    _tc(dev, "qdisc", "add", "dev", dev, "clsact")
    _tc(dev, "filter", "del", "dev", dev, "ingress", "pref", str(TC_PREF))
    r = _tc(
        dev,
        "filter",
        "add",
        "dev",
        dev,
        "ingress",
        "pref",
        str(TC_PREF),
        "protocol",
        "ip",
        "flower",
        "skip_hw",
        "dst_ip",
        group,
        "action",
        "drop",
    )
    if r.returncode:
        logger.warning("tc drop rule for %s on %s: %s", group, dev, r.stderr.strip())


def undo_drop(dev: str) -> None:
    _tc(dev, "filter", "del", "dev", dev, "ingress", "pref", str(TC_PREF))


def media_specific_audio(s: Stream) -> Optional[dict]:
    enc, rate = EBU_AUDIO_ENCODING.get(s.audio_format), EBU_SAMPLING.get(
        s.audio_sampling
    )
    if not enc or not rate:
        return None
    samples = round(rate * float(s.audio_ptime) / 1000)
    return {
        "encoding": enc,
        "sampling": str(rate),
        "number_channels": s.channels,
        "packet_time": f"{samples * 1000 / rate:.6f}",
    }


def _analysis_failures(stream: dict, skip=()) -> list[str]:
    """The name of each EBU LIST analysis of a stream that is not compliant."""
    out = []
    analyses = stream.get("analyses") or {}
    for name, a in analyses.items():
        if name in skip:
            continue
        result = a.get("result") if isinstance(a, dict) else a
        if result not in (None, "compliant", "not_relevant", "disabled"):
            out.append(f"{name}={result}")
    return out


def _metrics(stream: dict) -> dict:
    """The timing numbers of one stream, for the timeline of the report."""
    m = {}
    gva = stream.get("global_video_analysis") or {}
    for k in ("compliance", "trs", "vrx_full", "cinst"):
        if k in gva:
            m[k] = gva[k]
    ms = stream.get("media_specific") or {}
    for k in (
        "width",
        "height",
        "rate",
        "packing_mode",
        "schedule",
        "encoding",
        "sampling",
        "packet_time",
    ):
        if k in ms:
            m[k] = ms[k]
    net = stream.get("network_information") or {}
    for k in (
        "has_extended_header",
        "multicast_address_match",
        "valid_multicast_mac_address",
        "valid_multicast_ip_address",
    ):
        if k in net:
            m[k] = net[k]
    stat = stream.get("statistics") or {}
    for k in (
        "dropped_packet_count",
        "packet_count",
        "frame_count",
        "is_interlaced",
        "retransmitted_packet_count",
    ):
        if k in stat:
            m[k] = stat[k]
    return m


SDP_REANALYSIS_TIMEOUT_S = 30


def sdp_jxsv(s: Stream) -> str:
    """The SDP of a JPEG XS stream: RFC 9134, and ST 2110-22:2022 7.2 Table 1 (p5:
    width, height, TP), 7.3 (p5: b=AS, the average IP bit rate) and 7.4 (p6: the
    frame rate). b=AS is an estimate at 3 bits per pixel (the codestream size of
    the MTL JPEG XS plugin at "speed"); EBU LIST only needs it present."""
    rate = {"p59": "60000/1001", "p50": "50", "p29": "30000/1001", "p25": "25"}.get(
        s.fps, "60000/1001"
    )
    fps = FPS.get(s.fps, 60000 / 1001)
    kbps = math.ceil(s.width * s.height * 3 * fps * 1.04 / 1000)  # + 4 % IP/UDP/RTP
    src = s.sip or "0.0.0.0"
    return "\r\n".join(
        [
            "v=0",
            f"o=- 1 1 IN IP4 {src}",
            f"s=MTL stability {s.sid}",
            "t=0 0",
            f"m=video {s.port} RTP/AVP 114",
            f"c=IN IP4 {s.dip}/64",
            f"b=AS:{kbps}",
            f"a=framerate:{fps:.2f}",
            f"a=source-filter: incl IN IP4 {s.dip} {src}",
            "a=rtpmap:114 jxsv/90000",
            f"a=fmtp:114 packetmode=0;sampling=YCbCr-4:2:2;depth=10;width={s.width};"
            f"height={s.height};exactframerate={rate};colorimetry=BT709;TCS=SDR;"
            "SSN=ST2110-22:2022;TP=2110TPNL",
            "a=mediaclk:direct=0",
            "",
        ]
    )


def judge(s: Stream, report: dict, allow_wide: bool) -> tuple[str, list[str], dict]:
    reason = no_verdict_reason(report)
    if reason:
        return "no-stream", [reason], {}
    streams = report.get("streams") or []
    mine = [
        x
        for x in streams
        if (x.get("network_information") or {}).get("destination_address") == s.dip
    ] or streams
    reasons: list[str] = []
    skipped: list[str] = []
    metrics = _metrics(mine[0]) if mine else {}
    if len(mine) != 1:
        reasons.append(f"EBU LIST found {len(mine)} streams to {s.dip}, not 1")
    sub = {"streams": mine}
    expected_media = {"video": "video", "audio": "audio", "anc": "ancillary_data"}.get(
        s.media
    )
    for x in mine:
        mt = x.get("media_type")
        if expected_media and mt != expected_media:
            reasons.append(f"media_type {mt}, expected {expected_media}")
        reasons += _analysis_failures(x, s.not_judged)
        skipped += [n for n in _analysis_failures(x) if n.split("=")[0] in s.not_judged]
        if x.get("state") not in (None, "analyzed"):
            reasons.append(f"state {x.get('state')}")
    if s.media == "video" and not s.compressed:
        if _resolution_mismatch_streams(sub, s.width, s.height):
            reasons.append(f"resolution is not {s.width}x{s.height}")
        if _framerate_mismatch_streams(sub, s.fps):
            reasons.append(f"rate is not {s.fps}")
        if _packing_mismatch_streams(sub, s.packing):
            reasons.append(f"packing is not {s.packing}")
    timing_judged = "2110_21_vrx" not in s.not_judged
    wide = bool(s.media == "video" and timing_judged and _wide_video_streams(sub))
    nc = report.get("not_compliant_streams") or 0
    if nc and not reasons and not skipped:
        reasons.append(f"not_compliant_streams={nc}")
    if skipped:
        metrics["not_judged"] = skipped
    if reasons:
        return "fail", reasons, metrics
    if wide and not (allow_wide or s.allow_wide):
        return "fail", ["ST 2110-21 compliance is only wide (expected narrow)"], metrics
    return ("pass-wide" if wide else "pass"), [], metrics


class Rotation:
    """The EBU LIST rotation: ``next()`` captures and judges one stream."""

    def __init__(self, host: HostConfig, streams: list[Stream], run_dir: str):
        self.host = host
        self.streams = [s for s in streams if s.ebu]
        self.dev = host.capture.interface
        self.pcap_dir = os.path.join(run_dir, "pcap")
        os.makedirs(self.pcap_dir, exist_ok=True)
        self.i = 0
        self.kept_failed = 0
        self.kept_bytes = 0
        self.client: Optional[PcapComplianceClient] = None
        self.enabled = bool(
            (self.dev or host.capture.command) and host.ebu.ip and self.streams
        )

    def _client(self) -> PcapComplianceClient:
        if self.client is None:
            self.client = PcapComplianceClient(
                self.host.ebu.ip, self.host.ebu.user, self.host.ebu.password()
            )
        return self.client

    def _packets(self, s: Stream) -> int:
        c = self.host.capture
        if s.media == "video":
            return s.capture_packets(c.video_frames / FPS.get(s.fps, 60.0))
        if s.media == "audio":
            return s.capture_packets(c.audio_seconds)
        return s.capture_packets(c.anc_seconds)

    def capture(self, s: Stream) -> tuple[str, int, int, str]:
        """Return (pcap path, packets asked, drops, error text)."""
        n = self._packets(s)
        stamp = time.strftime("%Y%m%d-%H%M%S")
        pcap = os.path.join(self.pcap_dir, f"{s.sid}__{stamp}.pcap")
        if self.host.capture.command:
            return self._capture_command(s, n, pcap)
        join = Join(s.dip, self.dev)
        try:
            drop_after_tap(self.dev, s.dip)
            time.sleep(JOIN_SETTLE_S)
            before = _drops(self.dev)
            timeout = 10 + n / max(1.0, s.packets_per_second()) * 3
            cmd = [
                "netsniff-ng", "--silent", "--in", self.dev, "--out", pcap,
                "-T", "0xa1b23c4d", "--ring-size", f"{_ring_size_mib(n)}MiB",
                "--num", str(n), "-f", f"udp and dst host {s.dip} and dst port {s.port}",
            ]  # fmt: skip
            r = subprocess.run(
                cmd, capture_output=True, text=True, timeout=timeout, check=False
            )
            drops = _drops(self.dev) - before
            err = "" if r.returncode == 0 else (r.stderr or r.stdout).strip()[-400:]
            return pcap, n, drops, err
        except subprocess.TimeoutExpired:
            return (
                pcap,
                n,
                0,
                f"netsniff-ng got fewer than {n} packets in {timeout:.0f} s",
            )
        finally:
            undo_drop(self.dev)
            join.close()

    def _capture_command(
        self, s: Stream, n: int, pcap: str
    ) -> tuple[str, int, int, str]:
        """A capture by another host (a VM host that owns the PHC), in seconds."""
        seconds = min(
            CMD_MAX_S, max(1, math.ceil(n / max(1.0, s.packets_per_second())))
        )
        cmd = self.host.capture.command.format(
            group=s.dip, port=s.port, seconds=seconds, out=pcap
        )
        try:
            r = subprocess.run(
                cmd.split(),
                capture_output=True,
                text=True,
                timeout=seconds + 120,
                check=False,
            )
        except subprocess.TimeoutExpired:
            return pcap, n, 0, f"{cmd}: no answer in {seconds + 120} s"
        out = r.stdout + r.stderr
        counts = {
            k: int(v) for k, v in re.findall(r"\b(packets|dropped|ifdrop)=(\d+)", out)
        }
        drops = counts.get("dropped", 0) + counts.get("ifdrop", 0)
        if not os.path.exists(pcap) or (r.returncode and not drops):
            return pcap, n, drops, f"{cmd}: status {r.returncode}: {out.strip()[-300:]}"
        return pcap, counts.get("packets", n), drops, ""

    def analyze(self, s: Stream, pcap: str) -> dict:
        client = self._client()
        try:
            client.authenticate()
        except Exception:  # token expiry: one more login
            self.client = None
            client = self._client()
        client.pcap_file = pcap
        client.pcap_id = None
        client.upload_pcap()
        report = client.download_report()
        if report and not report.get("streams"):
            report = client.reanalyze(previous=report)
        ms = media_specific_audio(s) if s.media == "audio" else None
        if report and ms:
            report = client.reanalyze_audio(report, ms)
        if report and s.compressed:
            report = self._reanalyze_with_sdp(client, s, report)
        return report or {}

    def _reanalyze_with_sdp(self, client, s: Stream, previous: dict) -> dict:
        """EBU LIST reads a ST 2110-22 stream only with its SDP: the GUI sends it so."""
        url = f"http://{client.ebu_ip}/api/pcap/{client.pcap_id}"
        headers = {"Authorization": f"Bearer {client.token}"}
        r = client.session.patch(
            url,
            headers=headers,
            json={"sdps": [sdp_jxsv(s)], "transport_type": "RTP"},
            proxies=client.proxies,
            timeout=60,
        )
        r.raise_for_status()
        report = client.download_report()
        for _ in range(SDP_REANALYSIS_TIMEOUT_S):
            if (
                report
                and report.get("streams")
                and report["streams"][0].get("media_type") != "unknown"
            ):
                break
            time.sleep(1)
            report = client.download_report()
        return report or previous

    def next(self) -> Optional[Verdict]:
        if not self.enabled:
            return None
        s = self.streams[self.i % len(self.streams)]
        self.i += 1
        t0 = time.time()
        pcap, n, drops, err = self.capture(s)
        if err:
            return self._done(Verdict(s.sid, t0, "error", [err], n, drops, pcap))
        if drops:
            v = Verdict(
                s.sid,
                t0,
                "capture-loss",
                [f"the capture netdev dropped {drops} packets"],
                n,
                drops,
                pcap,
            )
            return self._done(v)
        try:
            report = self.analyze(s, pcap)
        except Exception as e:  # EBU LIST is down: the run goes on
            return self._done(
                Verdict(s.sid, t0, "error", [f"EBU LIST: {e}"], n, drops, pcap)
            )
        verdict, reasons, metrics = judge(s, report, self.host.ebu.allow_wide)
        v = Verdict(s.sid, t0, verdict, reasons, n, drops, pcap, metrics)
        if verdict in ("fail", "no-stream"):
            with open(pcap + ".report.json", "w", encoding="utf-8") as f:
                json.dump(report, f)
        return self._done(v)

    def _done(self, v: Verdict) -> Verdict:
        failed = v.verdict in ("fail", "no-stream")
        if self.client and self.client.pcap_id:
            try:
                self.client.delete_pcap()
            except Exception as e:
                logger.debug("delete pcap: %s", e)
            self.client.pcap_id = None
        size = os.path.getsize(v.pcap) if v.pcap and os.path.exists(v.pcap) else 0
        keep = (
            failed
            and self.kept_failed < self.host.ebu.keep_failed_pcaps
            and self.kept_bytes + size <= self.host.ebu.keep_failed_mb * 2**20
        )
        if keep:
            self.kept_failed += 1
            self.kept_bytes += size
        elif os.path.exists(v.pcap):
            os.unlink(v.pcap)
            v.pcap = ""
        return v
