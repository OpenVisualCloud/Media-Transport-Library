# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation
import logging
import os
import subprocess
import threading
import time
from queue import Queue
from typing import Any, List

import pytest
from mfd_common_libs.log_levels import TEST_FAIL
from pytest_check import check

from .const import LOG_FOLDER, TESTCMD_LVL
from .stash import add_result_log, set_result_note

logger = logging.getLogger(__name__)


class RaisingThread(threading.Thread):
    """Thread that passes exception to caller."""

    def run(self):
        """Store exception if raised."""
        self._exception = None
        self._result = None
        try:
            self._result = self._target(*self._args, **self._kwargs)
        except BaseException as e:
            self._exception = e

    def join(self, timeout=None) -> Any:
        """Raise exception back to caller.

        :param timeout: timeout
        """
        super().join(timeout=timeout)
        if self._exception:
            raise self._exception
        if self._result:
            return self._result
        return None


class AsyncProcess:
    def __init__(
        self,
        process: subprocess.Popen,
        reader: threading.Thread,
        timer: threading.Timer,
    ):
        self.process = process
        self.reader = reader
        self.timer = timer
        self.output = ""


def killproc(proc: subprocess.Popen, sigint: bool = False):
    """Kill a process gracefully, then forcefully if needed.
    
    This function ensures proper cleanup of DPDK processes which may
    hold VFIO resources. It tries SIGINT/SIGTERM first, then SIGKILL.
    
    Args:
        proc: Process to kill
        sigint: If True, use SIGINT; otherwise use SIGTERM
        
    Returns:
        Process return code
    """
    result = proc.poll()
    if result is not None:
        logger.debug(f"Process {proc.pid} already terminated with RC {result}")
        return result

    logger.debug(f"Terminating process {proc.pid} (sigint={sigint})")
    
    # try to 'gently' terminate proc
    if sigint:
        proc.send_signal(2)  # SIGINT
    else:
        proc.terminate()  # SIGTERM

    # Wait up to 5 seconds for graceful termination
    time.sleep(5)

    # Check multiple times with delays
    for attempt in range(5):
        result = proc.poll()
        if result is not None:
            logger.debug(f"Process {proc.pid} terminated gracefully with RC {result}")
            return result
        time.sleep(5)  # wait a little longer for proc to terminate

    # failed to terminate proc, so kill it
    logger.warning(f"Process {proc.pid} did not terminate gracefully, forcing kill")
    proc.kill()  # SIGKILL
    
    for attempt in range(10):
        result = proc.poll()
        if result is not None:
            logger.debug(f"Process {proc.pid} killed with RC {result}")
            return result
        time.sleep(5)  # give system more time to kill proc

    # failed to kill proc
    if result is None:
        logger.error(f"Failed to kill process with pid {proc.pid}")
        # Last resort: try system kill command
        try:
            subprocess.run(f"kill -9 {proc.pid}", shell=True, timeout=5)
            time.sleep(2)
            result = proc.poll()
        except Exception as e:
            logger.error(f"System kill also failed: {e}")
    
    return result


def readproc(process: subprocess.Popen):
    case_id = os.environ["PYTEST_CURRENT_TEST"]
    case_id = case_id[: case_id.rfind("(") - 1]
    logfile = os.path.join(LOG_FOLDER, "latest", f"{case_id}.pid{process.pid}.log")

    output = []
    with open(logfile, "w") as file:
        if process.stdout is not None:
            for line in iter(process.stdout.readline, ""):
                output.append(line)
                file.write(line)
    return "".join(output)


def call(
    command: str,
    cwd: str,
    timeout: int = 60,
    sigint: bool = False,
    env: dict = None,
    host=None,
) -> AsyncProcess:
    processes = calls(
        [command], cwd=cwd, timeout=timeout, sigint=sigint, env=env, host=host
    )
    return processes[0]


def calls(
    commands: List[str],
    cwd: str = None,
    timeout: int = 60,
    sigint: bool = False,
    env: dict = None,
    host=None,
) -> List[AsyncProcess]:
    ret = []
    for command in commands:
        if host is None:
            # Local execution
            process = subprocess.Popen(
                "exec " + command,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                shell=True,
                text=True,
                cwd=cwd,
                env=env,
            )
            logger.debug(f"PID: {process.pid}")
            reader = RaisingThread(target=readproc, args=[process])
            reader.daemon = True
            reader.start()
            timer = None
            if timeout > 0:
                timer = threading.Timer(timeout, killproc, args=[process, sigint])
                timer.daemon = True
                timer.start()
            ret.append(AsyncProcess(process=process, reader=reader, timer=timer))
        else:
            # Remote execution using mfd_connect
            process = host.connection.start_process(
                command,
                shell=True,
                stderr_to_stdout=True,
                cwd=cwd,
                env=env,
            )

            # Wrap remote process to mimic AsyncProcess interface if needed
            class RemoteAsyncProcess:
                def __init__(self, process):
                    self.process = process
                    self.reader = None
                    self.timer = None
                    self.output = ""

            ap = RemoteAsyncProcess(process)
            ret.append(ap)
    return ret


