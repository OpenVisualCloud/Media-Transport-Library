# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

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
    result = proc.poll()
    if result is not None:
        return result

    # try to 'gently' terminate proc
    if sigint:
        proc.send_signal(2)  # SIGINT
    else:
        proc.terminate()

    time.sleep(5)

    for _ in range(5):
        result = proc.poll()
        if result is not None:
            return result
        time.sleep(5)  # wait a little longer for proc to terminate

    # failed to terminate proc, so kill it
    proc.kill()
    for _ in range(10):
        result = proc.poll()
        if result is not None:
            return result
        time.sleep(5)  # give system more time to kill proc

    # failed to kill proc
    if result is None:
        logger.error(f"Failed to kill process with pid {proc.pid}")


def readproc(process: subprocess.Popen):
    case_id = os.environ["PYTEST_CURRENT_TEST"]
    case_id = case_id[: case_id.rfind("(") - 1]
    log_folder = os.environ.get("MTL_LOG_FOLDER", LOG_FOLDER)
    logfile = os.path.join(log_folder, "latest", f"{case_id}.pid{process.pid}.log")

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
    try:  # in case of user interrupt
        ap.process.wait()
        ap.timer.cancel()
    except:  # noqa E722
        killproc(ap.process)
        raise
    finally:
        ap.timer.cancel()
        ap.timer.join(30)
        ap.output = ap.reader.join(30)
        logger.debug(
            f"Process {ap.process.pid} finished with RC: {ap.process.returncode}"
        )
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
) -> any:
    if testcmd:
        logger.log(level=TESTCMD_LVL, msg=f"Test command: {command}")
    else:
        logger.debug(f"Run command: {command}")

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


# Canonical list of MTL-related process names that may be left over
# after a crash or timeout.  Mirrors .github/actions/cleanup/action.yml.
MTL_APP_NAMES = [
    "RxTxApp",
    "KahawaiTest",
    "KahawaiUfdTest",
    "KahawaiUplTest",
    "ffmpeg",
    "gtest.sh",
]


def kill_stale_processes(*hosts, names: list[str] | None = None) -> None:
    """Kill leftover MTL-related processes on the given hosts.

    Args:
        *hosts: One or more host objects with ``connection.execute_command``.
        names:  Process names to kill.  Defaults to :data:`MTL_APP_NAMES`.
                Each name is turned into a ``pkill`` regex that avoids
                matching the grep/pkill process itself (``[R]xTxApp``).
    """
    targets = names or MTL_APP_NAMES
    pattern = "|".join(f"[{n[0]}]{n[1:]}" for n in targets if n)
    for host in hosts:
        try:
            host.connection.execute_command(
                f"pkill -9 -f '{pattern}' || true", shell=True, timeout=15
            )
        except Exception:
            pass


def read_remote_log(host, log_path: str) -> list:
    """Read a log file from a remote host and return its lines."""
    try:
        result = host.connection.execute_command(f"cat {log_path}", shell=True)
        return result.stdout.splitlines() if result.stdout else []
    except Exception:
        return []
