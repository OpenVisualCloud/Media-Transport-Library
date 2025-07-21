# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2025 Intel Corporation
# Media Communications Mesh
import logging
import threading
import time

from mcm.Engine.const import LOG_FOLDER
from mcm.Engine.mcm_apps import save_process_log
from mfd_connect.exceptions import (
    RemoteProcessInvalidState,
    SSHRemoteProcessEndException,
)

from .ffmpeg_io import FFmpegIO

SLEEP_BETWEEN_CHECKS = 0.5  # time to wait for another check if process is running
RETRIES = 4  # how many times will the stop() be retried; for FFmpeg - minimum of 3 is required!

logger = logging.getLogger(__name__)


class FFmpeg:
    def __init__(
        self,
        prefix_variables: dict = {},
        ffmpeg_path: str = "ffmpeg",  # default: from $PATH
        ffmpeg_input: FFmpegIO | None = None,
        ffmpeg_output: FFmpegIO | None = None,
        yes_overwrite: bool = False,  # true => -y at the end, false => no -y
    ):
        self.prefix_variables = prefix_variables
        self.ffmpeg_path = ffmpeg_path
        self.ffmpeg_input = ffmpeg_input
        self.ffmpeg_output = ffmpeg_output
        self.yes_overwrite = yes_overwrite

    def get_items(self) -> dict:
        response = {
            "ffmpeg_path": self.ffmpeg_path,
            "ffmpeg_input": {},
            "ffmpeg_output": {},
            "yes_overwrite": self.yes_overwrite,
        }
        if self.ffmpeg_input:
            response["ffmpeg_input"] = self.ffmpeg_input.get_items()
        if self.ffmpeg_output:
            response["ffmpeg_output"] = self.ffmpeg_output.get_items()
        # TODO: Check if it takes yes_overwrite into account, and if should take prefix_variables into account
        return response

    def get_command(self) -> str:
        prefix = ""
        for key, value in self.prefix_variables.items():
            prefix += f"{key}={value} "
        response = f"{prefix}{self.ffmpeg_path}"
        if self.ffmpeg_input:
            response += self.ffmpeg_input.get_command()
        if self.ffmpeg_output:
            response += self.ffmpeg_output.get_command()
        if self.yes_overwrite:
            response += " -y"
        return response


