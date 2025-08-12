import logging

from mfd_connect.exceptions import ConnectionCalledProcessError


class Ramdisk:
    def __init__(self, host, mount_point, size_gib):
        self._host = host
        self._mount_point = mount_point
        self._size_gib = size_gib

    def mount(self):
        cmd = f"mkdir -p {self._mount_point} && mount -t ramfs -o size={self._size_gib}G ramfs {self._mount_point}"
        try:
            self._host.connection.execute_command(cmd)
        except ConnectionCalledProcessError as e:
            logging.log(
                level=logging.ERROR, msg=f"Failed to execute command {cmd}: {e}"
            )

    def unmount(self):
        cmd = f"umount {self._mount_point} && rmdir {self._mount_point}"
        try:
            self._host.connection.execute_command(cmd)
        except ConnectionCalledProcessError as e:
            logging.log(
                level=logging.ERROR, msg=f"Failed to execute command {cmd}: {e}"
            )
