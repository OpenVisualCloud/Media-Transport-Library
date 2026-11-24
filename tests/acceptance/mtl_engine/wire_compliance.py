# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

"""ST 2110-21 compliance of a sender's video streams, judged by EBU LIST on the wire.

The sender's streams go multicast, a few to a group (rxtxapp.multicast_groups()),
and a PF of the sending host that sends nothing itself captures one group at a time.
phc2sys holds the clock of that NIC to the CLOCK_TAI the sender paces on, so the
hosts need no common clock.  Not a sending port, nor a promiscuous capture: either
stalls the sender's TX.  Besides a tc qdisc and filter of its own, which it removes,
the pass only steers the PF's clock, as the capture fixture does.  The PF's
interrupts stay where the host puts them, which on the perf pair are the sender's
application CPUs.
"""

import base64
import json
import logging
import math
import os
import re
import shlex
import time
from dataclasses import dataclass, field

from compliance.compliance_client import PcapComplianceClient

logger = logging.getLogger(__name__)

# A capture takes the streams of one multicast group, as many as a kernel capture
# took with no drop, four 1080p59, for a second of each, RP 2110-25's window.
CAPTURE_PPS = 950_000
CAPTURE_S = 1
# A capture's wall time at most: tcpdump's timeout, the join before it, the trim.
CAPTURE_WALL_S = 40
# What LIST calls a stream within the type N model, which MTL signals.
COMPLIANT = ("narrow", "narrow_linear")
PHC_LOCK_NS = 2000  # the capture fixture's (conftest.py)
PHC_WAIT_S = 30
RE_PHC = re.compile(r" sys offset +(-?\d+) s(\d) ")  # its servo locked at s2
# Multicast packets a second that a PF which joined nothing may get from elsewhere,
# such as PTP; more is the switch flooding the groups.
FLOOD_PPS = 10_000


@dataclass(frozen=True)
class EbuServer:
    """test_config's ebu_server.

    Its repr leaves out the password, so that a traceback cannot show it.  It is
    false when test_config has no ebu_server.
    """

    ip: str
    user: str
    password: str = field(repr=False)

    @classmethod
    def of(cls, config: dict | None) -> "EbuServer":
        config = config or {}
        return cls(
            config.get("ebu_ip", ""), config.get("user", ""), config.get("password", "")
        )

    def __bool__(self) -> bool:
        return bool(self.ip)


# Captures COUNT packets of GROUP on PF into RAW, keeps each stream's whole frames in
# OUT, and prints the gaps in each stream's RTP sequence that it kept, and the time
# that its kept frames span, as JSON. It
# drops the group after the tap, as NetsniffRecorder does, so the IP stack does not
# hold up the queue. A stream starts at its second RTP marker, as libpcap loses
# packets of the first frame while it sets the filter, and ends at its last. Without
# -p tcpdump would turn the PF promiscuous, which stalls the TX of its VFs.
CAPTURE_SCRIPT = """\
import json, socket, struct, subprocess, sys, time

pf, group, count, raw, out = sys.argv[1:6]


def run(*cmd):
    return subprocess.run(cmd, capture_output=True, text=True)


def records(f):
    while len(h := f.read(16)) == 16:
        yield h, f.read(struct.unpack("<4I", h)[2])


def rtp(frame):  # its address:port, RTP marker and sequence number
    udp = 14 + (frame[14] & 15) * 4
    port, _, _, _, pt, seq = struct.unpack(">HHHBBH", frame[udp + 2 : udp + 12])
    return f"{socket.inet_ntoa(frame[30:34])}:{port}", pt >> 7, seq


def whole_frames():
    markers = {}
    with open(raw, "rb") as f:
        f.read(24)
        for n, (_, frame) in enumerate(records(f)):
            key, marker, _ = rtp(frame)
            if marker:
                markers.setdefault(key, []).append(n)
    gaps, seqs, first, last = {}, {}, {}, {}
    with open(raw, "rb") as f, open(out, "wb") as o:
        head = f.read(24)
        o.write(head)
        # a nanosecond pcap, as tcpdump writes with --time-stamp-precision=nano
        tick = 1e9 if struct.unpack("<I", head[:4])[0] == 0xA1B23C4D else 1e6
        for n, (h, frame) in enumerate(records(f)):
            key, _, seq = rtp(frame)
            m = markers.get(key, [])
            if len(m) < 3 or not m[1] <= n <= m[-1]:
                continue
            if n > m[1]:  # the second marker's packet only starts the sequence
                o.write(h + frame)
                gaps[key] = gaps.get(key, 0) + (seq != (seqs[key] + 1) & 0xFFFF)
                sec, frac = struct.unpack("<II", h[:8])
                first.setdefault(key, sec + frac / tick)
                last[key] = sec + frac / tick
            seqs[key] = seq
    return gaps, {k: last[k] - first[k] for k in first}


drop = ["dev", pf, "ingress", "pref", "2110"]
run("tc", "qdisc", "add", "dev", pf, "clsact")
run("tc", "filter", "del", *drop)
try:
    run("tc", "filter", "add", *drop, "protocol", "ip", "flower", "skip_hw",
        "dst_ip", group, "action", "drop")
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, socket.inet_aton(group)
                 + bytes(4) + struct.pack("@i", socket.if_nametoindex(pf)))
    time.sleep(1)  # for the switch to send the group here
    cap = run("timeout", "30", "tcpdump", "-p", "-q", "-i", pf, "-j", "adapter_unsynced",
              "--time-stamp-precision=nano", "-s", "128", "-B", "1000000", "-c", count,
              "-w", raw, f"udp and dst host {group}")
    s.close()
finally:
    run("tc", "filter", "del", *drop)
    # an empty clsact goes, as a run that died left it too
    if not any(run("tc", "filter", "show", "dev", pf, d).stdout for d in ("ingress", "egress")):
        run("tc", "qdisc", "del", "dev", pf, "clsact")
gaps, spans = whole_frames() if cap.returncode == 0 else ({}, {})
print(json.dumps({
    "rc": cap.returncode,
    "error": cap.stderr.strip()[-200:] if cap.returncode else "",
    "gaps": gaps,
    "spans": spans,
}))
"""