class FFmpegExecutor:
    """
    FFmpeg wrapper with MCM plugin
    """

    def __init__(self, host, ffmpeg_instance: FFmpeg, log_path=None):
        self.host = host
        self.ff = ffmpeg_instance
        self.log_path = log_path
        self._processes = []

    def start(self):
        """Starts the FFmpeg process on the host, waits for the process to start."""
        cmd = self.ff.get_command()
        ffmpeg_process = self.host.connection.start_process(
            cmd, stderr_to_stdout=True, shell=True
        )
        self._processes.append(ffmpeg_process)
        CURRENT_RETRIES = RETRIES
        retries_counter = 0
        while (
            not ffmpeg_process.running and retries_counter <= CURRENT_RETRIES
        ):  # wait for the process to start
            retries_counter += 1
            time.sleep(SLEEP_BETWEEN_CHECKS)
        # FIXME: Find a better way to check if the process is running; code below throws an error when the process is actually running sometimes
        if ffmpeg_process.running:
            logger.info(
                f"FFmpeg process started on {self.host.name} with command: {cmd}"
            )
        else:
            logger.debug(
                f"FFmpeg process failed to start on {self.host.name} after {CURRENT_RETRIES} retries."
            )

        log_dir = self.log_path if self.log_path else LOG_FOLDER
        subdir = f"RxTx/{self.host.name}"

        is_receiver = False

        if self.ff.ffmpeg_input and self.ff.ffmpeg_output:
            input_path = getattr(self.ff.ffmpeg_input, "input_path", None)
            output_path = getattr(self.ff.ffmpeg_output, "output_path", None)

            if input_path == "-" or (
                output_path and output_path != "-" and "." in output_path
            ):
                is_receiver = True

        filename = "ffmpeg_rx.log" if is_receiver else "ffmpeg_tx.log"
        input_class_name = None
        if self.ff.ffmpeg_input:
            input_class_name = self.ff.ffmpeg_input.__class__.__name__
        prefix = "mtl_" if input_class_name and "Mtl" in input_class_name else "mcm_"
        filename = prefix + filename

        def log_output():
            for line in ffmpeg_process.get_stdout_iter():
                save_process_log(
                    subdir=subdir,
                    filename=filename,
                    text=line.rstrip(),
                    cmd=cmd,
                    log_dir=log_dir,
                )

        threading.Thread(target=log_output, daemon=True).start()

    # TODO: Think about a better way to handle the default wait time
    def stop(self, wait: float = 0.0) -> float:
        elapsed = 0.0
        CURRENT_RETRIES = RETRIES
        for process in self._processes:
            try:
                while process.running and wait >= 0:  # count the wait time down
                    time.sleep(SLEEP_BETWEEN_CHECKS)
                    elapsed += SLEEP_BETWEEN_CHECKS
                    wait -= SLEEP_BETWEEN_CHECKS
                while (
                    process.running and CURRENT_RETRIES >= 0
                ):  # if wait has passed, stop the process
                    process.stop()
                    CURRENT_RETRIES -= 1
                    time.sleep(SLEEP_BETWEEN_CHECKS)
                    elapsed += SLEEP_BETWEEN_CHECKS
                if (
                    process.running
                ):  # avoid trying to read the logs before the process is stopped
                    process.kill()
            except SSHRemoteProcessEndException as e:
                logger.warning(
                    f"FFmpeg process on {self.host.name} was already stopped: {e}"
                )
            except RemoteProcessInvalidState as e:
                logger.warning(
                    f"FFmpeg process on {self.host.name} is in an invalid state: {e}"
                )
            except Exception as e:
                logger.error(
                    f"Error while stopping FFmpeg process on {self.host.name}: {e}"
                )
                raise e
            logger.debug(
                f">>> FFmpeg execution on '{self.host.name}' host returned:\n{process.stdout_text}"
            )
            if process.return_code != 0:
                logger.warning(
                    f"FFmpeg process on {self.host.name} return code is {process.return_code}"
                )
            # assert process.return_code == 0 # Sometimes a different return code is returned for a graceful stop, so we do not assert it here
        else:
            logger.info("No FFmpeg process to stop!")
        return elapsed


def no_proxy_to_prefix_variables(host, prefix_variables: dict | None = None):
    """
    Handles the no_proxy and NO_PROXY environment variables for FFmpeg execution.

    Decision table for no_proxy and NO_PROXY:
    | test_config | decision                            | topology_config |
    | ----------- | ----------------------------------- | --------------- |
    | present     | use test_config's no_proxy (<-)     | present         |
    | present     | use test_config's no_proxy (<-)     | not present     |
    | not present | use topology_config's no_proxy (->) | present         |
    | not present | do not use no_proxy (return {})     | not present     |

    Lower-case no_proxy from topology_config is propagated to prefix_variables' upper-case NO_PROXY.
    if test_config defines NO_PROXY or no_proxy – it will be added to the prefix_variables (overwrite topology_config);
    else it will try to return the no_proxy from topology_config's media_proxy;
    else return an empty prefix_variables dictionary.
    """
    prefix_variables = prefix_variables if prefix_variables else {}
    try:
        if "no_proxy" not in prefix_variables.keys():
            prefix_variables["no_proxy"] = host.topology.extra_info.media_proxy.get(
                "no_proxy"
            )
        if "NO_PROXY" not in prefix_variables.keys():
            prefix_variables["NO_PROXY"] = host.topology.extra_info.media_proxy.get(
                "no_proxy"
            )  # lower-case getter on purpose
    except KeyError:  # when no extra_info or media_proxy is set, return {}
        pass
    finally:
        return prefix_variables
