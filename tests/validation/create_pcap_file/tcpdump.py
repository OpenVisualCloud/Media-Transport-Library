import os
import time
import logging

from mfd_connect.exceptions import ConnectionCalledProcessError

logger = logging.getLogger(__name__)

class TcpDumpRecorder:
    """
    Class to manage the lifecycle of a tcpdump process for capturing network traffic.

    Attributes:
        host: Host object containing connection and network interfaces.
        test_name (str): Name for the capture session (used for file naming).
        pcap_dir (str): Directory to store the pcap files.
        tcpdump_process: The running tcpdump process object.
        pcap_file (str): Full path to the output pcap file.
    """

    def __init__(self, host, test_name, pcap_dir, interface=None, interface_index=0):
        self.host = host
        # Allow selection by name or index
        if interface is not None:
            self.interface = interface
        else:
            self.interface = self.host.network_interfaces[interface_index].name
        self.test_name = test_name
        self.pcap_dir = pcap_dir
        self.pcap_file = os.path.join(pcap_dir, f"{test_name}.pcap")
        self.tcpdump_process = None

    def start(self, startup_wait=2):
        """
        Starts the tcpdump process with sudo.
        :param startup_wait: Time in seconds to wait after starting tcpdump (default: 2)
        """
        if not self.tcpdump_process or not self.tcpdump_process.running:
            connection = self.host.connection
            try:
                cmd = [
                    "sudo",
                    "tcpdump",
                    "-i", self.interface,
                    "-s", "65535",
                    "-w", self.pcap_file,
                    "--time-stamp-type=adapter_unsynced"
                ]
                logger.info(f"Running command: {' '.join(cmd)}")
                self.tcpdump_process = connection.start_process(" ".join(cmd), stderr_to_stdout=True)
                logger.info(f"PCAP file will be saved at: {os.path.abspath(self.pcap_file)}")
                time.sleep(startup_wait) # Give tcpdump a moment to start and possibly error out
                if not self.tcpdump_process.running:
                    err = self.tcpdump_process.stderr_text
                    logger.error(f"tcpdump failed to start. Error output:\n{err}")
                    return False
                logger.info(f"tcpdump started with PID {self.tcpdump_process.pid}.")
                return True
            except ConnectionCalledProcessError as e:
                logger.error(f"Failed to start tcpdump: {e}")
                return False
            
    def capture(self, capture_time=20, startup_wait=2):
        """
        Starts tcpdump, captures for capture_time seconds, then stops.
        :param capture_time: Duration in seconds to capture packets.
        :param startup_wait: Time in seconds to wait after starting tcpdump (default: 2)
        """
        started = self.start(startup_wait=startup_wait)
        if started:
            logger.info(f"Capturing traffic for {capture_time} seconds...")
            time.sleep(capture_time)
            self.stop()
            logger.info("Capture complete.")
        else:
            logger.error("tcpdump did not start; skipping capture.")

    def stop(self):
        """
        Stops all tcpdump processes on the host using sudo pkill.
        """
        connection = self.host.connection
        try:
            logger.info("Stopping tcpdump using sudo pkill tcpdump...")
            connection.execute_command("sudo pkill tcpdump")
            logger.info("tcpdump stopped (via pkill).")
        except ConnectionCalledProcessError as e:
            logger.error(f"Failed to stop tcpdump: {e}")

# Example usage:
# Default, first network interface will be used
#   tcpdump = TcpDumpRecorder(host, "test8", "/home/pcap_files")
# Specifying interface by name
#   tcpdump = TcpDumpRecorder(host, "test8", "/home/pcap_files", interface="eth0")
# Specifying interface by index
#   tcpdump = TcpDumpRecorder(host, "test8", "/home/pcap_files", interface_index=0)

# Manual start/stop usage:
# started = tcpdump.start()
# if started:
#     # ... do something ...
#     tcpdump.stop()
#
# Capture for a specific duration (e.g., 500 ms):
# tcpdump.capture(capture_time=0.5)
