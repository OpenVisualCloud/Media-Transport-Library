import logging
import re

from mfd_connect.exceptions import ConnectionCalledProcessError

logger = logging.getLogger(__name__)

# Allowed parent directories for ramdisk mount points
ALLOWED_MOUNT_PREFIXES = ("/mnt/ramdisk",)


class Ramdisk:
    def __init__(self, host, mount_point: str, size_gib: int):
        self._host = host
        self._mount_point = self._validate_mount_point(mount_point)
        self._size_gib = size_gib

    @staticmethod
    def _validate_mount_point(mount_point: str) -> str:
        """Validate mount point to prevent accidental deletion of important directories."""
        # Normalize path
        mount_point = "/" + mount_point.strip("/")

        # Safety checks
        if (
            not mount_point
            or ".." in mount_point
            or "//" in mount_point
            or not re.match(r"^[a-zA-Z0-9/_-]+$", mount_point)
        ):
            raise ValueError(f"Invalid mount point: {mount_point}")

        # Must be under allowed prefix with at least 2 path components
        path_parts = [p for p in mount_point.split("/") if p]
        if len(path_parts) < 2 or not any(
            mount_point.startswith(p) for p in ALLOWED_MOUNT_PREFIXES
        ):
            raise ValueError(
                f"Mount point must be under {ALLOWED_MOUNT_PREFIXES} "
                f"with at least 2 components: {mount_point}"
            )

        return mount_point

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
        """Mount a tmpfs ramdisk, unmounting any existing mount first."""
        try:
            if self.is_mounted():
                logger.warning(f"{self._mount_point} already mounted, unmounting first")
                self.unmount()

            self._host.connection.execute_command(f"sudo mkdir -p {self._mount_point}")
            self._host.connection.execute_command(
                f"sudo mount -t tmpfs -o size={self._size_gib}G,nosuid,nodev,noexec "
                f"tmpfs {self._mount_point}"
            )
            logger.info(f"Mounted ramdisk at {self._mount_point}")
        except ConnectionCalledProcessError as e:
            logger.error(f"Failed to mount ramdisk at {self._mount_point}: {e}")
            raise

    def unmount(self):
        """Unmount the ramdisk and remove the mount point directory."""
        try:
            if self.is_mounted():
                self._host.connection.execute_command(
                    f"sudo umount {self._mount_point}"
                )
                logger.info(f"Unmounted {self._mount_point}")

            # Safe to remove - already validated in __init__, check not mounted
            self._host.connection.execute_command(f"sudo rm -rf {self._mount_point}")
            logger.debug(f"Removed directory {self._mount_point}")
        except ConnectionCalledProcessError as e:
            logger.warning(f"Cleanup issue at {self._mount_point}: {e}")