def sh(host, script: str, check: bool = True) -> str:
    """Run *script* as root on *host* and return its output."""
    res = host.connection.execute_command(
        f"sudo -n bash -c {shlex.quote(script)}",
        shell=True,
        stderr_to_stdout=True,
        expected_return_codes=None,
    )
    if check and res.return_code != 0:
        raise RuntimeError(
            f"{host.name}: rc {res.return_code} from {script[:200]!r}: {res.stdout}"
        )
    return res.stdout or ""


def frame_packets(width: int, height: int) -> int:
    """The packets of a 4:2:2 10-bit frame, of 1320 bytes of pixels: 3928 at 1080p."""
    return math.ceil(width * height * 20 / 8 / 1320)


def list_verdicts(report: dict, link: str = "") -> dict[str, dict]:
    """Each video stream of an EBU LIST *report* by its address:port: LIST's ST 2110-21
    class, the checks it failed, its largest Cinst, its VRX range, and its page in
    LIST's UI under *link*, the page of its capture."""
    verdicts = {}
    for s in report.get("streams") or []:
        if not isinstance(s, dict) or s.get("media_type") != "video":
            continue
        net = s.get("network_information") or {}
        timing = s.get("global_video_analysis") or {}
        cinst = [b for b, _ in (timing.get("cinst") or {}).get("histogram") or []]
        vrx = [b for b, _ in (timing.get("vrx") or {}).get("histogram") or []]
        stream = f"{net.get('destination_address')}:{net.get('destination_port')}"
        verdicts[stream] = {
            "class": timing.get("compliance"),
            "failed": sorted(
                k
                for k, a in (s.get("analyses") or {}).items()
                if (a or {}).get("result") != "compliant"
            ),
            "cinst": max(cinst, default=None),
            "vrx": [min(vrx, default=None), max(vrx, default=None)],
            "link": f"{link}{s['id']}" if link and s.get("id") else link,
        }
    return verdicts


def compliant(verdict: dict) -> bool:
    """Whether LIST found a stream of list_verdicts() of type N and failed no check."""
    return verdict.get("class") in COMPLIANT and not verdict.get("failed")


