# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation
"""Fixtures of the IPMX acceptance cases (VSF TR-10, TP-1 section 13).

* ``ipmx_tools``: the external VSF script clone (``test_config.ipmx``); the
  cases skip when it is not configured or the RxTxApp is built without IPMX.
* ``ipmx_capture``: a wire capture started before RxTxApp, so the pcap holds
  the first RTCP Sender Report (TP-1 13.1). The root ``pcap_capture`` fixture
  is not used: it needs an EBU LIST server and arms after the stream started.
"""

import logging
import os
import re
import time

import pytest
from tests.single.ipmx import ipmx_vsf

logger = logging.getLogger(__name__)


@pytest.fixture(scope="session")
def ipmx_tools(test_config, mtl_path, hosts):
    """VSF clone, its interpreter, the validator CPUs and the JPEG XS settings."""
    cfg = test_config.get("ipmx") or {}
    vsf_path = cfg.get("vsf_path")
    if not vsf_path:
        pytest.skip("test_config.ipmx.vsf_path not set (VSF IPMX test scripts clone)")
    host = list(hosts.values())[0]
    python = cfg.get("vsf_python", "python3")
    pc = host.connection.execute_command(
        f"grep -rh '^ipmx=' {mtl_path}/.local_install/mtl/lib*/ --include=mtl.pc",
        shell=True,
        expected_return_codes=None,
    ).stdout
    if "ipmx=true" not in (pc or ""):
        pytest.skip(f"{mtl_path}/.local_install/mtl is built without IPMX (mtl.pc)")
    for tool in (python, "tcpdump", "reordercap", "editcap", "tshark"):
        if host.connection.execute_command(
            f"command -v {tool}", shell=True, expected_return_codes=None
        ).return_code:
            pytest.skip(f"{tool} not found on the host")
    commit = host.connection.execute_command(
        f"git -C {vsf_path} rev-parse --short HEAD",
        shell=True,
        expected_return_codes=None,
    ).stdout.strip()
    logger.info(f"VSF IPMX scripts: {vsf_path} at {commit or 'unknown commit'}")
    return {
        "vsf_path": vsf_path,
        "python": python,
        "cpus": cfg.get("vsf_cpus", "0-1023"),
        # A floor: every case reads at least 2 s of frames.
        "max_frames": int(cfg.get("max_frames", 0)),
        "svt_plugin_dir": cfg.get("svt_plugin_dir"),
        "lcores": cfg.get("lcores"),
    }


# Holds an IPv4 group membership on a netdev until killed: argv group netdev tag.
_JOIN_PY = (
    "import socket, struct, sys, time; "
    "s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); "
    "s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, struct.pack('4s4si', "
    "socket.inet_aton(sys.argv[1]), bytes(4), socket.if_nametoindex(sys.argv[2]))); "
    "time.sleep(86400)"
)


