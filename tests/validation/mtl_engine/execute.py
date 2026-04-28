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
from mfd_connect.exceptions import (
    ConnectionCalledProcessError,
    RemoteProcessInvalidState,
)
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


def log_warn(msg: str):
    add_result_log(msg)
    logger.warning(msg)


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

    Uses a graceful signal ladder (SIGINT → SIGTERM → SIGKILL) so DPDK
    applications get a chance to run ``rte_eal_cleanup()`` and release
    their VFIO group fd. Going straight to SIGKILL leaves the kernel-side
    VFIO refcount non-zero and causes the next ``nicctl disable_vf`` call
    to block forever in ``vfio_unregister_group_dev``.

    Args:
        *hosts: One or more host objects with ``connection.execute_command``.
        names:  Process names to kill.  Defaults to :data:`MTL_APP_NAMES`.
                Each name is turned into a ``pkill`` regex that avoids
                matching the grep/pkill process itself (``[R]xTxApp``).
    """
    targets = names or MTL_APP_NAMES
    pattern = "|".join(f"[{n[0]}]{n[1:]}" for n in targets if n)
    # SIGINT -> 3s; SIGTERM -> 2s; final SIGKILL.
    cmd = (
        f"sudo pkill -INT -f '{pattern}' 2>/dev/null; "
        "for i in 1 2 3 4 5 6; do "
        f"  pgrep -f '{pattern}' >/dev/null || break; sleep 0.5; "
        "done; "
        f"sudo pkill -TERM -f '{pattern}' 2>/dev/null; "
        "for i in 1 2 3 4; do "
        f"  pgrep -f '{pattern}' >/dev/null || break; sleep 0.5; "
        "done; "
        f"sudo pkill -KILL -f '{pattern}' 2>/dev/null; "
        "true"
    )
    for host in hosts:
        try:
            host.connection.execute_command(cmd, shell=True, timeout=20)
        except Exception:
            pass


def read_remote_log(host, log_path: str) -> list:
    """Read a log file from a remote host and return its lines."""
    try:
        result = host.connection.execute_command(f"cat {log_path}", shell=True)
        return result.stdout.splitlines() if result.stdout else []
    except Exception:
        return []


# --- SSH-backed remote process termination -----------------------------------
#
# Helpers for tearing down processes started by :func:`run` (background=True).
# Teardown contract:
#   * Idempotent  — calling on a process that already exited is a no-op.
#   * Bounded     — must not hang the test if the SSH transport is unresponsive.
#   * Non-raising — a teardown failure must never mask the real test verdict.
#
# Do NOT discover the PID with ``pgrep -n <name>``: it returns the most recently
# started matching process system-wide, so on a shared CI runner it can pick up
# unrelated leftovers and SIGKILL them. Always use the PID the connection
# abstraction already owns for *our* process.
#
# ``mfd_connect.process.ssh.base.SSHRemoteProcess.pid`` is a property that
# issues a remote ``ps`` and raises :class:`RemoteProcessInvalidState` once the
# process has exited; that is the *normal* termination signal during teardown.
# ``proc.running`` swallows the same exception and returns ``False``, so it's
# safe to call without a guard.

_GRACEFUL_STOP_WAIT_S = 2
_FORCE_KILL_WAIT_S = 1


def _safe_pid(proc) -> int | None:
    """Return the remote PID of *proc*, or ``None`` if it's already finished."""
    if proc is None:
        return None
    try:
        return proc.pid
    except RemoteProcessInvalidState:
        return None
    except Exception as exc:  # noqa: BLE001 - non-fatal, never mask test result
        logger.warning("Unexpected error reading PID: %s", exc)
        return None


def _terminate(proc, proc_name: str, host) -> None:
    """Single graceful → forceful termination pass. Caller bounds the wallclock.

    Order:
      1. ``proc.stop(wait=2)`` — SIGTERM via the connection's own primitive,
         honoring its bounded wait.
      2. ``proc.kill(with_signal="SIGKILL")`` — SIGKILL via the connection.
         Note: the abstraction's default signal is SIGTERM despite the method
         name, hence the explicit ``with_signal``.
      3. Belt-and-braces remote ``kill -9 <pid>`` — only if the abstraction
         ran out of room to retry and only against the PID we actually own.
         Never ``pgrep`` for it (that races with other processes on the host).
    """
    if proc is None:
        return

    pid = _safe_pid(proc)

    try:
        proc.stop(wait=_GRACEFUL_STOP_WAIT_S)
    except RemoteProcessInvalidState:
        return  # exited mid-stop
    except Exception as exc:  # noqa: BLE001
        logger.debug("%s: graceful stop raised (%s); escalating", proc_name, exc)

    if not proc.running:
        return

    try:
        proc.kill(wait=_FORCE_KILL_WAIT_S, with_signal="SIGKILL")
    except RemoteProcessInvalidState:
        return
    except Exception as exc:  # noqa: BLE001
        logger.debug(
            "%s: kill() raised (%s); falling back to remote shell", proc_name, exc
        )

    if not proc.running or pid is None or host is None:
        return

    # Last resort: shell-level kill of the PID the abstraction handed us.
    try:
        host.connection.execute_command(
            f"kill -9 {pid} 2>/dev/null || true", shell=True
        )
    except ConnectionCalledProcessError as exc:
        logger.warning(
            "%s: remote `kill -9 %d` returned non-zero: %s", proc_name, pid, exc
        )
    except Exception as exc:  # noqa: BLE001
        logger.warning("%s: remote `kill -9 %d` raised: %s", proc_name, pid, exc)


def stop_process(proc, proc_name: str = "process", timeout: int = 5, host=None) -> None:
    """Best-effort termination of an mfd_connect background process.

    Idempotent and never raises. The bounded waits inside
    ``proc.stop`` / ``proc.kill`` are honored by the abstraction itself; the
    daemon thread here only guards against the rare case where the underlying
    SSH transport hangs and ignores those bounds. On timeout we log and move
    on — abandoning the daemon thread is acceptable in the short-lived test
    runner context, and the surrounding orphan sweep will reap any survivors.
    """
    if proc is None:
        logger.debug("%s: no process to stop", proc_name)
        return

    if not proc.running:
        logger.debug("%s: already finished, skipping stop", proc_name)
        return

    logger.debug("%s: stopping (PID %s)", proc_name, _safe_pid(proc))

    worker = threading.Thread(
        target=_terminate, args=(proc, proc_name, host), daemon=True
    )
    worker.start()
    worker.join(timeout=timeout)

    if worker.is_alive():
        logger.error(
            "%s: termination thread still alive after %ds; abandoning "
            "(orphan sweep will reap any survivors)",
            proc_name,
            timeout,
        )
