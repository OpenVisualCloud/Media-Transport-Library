import logging

from mfd_connect.exceptions import ConnectionCalledProcessError

logger = logging.getLogger(__name__)


class Ramdisk:
    def __init__(self, host, mount_point, size_gib):
        self._host = host
        self._mount_point = mount_point
        self._size_gib = size_gib

    def is_mounted(self) -> bool:
        """Check if the mount point is currently mounted."""
        try:
            result = self._host.connection.execute_command(
                f"mountpoint -q {self._mount_point}"
            )
            return result.return_code == 0
        except ConnectionCalledProcessError:
            return False

    def mount(self):
        try:
            self._host.connection.execute_command(f"sudo mkdir -p {self._mount_point}")

            # Check if already mounted and unmount first
            if self.is_mounted():
                logger.warning(
                    f"Mount point {self._mount_point} is already mounted, unmounting first"
                )
                self.unmount()

            self._host.connection.execute_command(
                f"sudo mount -t tmpfs -o size={self._size_gib}G,nosuid,nodev,noexec tmpfs {self._mount_point}"
            )
        except ConnectionCalledProcessError as e:
            logger.error(f"Failed to mount ramdisk at {self._mount_point}: {e}")
            raise

    def unmount(self):
        """Unmount the ramdisk and remove the mount point directory."""
        try:
            if not self.is_mounted():
                logger.warning(f"Mount point {self._mount_point} is not mounted")
            else:
                self._host.connection.execute_command(
                    f"sudo umount {self._mount_point}"
                )
                logger.info(f"Successfully unmounted {self._mount_point}")
        except ConnectionCalledProcessError as e:
            logger.error(f"Failed to unmount {self._mount_point}: {e}")
            raise

        try:
            self._host.connection.execute_command(f"sudo rmdir {self._mount_point}")
            logger.info(f"Successfully removed directory {self._mount_point}")
        except ConnectionCalledProcessError as e:
            logger.warning(f"Could not remove directory {self._mount_point}: {e}")
