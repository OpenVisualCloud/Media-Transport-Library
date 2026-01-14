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
        capture_time: int = 5,
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
            try:
                self.pcap_file = self._build_pcap_path()
                cmd = [
                    "netsniff-ng",
                    "--silent" if self.silent else "",
                    "--in",
                    str(self.interface),
                    "--out",
                    f"'{self.pcap_file}'",
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
                    return False
                logger.info(
                    f"netsniff-ng started with PID {self.netsniff_process.pid}."
                )
                return True
            except ConnectionCalledProcessError as e:
                logger.error(f"Failed to start netsniff-ng: {e}")
                return False

    def capture(self, startup_wait=2):
        """
        Starts netsniff-ng, captures packets count set in packets_capture or capture_time in seconds, then stops.
        :param startup_wait: Time in seconds to wait after starting netsniff-ng (default: 2)
        """
        started = self.start()
        if started:
            if self.packets_capture is None:
                logger.info(f"Capturing traffic for {self.capture_time} seconds...")
                sleep(self.capture_time + startup_wait)
                self.stop()
                logger.info("Capture complete.")
            else:
                try:
                    logger.info(
                        f"Capturing traffic for {self.packets_capture} packets..."
                    )
                    self.netsniff_process.wait(timeout=startup_wait + 10)
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

        try:
            logger.info("Stopping netsniff-ng using pkill netsniff-ng...")
            self.netsniff_process.stop(wait=5)
        except SSHRemoteProcessEndException:
            try:
                self.netsniff_process.kill()
            except RemoteProcessInvalidState:
                logger.debug("Process killed.")
        logger.error("netsniff-ng process did not stopped by itself.")
        logger.error(self.netsniff_process.stdout_text)

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
