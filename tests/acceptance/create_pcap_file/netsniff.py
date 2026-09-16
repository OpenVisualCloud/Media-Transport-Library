# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2025 Intel Corporation
import datetime
import ipaddress
import logging
import math
import os
import re
from time import sleep

from mfd_connect.exceptions import (
    ConnectionCalledProcessError,
    RemoteProcessInvalidState,
    RemoteProcessTimeoutExpired,
    SSHRemoteProcessEndException,
)

logger = logging.getLogger(__name__)
STARTUP_WAIT = 2  # Default wait time after starting the process
_REAP_GRACE_SEC = 0.3  # Grace period between SIGTERM and SIGKILL for the capture

# pgroup size (octets) and coverage (pixels) per sampling system and bit depth,
# from SMPTE ST 2110-20:2022 tables 1 (4:4:4), 2 (4:2:2) and 3 (4:2:0). Keyed by
# the MTL transport-format name used in mtl_engine.media_files ("format").
# RGB/XYZ/ICtCp share the 4:4:4 geometry, so they map onto the same rows.
_PGROUP_444 = {8: (3, 1), 10: (15, 4), 12: (9, 2), 16: (6, 1)}
_PGROUPS = {
    "YUV_422": {8: (4, 2), 10: (5, 2), 12: (6, 2), 16: (8, 2)},
    "YUV_420": {8: (6, 4), 10: (15, 8), 12: (9, 4)},
    "YUV_444": _PGROUP_444,
    "RGB": _PGROUP_444,
}
_DEFAULT_PGROUP = (5, 2)  # 4:2:2 10-bit
_BPM_PAYLOAD_SIZE = 1260  # ST_VIDEO_BPM_SIZE in lib/src/st2110/st_header.h


def _pgroup_for(transport_format: str | None) -> tuple[int, int]:
    """Return ``(pgroup_size, pgroup_coverage)`` for an MTL transport format.

    Falls back to the 4:2:2 10-bit pgroup for unknown/absent formats so callers
    keep the historical behaviour rather than crashing.
    """
    if not transport_format:
        return _DEFAULT_PGROUP
    sampling, _, depth_field = transport_format.rpartition("_")
    try:
        depth = int(depth_field.removesuffix("bit"))
    except ValueError:
        return _DEFAULT_PGROUP
    return _PGROUPS.get(sampling, {}).get(depth, _DEFAULT_PGROUP)


def calculate_packets_per_frame(media_file_info, mtu: int = 1500) -> int:
    # Simplified calculation for the number of packets per frame: how many
    # whole pgroups fit in one packet payload, and therefore how many packets
    # a full frame of pixels needs. Audio formats assume 1 packet per 1ms.
    packets = 1000
    if not media_file_info:
        raise ValueError("Missing media file info; cannot calculate packets per frame.")
    if "width" in media_file_info and "height" in media_file_info:
        # A wrong pgroup here under-counts packets for the denser samplings
        # (4:4:4 10-bit needs ~3x the packets of 4:2:2 10-bit), which truncates
        # the capture to a fraction of a frame and makes EBU LIST report the
        # stream as "unknown media_type" instead of analysing it.
        pgroupsize, pgroupcoverage = _pgroup_for(media_file_info.get("format"))
        headersize = 74  # Ethernet + IP + UDP + RTP headers
        packets = 1 + int(
            (media_file_info["width"] * media_file_info["height"])
            / (int((mtu - headersize) / pgroupsize) * pgroupcoverage)
        )
        # MTL's default packing is BPM, which uses a fixed 1260-octet payload
        # (ST_VIDEO_BPM_SIZE) rather than filling the MTU, so it needs more
        # packets per frame than the estimate above. Under-capturing truncates
        # the frame and makes EBU LIST misjudge (or fail to classify) the
        # stream, so size the capture for the larger of the two.
        frame_size = (
            media_file_info["width"]
            * media_file_info["height"]
            // pgroupcoverage
            * pgroupsize
        )
        packets = max(packets, math.ceil(frame_size / _BPM_PAYLOAD_SIZE))
    return packets