def captured(cap: dict, streams: list[str]) -> str:
    """Why the capture *cap* of CAPTURE_SCRIPT does not hold *streams* whole, or "": a
    packet the kernel or the NIC dropped in a frame kept is a gap in its sequence."""
    if cap["rc"]:
        return f"tcpdump exited {cap['rc']}: {cap['error']}"
    if missing := [s for s in streams if s not in cap["gaps"]]:
        return f"no whole frame of {', '.join(missing)}"
    if gaps := sum(cap["gaps"][s] for s in streams):
        return f"{gaps} gaps in the RTP sequence"
    if short := [s for s in streams if cap.get("spans", {}).get(s, 0) < CAPTURE_S]:
        return f"{len(short)} streams kept under {CAPTURE_S} s of whole frames"
    return ""


def list_report(
    host, mtl_path: str, ebu: EbuServer, pcap: str
) -> tuple[dict | None, str, str]:
    """Upload *pcap* of *host* to EBU LIST *ebu*.  Returns LIST's report of it, or
    None and why, and the capture's id on LIST, or "".

    The capture stays on LIST, so that the report can link to it, as the captures of
    pcap_compliance.py do.  LIST's own cleanup deletes it after a few days.  The
    client logs in first, so that a login that fails uploads nothing.  The password
    goes over stdin, as mfd_connect logs every command line.  An empty report is
    analysed twice more, as pcap_compliance.py does.
    """
    try:
        client = PcapComplianceClient(ebu.ip, ebu.user, ebu.password)
    except Exception as e:
        why = f"the login to LIST failed: {(str(e) or repr(e)).splitlines()[0]}"
        return None, why, ""
    res = host.connection.execute_command(
        f"sudo -n timeout 300 python3 {mtl_path}/tests/acceptance/compliance/"
        f"upload_pcap.py --ip {ebu.ip} --user {ebu.user} --password-stdin "
        f"--pcap {pcap}",
        shell=True,
        stderr_to_stdout=True,
        expected_return_codes=None,
        input_data=f"{ebu.password}\n",
    )
    uuid = re.search(r">>>UUID: (\S+)", res.stdout or "")
    if not uuid:  # the last line of a traceback is its error
        out = (res.stdout or "").strip().splitlines() or [""]
        why = f"the upload failed: {out[-1][-200:] or f'rc {res.return_code}'}"
        return None, why, ""
    client.pcap_id = uuid[1]
    report = client.download_report()
    for _ in range(2):  # a dropped ingest
        if not report or report.get("streams"):
            break
        report = client.reanalyze(report)
    if not report:
        return None, "LIST did not analyse it in time", uuid[1]
    return report, "", uuid[1]


def tai_utc(host) -> int:
    """The TAI-UTC offset of *host*'s kernel, in s: CLOCK_TAI, which RxTxApp paces on,
    less CLOCK_REALTIME."""
    return int(
        sh(
            host,
            "python3 -c 'import time; "
            "print(round(time.clock_gettime(time.CLOCK_TAI) - time.time()))'",
        )
    )


