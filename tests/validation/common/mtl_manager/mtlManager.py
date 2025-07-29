import logging
from mfd_connect.exceptions import ConnectionCalledProcessError

logger = logging.getLogger(__name__)

class MtlManager:
    """
    Class to manage the lifecycle of the MtlManager process on a remote host.

    Attributes:
        host: Host object containing a .connection attribute for remote command execution.
        mtl_manager_process: The running MtlManager process object (if started).
    """

    def __init__(self, host):
        """
        Initialize the MtlManager with a host object.
        :param host: Host object with a .connection attribute.
        """
        self.host = host
        self.cmd = None
        self.mtl_manager_process = None

    def start(self):
        """
        Starts the MtlManager process on the remote host using sudo.
        Returns True if started successfully, False otherwise.
        """
        if not self.mtl_manager_process or not self.mtl_manager_process.running:
            connection = self.host.connection
            self.cmd = "sudo MtlManager"
            try:
                logger.info(f"Running command on host {self.host.name}: {self.cmd}")
                self.mtl_manager_process = connection.start_process(self.cmd, stderr_to_stdout=True)

                if not self.mtl_manager_process.running:
                    err = self.mtl_manager_process.stderr_text
                    logger.error(f"MtlManager failed to start. Error output:\n{err}")
                    return False
                logger.info(f"MtlManager started with PID {self.mtl_manager_process.pid}.")
                return True
            except ConnectionCalledProcessError as e:
                logger.error(f"Failed to start MtlManager: {e}")
                return False
        else:
            logger.info("MtlManager is already running.")
            return True
        pass

      
    def stop(self):
        """
        Stops all MtlManager processes on the remote host using sudo pkill.
        """
        connection = self.host.connection
        try:
            logger.info("Stopping MtlManager using sudo pkill MtlManager...")
            connection.execute_command("sudo pkill MtlManager")
            logger.info("MtlManager stopped (via pkill).")
        except ConnectionCalledProcessError as e:
            logger.error(f"Failed to stop MtlManager: {e}")
        pass