class IpmxCapture:
    """tcpdump with hardware timestamps on RX PF netdevs, around one run.

    One capture per leg: the primary stream, and for ST 2022-7 the redundant one.
    """

    def __init__(self, host, workdir: str):
        self.host = host
        self.workdir = workdir
        self.raws = {}
        self.pcaps = {}
        # The process objects must live: dropping one stops its tcpdump.
        self.procs = []

    def start(self, rx_vf: str, dst_ip: str, tag: str, max_packets: int = 0):
        """Start capturing before RxTxApp, so the first SR is recorded.

        The capture netdev joins the group first: a switch with IGMP snooping
        between the ports forwards the group only after a join, and the RX
        session joins when RxTxApp starts, the moment the TX starts sending.
        ``max_packets`` > 0 ends the capture after that many packets, which
        bounds the file of a long high-rate stream.
        """
        netdev = ipmx_vsf.pf_netdev(self.host, rx_vf)
        raw = f"{self.workdir}/{tag}.raw.pcap"
        self.procs.append(
            self.host.connection.start_process(
                f'exec python3 -c "{_JOIN_PY}" {dst_ip} {netdev} {raw}', shell=True
            )
        )
        time.sleep(3)
        proc = self.host.connection.start_process(
            f"tcpdump -i {netdev} -j adapter_unsynced --time-stamp-precision=nano "
            f"-B 1048576 -s 320 {f'-c {max_packets} ' if max_packets else ''}"
            f"-w {raw} udp and dst host {dst_ip}",
            shell=True,
        )
        self.procs.append(proc)
        self.raws[tag] = raw
        time.sleep(2)

    def stop(self, frames: int = 0, port: int = 0):
        """Stop the captures and sort them: kernel RSS writes out of timestamp order.

        ``frames`` > 0 keeps the packets up to the last one (marker bit) of
        that frame of the RTP stream on ``port``: the stream start with its
        first Sender Report (TP-1 13.1) and whole frames only. The validators
        parse the whole file whatever their frame limit, and when they stop
        at the limit they count its last frame twice (VSF clone 86668b5).
        """
        if not self.raws:
            return
        for raw in self.raws.values():
            # "[t]" and "[i]" keep the patterns from matching this command's
            # own shell.
            self.host.connection.execute_command(
                f"pkill -INT -f '[t]cpdump .*-w {raw} '; "
                f"pkill -f '[i]mport socket.* {raw}$'",
                shell=True,
                expected_return_codes=None,
            )
        time.sleep(2)
        for tag, raw in self.raws.items():
            pcap = f"{self.workdir}/{tag}.pcap"
            sorted_pcap = f"{raw}.sorted"
            cmd = f"reordercap {raw} {sorted_pcap} && rm -f {raw} && "
            if frames:
                # sed quits at the frame, which stops tshark early.
                cmd += (
                    f"last=$(tshark -r {sorted_pcap} -d udp.port=={port},rtp "
                    f"-Y 'udp.dstport=={port} && rtp.marker==1' -T fields "
                    f"-e frame.number 2> /dev/null | sed -n '{frames}{{p;q}}') && "
                    f'[ -n "$last" ] && editcap -r {sorted_pcap} {pcap} 1-$last && '
                    f"rm -f {sorted_pcap}"
                )
            else:
                cmd += f"mv {sorted_pcap} {pcap}"
            self.host.connection.execute_command(cmd, shell=True)
            self.pcaps[tag] = pcap
        self.raws = {}
        self.procs = []


@pytest.fixture
def ipmx_capture(hosts, request):
    """Per-case capture directory and recorder; files stay for failure analysis."""
    host = list(hosts.values())[0]
    # The node name holds the fixture values (paths) of the root conftest.
    name = re.sub(r"\|[^|]*\|", "", request.node.name)
    workdir = (
        f"/tmp/ipmx_acceptance/{re.sub(r'[^A-Za-z0-9_.-]+', '_', name).strip('_-')}"
    )
    host.connection.execute_command(f"mkdir -p {workdir}", shell=True)
    cap = IpmxCapture(host, workdir)
    yield cap
    cap.stop()
    logger.info(f"IPMX capture and SDP kept in {workdir}")


@pytest.fixture
def ipmx_media(hosts):
    """Random-content raw frames: the IPMX checks need valid packets, not a picture.

    ``bits`` above 8 makes little-endian 16-bit samples of that depth (a
    ``*PLANAR10LE`` frame), so every sample is in the range the codec takes.
    """
    host = list(hosts.values())[0]

    def make(name: str, frame_bytes: int, frames: int = 4, bits: int = 8) -> str:
        path = f"/tmp/ipmx_acceptance/media/{name}"
        clip = ""
        if bits > 8:
            mask = (1 << (bits - 8)) - 1
            clip = (
                " | python3 -c 'import sys; d = bytearray(sys.stdin.buffer.read()); "
                f"d[1::2] = d[1::2].translate(bytes(i & {mask} for i in range(256))); "
                "sys.stdout.buffer.write(d)'"
            )
        host.connection.execute_command(
            f"mkdir -p {os.path.dirname(path)} && "
            f"head -c {frame_bytes * frames} /dev/urandom{clip} > {path}",
            shell=True,
        )
        return path

    return make