class WireCapture:
    """The captures of a sender's multicast groups on *pf* of *host*, which sends
    nothing, run on *cpus*, away from the sender's, in *run_dir*."""

    def __init__(self, host, pf: str, run_dir: str, cpus: str | None, label: str):
        self.host, self.pf, self.run_dir, self.label = host, pf, run_dir, label
        self.pin = f"taskset -c {cpus} " if cpus else ""
        self.pcaps: dict[str, tuple[list[str], str]] = {}  # group → (streams, why)
        self.links: dict[str, str] = {}  # group → its capture's page in LIST's UI
        self.saved: dict[str, str] = {}  # group → its LIST report saved, if it failed
        self.problems: list[str] = []
        if not cpus:
            self.problems.append("the captures ran on the sender's CPUs")
        self.tai = None
        self.held_from = None  # the first phc2sys sample that must be locked

    def _samples(self) -> list[tuple[int, int]]:
        log = sh(self.host, f"cat {self.run_dir}/phc2sys.log", check=False)
        return [(int(o), int(state)) for o, state in RE_PHC.findall(log)]

    @staticmethod
    def _held(samples) -> list[int]:
        return [o for o, state in samples if state != 2 or abs(o) > PHC_LOCK_NS]

    def start(self, seconds: int) -> str:
        """Start phc2sys on the PF for at most *seconds*, and wait until it holds the
        PF's clock.  Returns why the PF cannot capture, or ""."""
        # A new run dir, so that stop() never reads a phc2sys.pid of an earlier run.
        script = base64.b64encode(CAPTURE_SCRIPT.encode()).decode()
        sh(
            self.host,
            f"rm -rf {self.run_dir} && mkdir -m 0700 {self.run_dir} && "
            f"echo {script} | base64 -d > {self.run_dir}/capture.py",
        )
        types = sh(self.host, f"tcpdump -J -i {self.pf}", check=False)
        if "adapter_unsynced" not in types:  # else tcpdump falls back to the host's
            return f"{self.pf} gives no hardware time stamps (tcpdump -J)"
        self.tai = tai_utc(self.host)
        sh(
            self.host,
            f"{{ {self.pin}timeout {seconds} phc2sys -s CLOCK_REALTIME -c {self.pf} "
            f"-O {self.tai} -S 0.001 -m > {self.run_dir}/phc2sys.log 2>&1 & "
            f"echo $! > {self.run_dir}/phc2sys.pid; }}",
        )
        deadline = time.monotonic() + PHC_WAIT_S
        while time.monotonic() < deadline:
            last = self._samples()[-3:]
            if len(last) == 3 and not self._held(last):
                self.held_from = len(self._samples())
                return ""
            time.sleep(1)
        return (
            f"phc2sys held {self.pf} within {PHC_LOCK_NS} ns of CLOCK_TAI in no "
            f"three samples in a row in {PHC_WAIT_S} s"
        )

    def begin(self) -> None:
        """Start the window in which phc2sys must hold the clock: the captures'."""
        self.held_from = len(self._samples())

    def flooding(self) -> str:
        """Why the switch sends the PF multicast that it did not join, or "".

        The port counts every multicast packet that reaches it (rx_multicast.nic),
        before the filter that drops those of a group it did not join.
        """
        out = sh(
            self.host,
            f"c() {{ ethtool -S {self.pf} | awk '/rx_multicast.nic:/ {{print $2}}'; }}; "
            "echo $(c); sleep 1; echo $(c)",
            check=False,
        )
        counts = [int(v) for v in out.split() if v.isdigit()]
        if len(counts) != 2:
            return (
                f"{self.pf} gives no rx_multicast.nic counter, so flooding is not known"
            )
        if counts[1] < counts[0]:
            return f"the rx_multicast.nic counter of {self.pf} went back, so flooding is not known"
        if counts[1] - counts[0] <= FLOOD_PPS:
            return ""
        return (
            f"the switch sent {self.pf} {counts[1] - counts[0]} multicast packets/s "
            "that it did not join"
        )

    def _pcap(self, address: str) -> str:
        """The capture of group *address*, named so that LIST lists it by its case."""
        return f"{self.run_dir}/{self.label}_{address}.pcap"

    def note(self, address: str, streams: list[str], why: str) -> None:
        """Leave group *address*'s *streams* not captured, for *why*."""
        self.pcaps[address] = (streams, why)

    def capture(self, address: str, streams: list[str], pps: float, fps: float) -> None:
        """Capture group *address* for CAPTURE_S, *pps* packets a second a stream at
        *fps* frames a second.  A capture takes 4 frames more, which cover the frames
        that the trim to whole frames drops."""
        pcap = self._pcap(address)
        count = math.ceil(len(streams) * pps * (CAPTURE_S + 4 / fps))
        before = len(self._samples())
        try:
            lines = sh(
                self.host,
                f"{self.pin}python3 -I {self.run_dir}/capture.py {self.pf} {address} "
                f'{count} {pcap}.raw {pcap}; echo "exit $?"; rm -f {pcap}.raw',
                check=False,
            ).splitlines()
            code = lines[-1] if lines else ""
            last = lines[-2] if len(lines) > 1 else ""
            if code != "exit 0":  # a traceback's last line is its error
                why = f"capture.py {code or 'gave no exit code'}: {last[-200:]}"
            else:
                why = captured(json.loads(last), streams) or self._clock(before)
        except Exception as e:
            why = f"capture failed: {(str(e) or repr(e)).splitlines()[0]}"
        self.note(address, streams, why)

    def _clock(self, before: int) -> str:
        """Why phc2sys did not hold the clock in its samples after *before*, those of
        a capture, or "".  It logs a sample a second, so a capture has at least one."""
        new = self._samples()[before:]
        if not new:
            return "phc2sys logged no sample during the capture"
        if held := self._held(new):
            return (
                f"phc2sys let the clock drift by up to {max(map(abs, held))} ns "
                "during the capture"
            )
        return ""

    def _save(self, report: dict, keep: str, address: str) -> str:
        """Save LIST's *report* of group *address* in directory *keep*; its path, or ""
        if it could not be written."""
        path = os.path.join(keep, f"{self.label}_{address}.json")
        try:
            os.makedirs(keep, exist_ok=True)
            with open(path, "w") as f:
                json.dump(report, f, indent=1)
        except OSError as e:
            logger.warning(f"LIST report of {address} not saved in {keep}: {e}")
            return ""
        return path

    def stop(self) -> None:
        """Stop phc2sys, and note whether it held the clock through the captures."""
        window = self._samples()[self.held_from :]
        held = self._held(window)
        sh(self.host, f"kill $(cat {self.run_dir}/phc2sys.pid)", check=False)
        if self.held_from is None:
            return
        if not window:
            self.problems.append("phc2sys logged no sample during the captures")
        if held:
            self.problems.append(
                f"phc2sys let the clock of {self.pf} drift from CLOCK_TAI by up "
                f"to {max(map(abs, held))} ns during the captures, over "
                f"{PHC_LOCK_NS} ns"
            )
        try:
            if tai_utc(self.host) != self.tai:  # a sender reads it at its start only
                self.problems.append(f"the TAI-UTC offset of {self.host.name} changed")
        except Exception as e:
            self.problems.append(f"the TAI-UTC offset not read again: {e}")

    def judge(self, mtl_path: str, ebu: EbuServer, keep: str = "") -> dict[str, dict]:
        """LIST's verdict on each stream captured, or {"why": ...} for one not judged.

        The LIST report of a group with a stream that is not narrow is saved as JSON
        in directory *keep*, if given, for the artifacts.  The captures are deleted
        after.
        """
        verdicts, down = {}, ""
        try:
            for address, (streams, why) in self.pcaps.items():
                got = {}
                if not why and down:
                    why = f"not uploaded, as an earlier group failed: {down}"
                elif not why:
                    try:
                        pcap = self._pcap(address)
                        report, why, pcap_id = list_report(
                            self.host, mtl_path, ebu, pcap
                        )
                        if why.startswith(("the upload failed", "the login to LIST")):
                            down = why
                        link = ""
                        if pcap_id:
                            link = f"http://{ebu.ip}/pcaps/{pcap_id}/streams/"
                            self.links[address] = link
                        got = list_verdicts(report, link) if report else {}
                        if keep and not all(map(compliant, got.values())):
                            self.saved[address] = self._save(report, keep, address)
                    except Exception as e:
                        why = f"LIST failed: {(str(e) or repr(e)).splitlines()[0]}"
                why = why or "LIST found no video stream there"
                verdicts.update({s: got.get(s) or {"why": why} for s in streams})
        finally:
            sh(self.host, f"rm -rf {self.run_dir}", check=False)
        return verdicts


