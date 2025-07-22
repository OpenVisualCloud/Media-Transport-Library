import logging

logger = logging.getLogger(__name__)


class VideoIntegrityRunner:
    module_name = "video_integrity.py"

    def __init__(
        self,
        host,
        test_repo_path,
        src_url: str,
        out_name: str,
        resolution: str,
        file_format: str = "yuv422p10le",
        out_path: str = "/mnt/ramdisk",
        delete_file: bool = True,
        python_path=None,
        integrity_path=None,
    ):
        self.host = host
        self.test_repo_path = test_repo_path
        self.integrity_path = self.get_path(integrity_path)
        self.resolution = resolution
        self.file_format = file_format
        self.src_url = src_url
        self.out_name = out_name
        self.out_path = out_path
        self.delete_file = delete_file
        self.python_path = python_path or "python3"

    def get_path(self, integrity_path):
        """
        Returns the path to the integrity module.
        If integrity_path is provided, it uses that; otherwise, it constructs the path based on the test_repo_path.
        """
        if integrity_path:
            return str(self.host.connection.path(integrity_path, self.module_name))
        return str(
            self.host.connection.path(
                self.test_repo_path, "tests", "common", "integrity", self.module_name
            )
        )

    def setup(self):
        """
        Setup method to prepare the environment for running the integrity check.
        This can include creating directories, checking dependencies, etc.
        """
        logger.info(
            f"Setting up integrity check on {self.host.name} for {self.out_name}"
        )
        self.host.connection.execute_command("apt install tesseract-ocr -y", shell=True)
        reqs = str(
            self.host.connection.path(self.integrity_path).parents[0]
            / "requirements.txt"
        )
        for library in ["pytesseract", "opencv-python"]:
            cmd = f"{self.python_path} -m pip list | grep {library} || {self.python_path} -m pip install -r {reqs}"
            self.host.connection.execute_command(cmd, shell=True)


class FileVideoIntegrityRunner(VideoIntegrityRunner):
    def __init__(
        self,
        host,
        test_repo_path,
        src_url: str,
        out_name: str,
        resolution: str,
        file_format: str = "yuv422p10le",
        out_path: str = "/mnt/ramdisk",
        delete_file: bool = True,
        python_path=None,
        integrity_path=None,
    ):
        super().__init__(
            host,
            test_repo_path,
            src_url,
            out_name,
            resolution,
            file_format,
            out_path,
            delete_file,
            python_path,
            integrity_path,
        )

    def run(self):
        cmd = " ".join(
            [
                self.python_path,
                self.integrity_path,
                "file",
                self.src_url,
                self.out_name,
                self.resolution,
                self.file_format,
                "--output_path",
                self.out_path,
                "--delete_file" if self.delete_file else "--no_delete_file",
            ]
        )
        logger.debug(
            f"Running integrity check on {self.host.name} for {self.out_name} with command: {cmd}"
        )
        result = self.host.connection.execute_command(
            cmd, shell=True, stderr_to_stdout=True, expected_return_codes=(0, 1)
        )
        if result.return_code > 0:
            logger.error(f"Integrity check failed on {self.host.name}: {self.out_name}")
            logger.error(result.stdout)
            return False
        logger.info(
            f"Integrity check completed successfully on {self.host.name} for {self.out_name}"
        )
        return True


class StreamVideoIntegrityRunner(VideoIntegrityRunner):
    def __init__(
        self,
        host,
        test_repo_path,
        src_url: str,
        out_name: str,
        resolution: str,
        file_format: str = "yuv422p10le",
        out_path: str = "/mnt/ramdisk",
        delete_file: bool = True,
        python_path=None,
        integrity_path=None,
        segment_duration: int = 3,
        workers: int = 5,
    ):
        super().__init__(
            host,
            test_repo_path,
            src_url,
            out_name,
            resolution,
            file_format,
            out_path,
            delete_file,
            python_path,
            integrity_path,
        )
        self.segment_duration = segment_duration
        self.workers = workers
        self.process = None

    def run(self):
        cmd = " ".join(
            [
                self.python_path,
                self.integrity_path,
                "stream",
                self.src_url,
                self.out_name,
                self.resolution,
                self.file_format,
                "--output_path",
                self.out_path,
                "--delete_file" if self.delete_file else "--no_delete_file",
                "--segment_duration",
                str(self.segment_duration),
                "--workers",
                str(self.workers),
            ]
        )
        logger.debug(
            f"Running stream integrity check on {self.host.name} for {self.out_name} with command: {cmd}"
        )
        self.process = self.host.connection.start_process(
            cmd, shell=True, stderr_to_stdout=True
        )

    def stop(self, timeout: int = 10):
        if self.process:
            self.process.wait(timeout)
            logger.info(
                f"Stream integrity check stopped on {self.host.name} for {self.out_name}"
            )
        else:
            logger.warning(
                f"No active process to stop for {self.out_name} on {self.host.name}"
            )

    def stop_and_verify(self, timeout: int = 10):
        """
        Stops the stream integrity check and verifies the output.
        """
        self.stop(timeout)
        if self.process.return_code != 0:
            logger.error(
                f"Stream integrity check failed on {self.host.name} for {self.out_name}"
            )
            logger.error(f"Process output: {self.process.stdout_text}")
            return False
        logger.info(
            f"Stream integrity check completed successfully on {self.host.name} for {self.out_name}"
        )
        return True
