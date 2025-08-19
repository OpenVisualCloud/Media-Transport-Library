# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2025 Intel Corporation
import logging
import os
from datetime import datetime
from time import sleep

from mfd_connect.exceptions import ConnectionCalledProcessError

logger = logging.getLogger(__name__)

STARTUP_WAIT = 2  # Default wait time after starting the process

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
        filter (str): Optional filter to apply to the capture. (default: None)
    """

    def __init__(
            self,
            host,
            test_name: str,
            pcap_dir: str,
            interface = None,
            interface_index: int = 0,
            silent: bool = True,
            filter: str | None = None,
        ):
        self.host = host
        self.test_name = test_name
        self.pcap_dir = pcap_dir
        self.pcap_file = os.path.join(pcap_dir, f"{test_name}_{datetime.now().strftime('%Y-%m-%d_%H%M%S')}.pcap")
        if interface is not None:
            self.interface = interface
        else:
            self.interface = self.host.network_interfaces[interface_index].name
        self.netsniff_process = None
        self.silent = silent
        self.filter = filter

    def start(self, startup_wait=STARTUP_WAIT):
        """
        Starts the netsniff-ng
        """
        if not self.netsniff_process or not self.netsniff_process.running:
            connection = self.host.connection
            try:
                cmd = [
                    "netsniff-ng",
                    "--silent" if self.silent else "",
                    "--in",
                    str(self.interface),
                    "--out",
                    self.pcap_file,
                    f"-f \"{self.filter}\"" if self.filter else "",
                ]
                logger.info(f"Running command: {' '.join(cmd)}")
                self.netsniff_process = connection.start_process(
                        " ".join(cmd), stderr_to_stdout=True
                )
                logger.info(
                    f"PCAP file will be saved at: {os.path.abspath(self.pcap_file)}"
                )

                # Give netsniff-ng a moment to start and possibly error out
                sleep(startup_wait)

                if not self.netsniff_process.running:
                    err = self.netsniff_process.stderr_text
                    logger.error(f"netsniff-ng failed to start. Error output:\n{err}")
                    return False
                logger.info(f"netsniff-ng started with PID {self.netsniff_process.pid}.")
                return True
            except ConnectionCalledProcessError as e:
                    logger.error(f"Failed to start netsniff-ng: {e}")
                    return False


    def capture(self, capture_time=20, startup_wait=2):
        """
        Starts netsniff-ng, captures for capture_time seconds, then stops.
        :param capture_time: Duration in seconds to capture packets.
        :param startup_wait: Time in seconds to wait after starting netsniff-ng (default: 2)
        """
        started = self.start(startup_wait=startup_wait)
        if started:
            logger.info(f"Capturing traffic for {capture_time} seconds...")
            sleep(capture_time)
            self.stop()
            logger.info("Capture complete.")
        else:
            logger.error("netsniff-ng did not start; skipping capture.")


    def stop(self):
        """
        Stops all netsniff-ng processes on the host using pkill.
        """
        connection = self.host.connection
        try:
            logger.info("Stopping netsniff-ng using pkill netsniff-ng...")
            connection.execute_command("pkill netsniff-ng")
            logger.info("netsniff-ng stopped (via pkill).")
        except ConnectionCalledProcessError as e:
            logger.error(f"Failed to stop netsniff-ng: {e}")
