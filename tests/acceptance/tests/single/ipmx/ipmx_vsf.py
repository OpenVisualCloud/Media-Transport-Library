# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation
"""Run the VSF IPMX validation scripts on a capture and parse their reports.

The scripts (TP-1 sections 13.6.1 and 13.6.2) are not in this repository: they
run from an external clone named by ``test_config.ipmx.vsf_path``. The pcap
validators exit 0 even when a check fails, so the verdict comes from the report
text: ``PASS``, ``FAIL`` and ``CANNOT_TEST`` lines under the ``SHALL
requirements`` and ``SHOULD requirements`` headers.
"""

import logging
import math
import re
import warnings
from dataclasses import dataclass, field
from fractions import Fraction

import pytest

logger = logging.getLogger(__name__)

STREAMS_TOOLS = "ipmx/streams/ipmx-streams-tools"
_RESULT_RE = re.compile(r"^\s*(PASS|FAIL|CANNOT_TEST)\s+([\w.\-]+):\s*(.*)$")
_LEVEL_RE = re.compile(r"^\s*(SHALL|SHOULD) requirements\s*$")
_BLOCK_VERSION_RE = re.compile(
    r"Initial block version \(first Sender Report in PCAP\): (\d+)"
)

# Requirements the validators can never test from a capture of these cases:
# options the cases do not pass (--ssrc, --payload-type, --rtcp-port), the NMOS and receiver
# checks of TR-10-15a, and the TR-10-9 11.2c/d sender-type rules. Any other
# CANNOT_TEST means the capture misses what a check needs, so it fails.
UNTESTABLE = {
    "raw": {"RAW-CLI-PT", "RAW-CLI-RTCP-PORT", "RAW-CLI-SSRC"},
    "jxsv": {
        "JXSV-CLI-PT", "JXSV-CLI-RTCP-PORT", "JXSV-CLI-SSRC",
        "TR-10-15a-NMOS-RX", "TR-10-15a-NMOS-TX", "TR-10-15a-NMOS-TX-CAP",
        "TR-10-15a-PROFILE-CFG", "TR-10-15a-PROFILE-RX", "TR-10-15a-PROFILE-RX-CFG",
        "TR-10-9-11.2c", "TR-10-9-11.2d",
    },
}  # fmt: skip


@dataclass
class VsfReport:
    """Parsed report of one validator run."""

    text: str
    shall_fail: list = field(default_factory=list)
    should_fail: list = field(default_factory=list)
    cannot_test: list = field(default_factory=list)
    passed: int = 0
    block_version: int = None


def parse_report(text: str) -> VsfReport:
    """Split a validator report into failures by level, CANNOT_TEST ids and passes."""
    report = VsfReport(text=text)
    level = None
    for line in text.splitlines():
        m = _LEVEL_RE.match(line)
        if m:
            level = m.group(1)
            continue
        m = _BLOCK_VERSION_RE.search(line)
        if m:
            report.block_version = int(m.group(1))
            continue
        m = _RESULT_RE.match(line)
        if not m:
            continue
        verdict, req_id, desc = m.groups()
        if verdict == "PASS":
            report.passed += 1
        elif verdict == "CANNOT_TEST":
            report.cannot_test.append(req_id)
        elif level == "SHOULD":
            report.should_fail.append(f"{req_id}: {desc}")
        else:
            report.shall_fail.append(f"{req_id}: {desc}")
    return report


def validated_frames(tools: dict, rate: str) -> int:
    """Frames the validators read: at least 2 s of first packets and SRs.

    TR-10-9 11.2a/b measure the frame and Sender Report intervals over a 2
    second period; the validators report them CANNOT_TEST on a shorter span.
    """
    return max(tools["max_frames"], math.ceil(2 * Fraction(rate)) + 3)


def pf_netdev(host, vf_bdf: str):
    """Kernel netdev of the PF that owns a VF, or None if the port is no VF."""
    out = host.connection.execute_command(
        f"ls /sys/bus/pci/devices/{vf_bdf}/physfn/net/",
        shell=True,
        expected_return_codes=None,
    ).stdout.split()
    return out[0] if out else None


def run(host, cmd: str):
    """Run one validator command on the host; returns (exit code, output)."""
    res = host.connection.execute_command(
        f"LC_ALL=C {cmd}", shell=True, expected_return_codes=None, timeout=10800
    )
    return res.return_code, (res.stdout or "") + (res.stderr or "")


