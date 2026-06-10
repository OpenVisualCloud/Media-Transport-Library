# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2025 Intel Corporation
import datetime
import logging
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


def calculate_packets_per_frame(media_file_info, mtu: int = 1500) -> int:
    # Simplified calculation for the number of packets per frame
    # Supported only 4:2:2 format or audio formats assuming 1 packet per 1ms
    packets = 1000
    if not media_file_info:
        raise ValueError("Missing media file info; cannot calculate packets per frame.")
    if "width" in media_file_info and "height" in media_file_info:
        pgroupsize = 5
        pgroupcoverage = 2
        headersize = 74  # Ethernet + IP + UDP + RTP headers
        packets = 1 + int(
            (media_file_info["width"] * media_file_info["height"])
            / (int((mtu - headersize) / pgroupsize) * pgroupcoverage)
        )
    return packets


class NetsniffRecorder:
    """
    Class to handle the recording of network traffic using netsniff-ng.

    Captures with hardware (on-wire) RX timestamps, which netsniff-ng uses by
    default when the NIC supports them, written in the nanosecond pcap format
    (``-T 0xa1b23c4d``). This is immune to NIC RX interrupt coalescing, so
    ST 2110-21 Cinst/VRX are derived from true on-wire packet spacing. The
    timestamps originate from the NIC PHC, so the capture fixture also
    disciplines that PHC to the system clock (phc2sys) for the absolute-offset
    check.

    Attributes:
        host: Host object containing connection and network interfaces.
        test_name (str): Name for the capture session (used for file naming).
        pcap_dir (str): Directory to store the pcap files.
        interface: Network interface to capture traffic on.
        interface_index (int): Index of the network interface if not specified by interface.
        silent (bool): Whether to run netsniff-ng in silent mode (no stdout) (default: True).
        capture_filter (str): Optional filter to apply to the capture. (default: None)
    """

    def __init__(
        self,
        host,
        test_name: str,
        pcap_dir: str,
        interface=None,
        interface_index: int = 0,
        silent: bool = True,
        capture_filter: str | None = None,
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
        self.capture_filter = capture_filter
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
                    f'-f "{self.capture_filter}"' if self.capture_filter else "",
                ]
                logger.info(f"Running command: {' '.join(cmd)}")
                self.netsniff_process = connection.start_process(
                    " ".join(cmd), stderr_to_stdout=True
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
            self._restore_promisc()

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

    def update_filter(self, src_ip=None, dst_ip=None):
        """
        Updates the capture filter with new source and/or destination IP addresses.
        :param src_ip: New source IP address to filter (optional).
        :param dst_ip: New destination IP address to filter (optional).
        """
        filters = []
        if src_ip:
            filters.append(f"src {src_ip}")
        if dst_ip:
            filters.append(f"dst {dst_ip}")
        if len(filters) > 1:
            self.capture_filter = " and ".join(filters)
        elif len(filters) == 1:
            self.capture_filter = filters[0]
        logger.info(f"Updated capture filter to: {self.capture_filter}")