def wait(ap: AsyncProcess) -> str:
    """Wait for async process to complete and ensure proper cleanup.
    
    Args:
        ap: AsyncProcess to wait for
        
    Returns:
        Process output as string
    """
    try:  # in case of user interrupt
        ap.process.wait()
        if ap.timer:
            ap.timer.cancel()
    except KeyboardInterrupt:
        logger.warning("User interrupt detected, cleaning up process")
        killproc(ap.process)
        raise
    except Exception as e:
        logger.error(f"Exception while waiting for process: {e}")
        killproc(ap.process)
        raise
    finally:
        if ap.timer:
            ap.timer.cancel()
            ap.timer.join(30)
        if ap.reader:
            ap.output = ap.reader.join(30)
        
        # Ensure process is really terminated
        if ap.process.poll() is None:
            logger.warning(f"Process {ap.process.pid} still running after wait, forcing termination")
            killproc(ap.process, sigint=True)
        
        logger.debug(
            f"Process {ap.process.pid} finished with RC: {ap.process.returncode}"
        )
        
        # Give kernel a moment to release VFIO resources
        time.sleep(0.5)
    
    return ap.output


def waitall(aps=List[AsyncProcess]):
    for ap in aps:
        wait(ap)
    return


def is_process_running(process):
    logger.debug(f"Checking if process is running: {process}")
    if hasattr(process, "running") and callable(getattr(process, "running")):
        try:
            result = process.running()
            logger.debug(f"process.running(): {result}")
            return result
        except Exception as e:
            logger.debug(f"Exception calling process.running(): {e}")
            return False
    return None


def run(
    command: str,
    cwd: str = None,
    testcmd: bool = False,
    timeout: int = 60,
    host=None,
    env: dict = None,
    background: bool = False,
    enable_sudo: bool = False,
) -> any:
    if testcmd:
        logger.log(level=TESTCMD_LVL, msg=f"Test command: {command}")
    else:
        logger.debug(f"Run command: {command}")

    # if host is None:
    #     # Local execution
    #     try:
    #         cp = subprocess.run(
    #             "exec " + command,
    #             stdout=subprocess.PIPE,
    #             stderr=subprocess.STDOUT,
    #             timeout=timeout,
    #             shell=True,
    #             text=True,
    #             cwd=cwd,
    #             env=env,
    #         )
    #     except subprocess.TimeoutExpired:
    #         logger.debug("Timeout expired")
    #         raise

    #     for line in cp.stdout.splitlines():
    #         logger.debug(line.rstrip())
    #     logger.debug(f"RC: {cp.returncode}")
    #     return cp
    # else:
    # Remote execution
    # if enable_sudo:
    #     host.connection.enable_sudo()

    process = host.connection.start_process(
        command,
        shell=True,
        stderr_to_stdout=True,
        cwd=cwd,
        env=env,
    )
    if not background:
        process.wait(timeout=timeout)
        logger.debug(f"RC: {process.return_code}")
    else:
        logger.debug("Process started in background mode.")
    return process


def run_in_background(
    command: str, cwd: str, env: dict, result_queue: Queue, timeout: int = 60
) -> None:
    logger.debug(command)

    args = ["exec"]
    args.extend(command.split())

    proc = subprocess.Popen(
        args=args,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        shell=True,
        cwd=cwd,
        env=env,
        text=True,
    )

    time.sleep(timeout)
    proc.send_signal(2)
    stdout, _ = proc.communicate(timeout=5)

    logger.info(f"rc: {proc.returncode}")

    result_queue.put(stdout)


def log_fail(msg: str):
    logger.log(level=TEST_FAIL, msg=msg)

    with check:
        pytest.fail(msg)


def log_info(msg: str):
    add_result_log(msg)
    logger.info(msg)


def log_result_note(note: str):
    set_result_note(note)
    logger.info(f"Test result note: {note}")