def pcap_validate(
    tools: dict, host, validator: str, pcap: str, cfg: str, sdp: str, dst_ip: str,
    port: int, frames: int,
) -> VsfReport:  # fmt: skip
    """``ipmx_<validator>_validate_pcap.py --cmax --cfg --sdp`` (TP-1 13.6.1).

    ``validator`` is ``raw`` for ST 2110-20 and ``jxsv`` for ST 2110-22 JPEG XS;
    ``dst_ip`` and ``port`` pick the RTP stream, the leg of an ST 2022-7 one.
    """
    script = f"{tools['vsf_path']}/{STREAMS_TOOLS}/ipmx_{validator}_validate_pcap.py"
    _, out = run(
        host,
        f"taskset -c {tools['cpus']} {tools['python']} {script} --cmax --full-report "
        f"--max-frames {frames} --dst-ip {dst_ip} --port {port} --cfg {cfg} "
        f"--sdp {sdp} {pcap}",
    )
    return parse_report(out)


def write_cfg(
    host, path: str, width: int, height: int, rate: str, sampling: str, depth: int,
    ptp: bool = False,
):  # fmt: skip
    """Stream profile in the key=value form of the VSF ``cfg/Test*.cfg`` files."""
    text = (
        f"type=video\nrtpclock=90000\nPTP={str(ptp).lower()}\n"
        f"width={width}\nheight={height}\n"
        f"exactframerate={rate}\nsampling={sampling}\ndepth={depth}\n"
    )
    host.connection.execute_command(f"printf '{text}' > {path}", shell=True)


def sdp_validate(tools: dict, host, cfg: str, sdp: str):
    """``sdp_validation_script.py`` (TP-1 13.6.2): no FAIL, warning or error.

    The script exits 1 when it cannot read the SDP, and prints ``ERROR`` when
    a check throws; only a run that reaches its end message validated it.
    """
    script = f"{tools['vsf_path']}/ipmx/sdp_validation_script.py"
    rc, out = run(
        host,
        f"taskset -c {tools['cpus']} {tools['python']} {script} --cfg {cfg} --sdp {sdp}",
    )
    logger.info(out)
    assert rc == 0, out
    assert "All configuration validations passed!" in out, out
    assert "Validation complete!" in out, out
    for word in ("FAIL", "Warning", "ERROR", "Error"):
        assert word not in out, out


