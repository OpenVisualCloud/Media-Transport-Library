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
        self.cmd = "sudo MtlManager"
        self.mtl_manager_process = None

    def start(self):
        """
        Starts the MtlManager process on the remote host using sudo.
        Returns True if started successfully, False otherwise.
        """
        if not self.mtl_manager_process or not self.mtl_manager_process.running:
            connection = self.host.connection
            try:
                logger.info(f"Running command on host {self.host.name}: {self.cmd}")
                self.mtl_manager_process = connection.start_process(
                    self.cmd, stderr_to_stdout=True
                )

                if not self.mtl_manager_process.running:
                    err = self.mtl_manager_process.stderr_text
                    logger.error(f"MtlManager failed to start. Error output:\n{err}")
                    return False
                logger.info(
                    f"MtlManager started with PID {self.mtl_manager_process.pid}."
                )
                return True
            except ConnectionCalledProcessError as e:
                logger.error(f"Failed to start MtlManager: {e}")
                return False
        else:
            logger.info("MtlManager is already running.")
            return True

    def stop(self):
        """
        Stops the MtlManager process on the remote host.
        First attempts to gracefully stop the process if it's tracked,
        then falls back to pkill if needed.
        """
        if self.mtl_manager_process and self.mtl_manager_process.running:
            try:
                logger.info("Stopping MtlManager using process object methods...")
                # Try graceful termination first
                self.mtl_manager_process.stop()

                # Check if the process stopped gracefully
                if self.mtl_manager_process.running:
                    logger.info("MtlManager still running, trying kill...")
                    self.mtl_manager_process.kill()

                # Check logs for errors
                log_output = self.mtl_manager_process.stdout_text
                if log_output:
                    if "error" in log_output.lower() or "fail" in log_output.lower():
                        logger.error(f"Errors found in MtlManager logs: {log_output}")

                logger.info("MtlManager stopped successfully.")
                return
            except Exception as e:
                logger.error(f"Error while stopping MtlManager process: {e}")

        # Fallback to pkill if the process object is not available or the above failed
        connection = self.host.connection
        try:
            logger.info("Stopping MtlManager using sudo pkill MtlManager (fallback)...")
            connection.execute_command("sudo pkill MtlManager")
            logger.info("MtlManager stopped (via pkill).")
        except ConnectionCalledProcessError as e:
            logger.error(f"Failed to stop MtlManager via pkill: {e}")
