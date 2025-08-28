import logging

from mfd_connect.exceptions import ConnectionCalledProcessError


class Ramdisk:
    def __init__(self, host, mount_point, size_gib):
        self._host = host
        self._mount_point = mount_point
        self._size_gib = size_gib

    def mount(self):
        try:
            self._host.connection.execute_command(f"mkdir -p {self._mount_point}")
            self._host.connection.execute_command(
                f"mount -t ramfs -o size={self._size_gib}G ramfs {self._mount_point}"
            )
        except ConnectionCalledProcessError as e:
            logging.log(level=logging.ERROR, msg=f"Failed to execute command: {e}")

    def unmount(self):
        try:
            self._host.connection.execute_command(f"umount {self._mount_point}")
            self._host.connection.execute_command(f"rmdir {self._mount_point}")
        except ConnectionCalledProcessError as e:
            logging.log(level=logging.ERROR, msg=f"Failed to execute command: {e}")