def run_sender(
    app, host, build, test_time, tools, capture, session_type, validator,
    frame_bytes, width, height, rate, sampling, depth, host_limited=(),
    tag=None, block_version=None, ptp=False,
):  # fmt: skip
    """Run one IPMX RxTxApp TX-to-RX session and check it as TP-1 13.6 does.

    ``app`` holds one ``session_type`` TX and RX session from ``create_command``;
    this sets the RxTxApp IPMX keys (``ipmx``, and ``ipmx_block_version`` when
    given) and the SDP directory, captures every leg on its RX port from before
    the start, then asserts the RX Sender Report lines, the SDP script and
    every SHALL and SHOULD of the pcap validator on each leg. A requirement in
    ``host_limited`` only warns when it fails: the host cannot meet it.
    ``tag`` names the files of the run, so a case can run more than once.
    ``ptp`` (the case created the app with ``enable_ptp``) skips the case when
    the Sender Reports carry no PTP reference clock (no grandmaster).
    Returns the RX Sender Report lines, the SDP text and the pcap reports.
    """
    tag = tag or session_type
    tx = app.config["tx_sessions"][0]
    rx = app.config["rx_sessions"][0]
    tx[session_type][0]["ipmx"] = True
    rx[session_type][0]["ipmx"] = True
    if block_version is not None:
        tx[session_type][0]["ipmx_block_version"] = block_version
    sdp_dir = f"{capture.workdir}/{tag}_sdp"
    host.connection.execute_command(f"mkdir -p {sdp_dir}", shell=True)
    app.command += f" --ipmx_sdp_dir {sdp_dir}"
    # Leg i leaves on TX port i to dip[i] and arrives on RX port i.
    legs = [
        (f"{tag}_{i}", app.config["interfaces"][port]["name"], tx["dip"][i])
        for i, port in enumerate(rx["interface"])
    ]

    # The capture listens on the RX VF's PF netdev, which only sees packets
    # that crossed the cable: a leg between two VFs of one PF stays inside
    # the NIC switch.
    tx_ports = [app.config["interfaces"][port]["name"] for port in tx["interface"]]
    for (leg, rx_port, _), tx_port in zip(legs, tx_ports):
        rx_pf, tx_pf = pf_netdev(host, rx_port), pf_netdev(host, tx_port)
        if not rx_pf or rx_pf == tx_pf:
            pytest.skip(f"{leg}: needs TX and RX VFs of two cabled PFs")

    frames = validated_frames(tools, rate)
    port = tx[session_type][0]["start_port"]
    # Twice the packets of the validated frames (about 1200 bytes of payload
    # per packet) bound the capture file of a long high-rate stream.
    max_packets = 2 * (frames + 3) * (frame_bytes // 1200 + 1)
    # TP-1 13.1: the capture starts before the stream, so it holds the first SR.
    for leg, rx_port, dst_ip in legs:
        capture.start(rx_port, dst_ip, leg, max_packets=max_packets)
    try:
        app.execute_test(build=build, test_time=test_time, host=host)
    finally:
        capture.stop(frames=frames, port=port)

    # The RX session parsed the Sender Reports of every port and found them valid.
    sr_lines = [
        line
        for line in (app.last_output or "").splitlines()
        if f"app_ipmx_rx_{session_type}_result" in line and "ipmx sr port" in line
    ]
    assert len(sr_lines) == len(legs), sr_lines
    assert all(line.rstrip().endswith("OK") for line in sr_lines), sr_lines
    if ptp and not all("ts_refclk ptp=" in line for line in sr_lines):
        pytest.skip(f"no PTP grandmaster: {sr_lines}")

    sdp = f"{sdp_dir}/app_tx_{session_type}_0.sdp"
    cfg = f"{capture.workdir}/{tag}.cfg"
    write_cfg(host, cfg, width, height, rate, sampling, depth, ptp)
    # TP-1 13.2 / 13.6.2: the SDP the sender publishes.
    sdp_validate(tools, host, cfg, sdp)

    # TP-1 13.1, 13.3, 13.6.1, and TR-10-1 8.1 CMAX and TR-10-9 11.2 of 13.5:
    # every SHALL and SHOULD of the pcap validator, per leg, as the VSF run
    # guide asks. Without a grandmaster the free-running sender clock cannot
    # meet the NTP rate SHOULD.
    tolerated = set(host_limited) | (set() if ptp else {"TR-10-1-NTP-RATE"})
    reports = []
    for i, (leg, _, dst_ip) in enumerate(legs):
        leg_sdp = sdp
        if len(legs) > 1:
            # The tools read only the first media section of an SDP, so each
            # ST 2022-7 leg gets the SDP a receiver of that leg alone sees.
            leg_sdp = f"{sdp_dir}/{leg}.sdp"
            host.connection.execute_command(
                f"awk -v leg={i} 'BEGIN {{ n = -1 }} /^m=/ {{ n++ }} "
                f"/^a=(group:DUP|mid:)/ {{ next }} n == -1 || n == leg' "
                f"{sdp} > {leg_sdp}",
                shell=True,
            )
            sdp_validate(tools, host, cfg, leg_sdp)
        # TP-1 13.1: the capture holds the stream from its first packet.
        first_seq = host.connection.execute_command(
            f"tshark -r {capture.pcaps[leg]} -d udp.port=={port},rtp "
            f"-Y 'udp.dstport=={port}' -T fields -e rtp.seq 2> /dev/null | head -n 1",
            shell=True,
        ).stdout.strip()
        assert first_seq == "0", f"{leg}: capture starts at RTP seq {first_seq}"
        report = pcap_validate(
            tools, host, validator, capture.pcaps[leg], cfg, leg_sdp, dst_ip, port,
            frames,
        )  # fmt: skip
        logger.info(report.text)
        assert report.passed > 0, f"{leg}: validator reported no requirement"
        # TP-1 13.1: the capture holds a Sender Report with an Info Block.
        assert report.block_version is not None, f"{leg}: no IPMX Info Block"
        untested = set(report.cannot_test) - UNTESTABLE[validator]
        assert not untested, f"{leg}: CANNOT_TEST {sorted(untested)}"
        failed = []
        for item in report.shall_fail + report.should_fail:
            if item.split(":")[0] in tolerated:
                warnings.warn(f"{leg}: failed, known host limitation: {item}")
            else:
                failed.append(item)
        assert not failed, f"{leg}: {failed}"
        reports.append(report)

    sdp_text = host.connection.execute_command(f"cat {sdp}", shell=True).stdout
    return sr_lines, sdp_text, reports


def create_st20p(app_factory, interfaces, media, width, height, fps, file_fmt,
                 transport_fmt, test_time, **extra):  # fmt: skip
    """RxTxApp with one st20p multicast TX and RX session, for ``run_sender``."""
    app = app_factory("rxtxapp")
    app.create_command(
        session_type="st20p",
        nic_port_list=interfaces,
        test_mode="multicast",
        width=width,
        height=height,
        framerate=fps,
        pixel_format=file_fmt,
        transport_format=transport_fmt,
        input_file=media,
        test_time=test_time,
        **extra,
    )
    return app
