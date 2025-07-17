import logging

from mfd_connect.exceptions import ConnectionCalledProcessError

logger = logging.getLogger(__name__)


class RamdiskPreparer:
    """
    Prepares a directory for storing PCAP files by creating it if it doesn't exist,
    setting permissions, and mounting it as a tmpfs with the specified size.
    Uses the host's connection object for command execution.
    """

    def __init__(self, host, pcap_dir, tmpfs_size="768G", tmpfs_name="new_disk_name", use_sudo=True):
        self.host = host
        self.connection = self.host.connection
        self.pcap_dir = pcap_dir
        self.tmpfs_size = tmpfs_size
        self.tmpfs_name = tmpfs_name
        self.use_sudo = use_sudo

    def start(self):
        """
        Prepares the ramdisk directory and mounts tmpfs.
        Enables and disables sudo if self.use_sudo is True.
        """
        cmds = [
            f"mkdir -p {self.pcap_dir}",
            f"chmod 777 {self.pcap_dir}",
            f"""if ! grep -q "{self.pcap_dir} " /proc/mounts; then """
            f"sudo mount -t tmpfs -o size={self.tmpfs_size} {self.tmpfs_name} {self.pcap_dir} && "
            f"chmod 777 {self.pcap_dir}; "
            f"fi"
            "",
        ]
        full_cmd = " && ".join(cmds)

        logger.info(f"Preparing ramdisk with command: {full_cmd}")
        try:
            # if self.use_sudo:
            #     self.connection.enable_sudo()
            result = self.connection.execute_command(full_cmd)
            logger.info(f"Ramdisk prepared: {result}")
        except ConnectionCalledProcessError as e:
            logger.error(f"Failed to prepare ramdisk: {e}")
            return False
        finally:
            # if self.use_sudo:
            #     self.connection.disable_sudo()
            pass


# Example usage:
# prepare_ramdisk = RamdiskPreparer(host, "/home/pcap_files")
# prepare_ramdisk.start()
#
# Example usage with custom size and name:
# prepare_ramdisk = RamdiskPreparer(host, "/home/pcap_files", tmpfs_size="100G", tmpfs_name="myramdisk")
# prepare_ramdisk.start()