_SNAPLEN_FULL = 65535  # pcap's "the whole packet, whatever its length"

# What EBU LIST needs of an ST 2110-20 packet: Ethernet (14) + IPv4 (20) + UDP
# (8) + RTP (12) + the -20 payload header (a 2-octet extended sequence number
# and 6 octets per sample row datagram) -- 74 octets for the 3-row maximum. It
# never decodes pixels for a compliance report, so copying the rest out of the
# NIC ring buys nothing and costs the capture its headroom: 2160p119.88 offers
# 1.97 Mpps on one RSS queue, where a full 1442-octet copy per packet makes the
# receive softirq fall behind and the pcap comes back with sequence gaps.
_ST2110_20_SNAPLEN = 128


def _bpf_program(dst_ips, snaplen: int) -> str:
    """Return a netsniff-ng filter file accepting *snaplen* octets of UDP to *dst_ips*.

    The value a BPF filter returns is the snaplen the kernel applies:
    ``tpacket_rcv()`` clamps ``tp_snaplen`` to it and leaves ``tp_len`` at the
    true wire length, so the pcap still records the original length beside a
    truncated copy. netsniff-ng has no snaplen option of its own and libpcap
    compiles every filter expression with a 65535 return, so handing over a
    compiled program is the only way to ask for one -- which is why this exists
    instead of the ``dst <ip>`` expression it replaces.

    ``netsniff-ng -f`` reads this format -- one ``{ code, jt, jf, k },`` record
    per line, parsed by ``bpf_parse_rules()`` -- and it is byte-identical to what
    ``bpfc -f netsniff-ng`` emits for a single address.

    Also narrower than ``dst <ip>``: this requires IPv4/UDP, so ARP or ICMP to a
    stream's address stays out of the pcap.

    *dst_ips* must not be empty: with nothing to compare, the program would
    accept every IPv4/UDP packet on the interface.
    """
    if not dst_ips:
        raise ValueError("a capture filter needs at least one destination IP")
    count = len(dst_ips)
    program = [
        (0x28, 0, 0, 12),  # ldh [12]           ethertype
        (0x15, 0, count + 4, 0x0800),  # jne #ETH_P_IP      -> drop
        (0x30, 0, 0, 23),  # ldb [23]           IPv4 protocol
        (0x15, 0, count + 2, 17),  # jne #IPPROTO_UDP   -> drop
        (0x20, 0, 0, 30),  # ld  [30]           IPv4 destination
    ]
    # Every address jumps forward to the accept; only the last needs a false
    # branch, since a miss on any earlier one falls through to the next compare.
    for index, dst_ip in enumerate(dst_ips):
        program.append(
            (
                0x15,
                count - 1 - index,
                1 if index == count - 1 else 0,
                int(ipaddress.IPv4Address(dst_ip)),
            )
        )
    program.append((0x06, 0, 0, snaplen))  # ret #snaplen       accept
    program.append((0x06, 0, 0, 0))  # ret #0             drop
    return "".join("{ 0x%x, %u, %u, 0x%08x },\n" % insn for insn in program)