def summary(verdicts: dict[str, dict], problems: list[str]) -> str:
    """One line on *verdicts* of WireCapture.judge(): the streams of type N, those that
    failed a check, those not judged and why, and the range of VRX and the largest
    Cinst that LIST measured."""
    judged = {s: v for s, v in verdicts.items() if "why" not in v}
    narrow = sum(compliant(v) for v in judged.values())
    failed = {}
    for v in judged.values():
        for check in v["failed"] or ([] if compliant(v) else [f"class {v['class']}"]):
            failed[check] = failed.get(check, 0) + 1
    vrx = [v["vrx"] for v in judged.values() if None not in v["vrx"]]
    cinst = [v["cinst"] for v in judged.values() if v["cinst"] is not None]
    why = sorted({v["why"] for v in verdicts.values() if "why" in v})
    text = f"{narrow}/{len(verdicts)} streams narrow"
    if judged:
        text += f" in captures of {CAPTURE_S} s"
    if failed:
        text += ", failed " + ", ".join(f"{k} x{n}" for k, n in sorted(failed.items()))
    if vrx:
        text += f", VRX {min(lo for lo, _ in vrx)} to {max(hi for _, hi in vrx)}"
    if cinst:
        text += f", Cinst up to {max(cinst)}"
    if len(judged) < len(verdicts):
        text += f", {len(verdicts) - len(judged)} not judged: {'; '.join(why)}"
    if problems:
        text += f"; {'; '.join(problems)}"
    return text