class NetsniffRecorder:
    """
    Class to handle the recording of network traffic using netsniff-ng.

    Captures with hardware (on-wire) RX timestamps in nanosecond pcap format
    for accurate ST 2110-21 timing (see the ``-T`` flag in ``start``).

    Attributes:
        host: Host object containing connection and network interfaces.
        test_name (str): Name for the capture session (used for file naming).
        pcap_dir (str): Directory to store the pcap files.
        interface: Network interface to capture traffic on.
        interface_index (int): Index of the network interface if not specified by interface.
        silent (bool): Whether to run netsniff-ng in silent mode (no stdout) (default: True).
    """

    def __init__(
        self,
        host,
        test_name: str,
        pcap_dir: str,
        interface=None,
        interface_index: int = 0,
        silent: bool = True,
        packets_capture: int | None = None,
        capture_time: int = 0,
    ):
        self.host = host
        self.test_name = test_name
        self.pcap_dir = pcap_dir
        self.pcap_file = None
        if interface is not None:
            self.interface = interface
        else:
            self.interface = self.host.network_interfaces[interface_index].name
        self.netsniff_process = None
        self.silent = silent
        # netsniff-ng filter file text, "" until update_filter() names a stream.
        self._filter_program = ""
        self.packets_capture = packets_capture
        self.capture_time = capture_time
        self._promisc_was_off = False

    @staticmethod
    def _sanitize_filename_component(value: str, *, max_len: int = 64) -> str:
        cleaned = re.sub(r"[^A-Za-z0-9._-]+", "-", (value or "").strip())
        cleaned = cleaned.strip("-._")
        if not cleaned:
            cleaned = "unknown"
        return cleaned[:max_len]

    def _get_remote_hostname(self) -> str:
        try:
            res = self.host.connection.execute_command("uname -n")
            hostname = (res.stdout or "").strip().splitlines()[0]
            if hostname:
                return hostname
        except Exception:
            pass
        return str(getattr(self.host, "name", "unknown"))

    def _build_pcap_path(self) -> str:
        hostname = self._sanitize_filename_component(self._get_remote_hostname())
        timestamp = datetime.datetime.now(datetime.timezone.utc).strftime(
            "%Y%m%dT%H%M%SZ"
        )
        job = (
            os.environ.get("MTL_GITHUB_WORKFLOW") or os.environ.get("GITHUB_JOB") or ""
        )
        job = self._sanitize_filename_component(job, max_len=96) if job else ""

        test = self._sanitize_filename_component(self.test_name, max_len=128)
        parts = [test, hostname, timestamp]
        if job:
            parts.append(job)
        filename = "__".join(parts) + ".pcap"
        return os.path.join(self.pcap_dir, filename)

    def start(self):
        """
        Starts the netsniff-ng
        """
        if not self.netsniff_process or not self.netsniff_process.running:
            connection = self.host.connection
            self._enable_promisc(connection)
            try:
                self.pcap_file = self._build_pcap_path()
                # netsniff-ng needs privilege for two separate things: it raises
                # /proc/sys/net/core/{r,w}mem_max, and it opens a PF_PACKET
                # socket. Unprivileged it reports "Permission denied" for the
                # former and "Creation of PF socket failed" for the latter, and
                # then exits 0 -- so a capture over an SSH session running as an
                # ordinary lab user recorded nothing while looking like it had
                # worked. sudo is how the framework's other privileged steps run
                # (see _enable_promisc below), and it is the only option here:
                # file capabilities are not enough, because netsniff-ng also asks
                # for a realtime I/O priority, which needs CAP_SYS_ADMIN, and
                # granting that to a binary is no better than running it as root.
                cmd = [
                    "netsniff-ng",
                    "--silent" if self.silent else "",
                    "--in",
                    str(self.interface),
                    "--out",
                    f"'{self.pcap_file}'",
                    # Nanosecond, tcpdump/EBU-compatible pcap magic (0xa1b23c4d).
                    # netsniff-ng records hardware (on-wire) RX timestamps by
                    # default; the ns format preserves them at full resolution
                    # so RX interrupt coalescing cannot smear ST 2110-21 packet
                    # spacing (the usec default 0xa1b2c3d4 truncates them).
                    "-T",
                    "0xa1b23c4d",
                    (
                        f"--num {self.packets_capture}"
                        if self.packets_capture is not None
                        else ""
                    ),
                    "-f -" if self._filter_program else "",
                ]
                if self._filter_program:
                    # "-f -" reads the compiled filter from stdin, so the program
                    # never has to exist as a file on the capture host -- nothing
                    # to create, clean up, or leave behind when a capture is
                    # killed. Record separators travel as printf escapes because
                    # the whole thing is one SSH command line.
                    escaped = self._filter_program.replace("\n", "\\n")
                    cmd.insert(0, f"printf '%b' '{escaped}' |")
                # One "sh -c" so the SSH command has exactly one child whatever
                # the filter is: mfd_connect resolves the PID by listing the
                # wrapper shell's children and raises SSHPIDException when there
                # is more than one, which a top-level pipeline briefly shows.
                # That escapes start_process uncaught -- it is a sibling of
                # RemoteProcessInvalidState, not a subclass.
                inner = " ".join(cmd)
                command = f'sudo sh -c "{inner}"'
                logger.info(f"Running command: {command}")
                self.netsniff_process = connection.start_process(
                    command, stderr_to_stdout=True
                )
                logger.info(f"PCAP file will be saved at: {self.pcap_file}")

                if not self.netsniff_process.running:
                    err = self.netsniff_process.stdout_text
                    logger.error(f"netsniff-ng failed to start. Error output:\n{err}")
                    # No pcap was written; clear so teardown skips upload.
                    self.pcap_file = None
                    self._restore_promisc()
                    return False
                logger.info(
                    f"netsniff-ng started with PID {self.netsniff_process.pid}."
                )
                return True
            except ConnectionCalledProcessError as e:
                logger.error(f"Failed to start netsniff-ng: {e}")
                self.pcap_file = None
                self._restore_promisc()
                return False

    def capture(self, capture_time=None):
        """
        Starts netsniff-ng, captures packets count set in packets_capture or capture_time in seconds, then stops.
        :param capture_time: Fallback capture time in seconds if self.capture_time is not set (e.g. test_time)
        """
        # Use config capture_time first, fallback to self.capture_time
        effective_capture_time = capture_time if capture_time else self.capture_time
        started = self.start()
        if started:
            if self.packets_capture is None:
                logger.info(
                    f"Capturing traffic for {effective_capture_time} seconds..."
                )
                sleep(effective_capture_time or 0)
                self.stop()
                logger.info("Capture complete.")
            else:
                try:
                    logger.info(
                        f"Capturing traffic for {self.packets_capture} packets..."
                    )
                    # Use effective_capture_time as timeout to allow full test duration for packet capture
                    timeout = (effective_capture_time or 0) + 10

                    self.netsniff_process.wait(timeout=timeout)
                    logger.info("Capture complete.")
                    logger.debug(self.netsniff_process.stdout_text)
                except RemoteProcessTimeoutExpired:
                    logger.warning(
                        "Capture timed out. Probably not enough packets were sent. "
                        "Please adjust packets_capture or capture_time to the test case."
                    )
                    self.stop()
        else:
            logger.error("netsniff-ng did not start; skipping capture.")

    def stop(self):
        """
        Stops all netsniff-ng processes on the host using pkill.
        """
        if not self.netsniff_process:
            logger.debug("No netsniff-ng process to stop.")
            return

        # Check if process is still running before trying to stop
        if not self.netsniff_process.running:
            logger.debug("netsniff-ng process has already finished.")
            self._reap()
            self._restore_promisc()
            return

        try:
            logger.info("Stopping netsniff-ng...")
            self.netsniff_process.stop(wait=2)
        except SSHRemoteProcessEndException:
            try:
                self.netsniff_process.kill()
            except RemoteProcessInvalidState:
                logger.debug("Process already finished.")
        except RemoteProcessInvalidState:
            logger.debug("Process already finished.")
        else:
            logger.debug("netsniff-ng process stopped gracefully.")
        finally:
            self._reap()
            self._restore_promisc()

    def _reap(self):
        """Make sure no root ``netsniff-ng`` survives the process handle.

        ``start_process('sudo sh -c "netsniff-ng ..."')`` runs under ``bash -c``,
        so the handle above signals the bash wrapper and sudo does not pass the
        signal on to its child. The capture therefore keeps running as root,
        reparented to PID 1: it grows the pcap while it is being uploaded and
        holds the interface open for the next test. This is the same reason
        ``conftest._reap_ptp_daemons`` reaps ptp4l/phc2sys by argv, and the fix
        is the same -- ``pkill`` on the argv rather than on the handle.

        SIGTERM first, because netsniff-ng flushes and closes the pcap on it;
        SIGKILL only for anything that ignored that, where a truncated pcap is
        still better than a leaked capture.
        """
        connection = self.host.connection
        for signal_name in ("TERM", "KILL"):
            try:
                connection.execute_command(
                    f"sudo pkill -{signal_name} -x netsniff-ng || true",
                    expected_return_codes=None,
                )
            except Exception as e:
                logger.debug("pkill -%s netsniff-ng: %s", signal_name, e)
            if signal_name == "TERM":
                sleep(_REAP_GRACE_SEC)

    def _enable_promisc(self, connection):
        """Enable promiscuous mode on the capture interface; remember prior state.

        No-op when the interface has no kernel netdev (e.g. the PF is bound
        to vfio-pci for DPDK use). The caller will still try to start
        netsniff-ng, which will then fail loudly with a useful error.
        """
        try:
            res = connection.execute_command(
                f"ip -o link show dev {self.interface}", expected_return_codes=None
            )
            if res.return_code != 0:
                logger.warning(
                    "No kernel netdev for %s (rc=%s): %s",
                    self.interface,
                    res.return_code,
                    (res.stderr or res.stdout or "").strip(),
                )
                self._promisc_was_off = False
                return
            flags = (res.stdout or "").upper()
            if "PROMISC" in flags:
                self._promisc_was_off = False
                return
            set_res = connection.execute_command(
                f"sudo ip link set dev {self.interface} promisc on",
                expected_return_codes=None,
            )
            if set_res.return_code != 0:
                logger.warning(
                    "Failed to enable promisc on %s: %s",
                    self.interface,
                    (set_res.stderr or set_res.stdout or "").strip(),
                )
                self._promisc_was_off = False
                return
            self._promisc_was_off = True
            logger.debug("Enabled promiscuous mode on %s", self.interface)
        except Exception as e:
            logger.warning("Could not enable promisc on %s: %s", self.interface, e)
            self._promisc_was_off = False

    def _restore_promisc(self):
        if not self._promisc_was_off:
            return
        try:
            self.host.connection.execute_command(
                f"sudo ip link set dev {self.interface} promisc off",
                expected_return_codes=None,
            )
            logger.debug("Restored promiscuous mode off on %s", self.interface)
        except Exception as e:
            logger.warning("Could not restore promisc on %s: %s", self.interface, e)
        finally:
            self._promisc_was_off = False

    def update_filter(self, dst_ip=None, st2110_20_only: bool = False):
        """
        Restricts the capture to UDP addressed to one or more destination IPs.
        :param dst_ip: Destination IP to filter, or several (an ST 2022-7
            session's two copies go into one pcap so EBU judges both).
        :param st2110_20_only: True only when the capture will hold nothing but
            ST 2110-20 video, which is then truncated to ``_ST2110_20_SNAPLEN``.
            EBU LIST silently skips ST 2110-40 payload analysis on a truncated
            pcap, so truncating a capture that also carries ancillary data would
            report an unanalysed stream as compliant.
        """
        if not dst_ip:
            return
        dst_ips = (dst_ip,) if isinstance(dst_ip, str) else tuple(dst_ip)
        snaplen = _ST2110_20_SNAPLEN if st2110_20_only else _SNAPLEN_FULL
        self._filter_program = _bpf_program(dst_ips, snaplen)
        logger.info(
            "Capture filter: UDP to %s, %d octets per packet",
            " or ".join(dst_ips),
            snaplen,
        )
