# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation
"""MtlManager runtime files: their modes, and no socket left after a stop.

MtlManager creates ``/var/run/imtl`` and binds its socket there. Clients of any
user need search on the directory and write on the socket to ``connect(2)``, and
the manager sets them ``0755`` and ``0666`` whatever the umask. A directory
already there loses write for others, who could otherwise replace the socket,
and keeps the rest of its mode.
SIGINT and SIGTERM must end the manager cleanly and remove the socket.

Each case runs its own manager. The session manager is stopped meanwhile,
because a manager unlinks the socket of any manager already running.
"""

import logging
import os
import signal
import threading

import pytest
from conftest import get_host_mtl_path
from mtl_engine.const import MTL_MANAGER_EXE

logger = logging.getLogger(__name__)

pytestmark = [pytest.mark.verified, pytest.mark.smoke]

RUN_DIR = "/var/run/imtl"
SOCKET = f"{RUN_DIR}/mtl_manager.sock"  # MTL_MANAGER_SOCK_PATH
READY = "MTL Manager is running"  # logged after the chmod and listen()
TIMEOUT = 10  # seconds; starting and stopping each take well under one


def stop_manager(dut, process, sig):
    """Send ``sig`` to the manager and return its exit status.

    The SSH user may not own the root sudo, so the kill runs under sudo too.
    sudo relays the signal to the manager.
    """
    dut.connection.execute_command(f"sudo kill -{int(sig)} {process.pid}", shell=True)
    return process.wait(timeout=TIMEOUT)


def socket_exists(dut):
    result = dut.connection.execute_command(
        f"sudo test -e {SOCKET}", shell=True, expected_return_codes=None
    )
    return result.return_code == 0


@pytest.fixture(scope="module")
def dut(hosts, mtl_manager):
    """The DUT, with the session MtlManager stopped until the module ends."""
    host = list(hosts.values())[0]
    mtl_manager[host.name].stop()
    try:
        # stop() may only signal it, and it unlinks the socket as it exits.
        waited = host.connection.execute_command(
            f"timeout {TIMEOUT} sh -c 'while pgrep -x MtlManager; do sleep 0.1; done'",
            shell=True,
            expected_return_codes=None,
        )
        assert waited.return_code == 0, (
            f"an MtlManager is still running {TIMEOUT} s after the session one was "
            "stopped: another MtlManager runs on the host, or SIGTERM did not stop it"
        )
        yield host
    finally:
        assert mtl_manager[host.name].start(), "cannot restart the session MtlManager"


@pytest.fixture
def manager(request, dut, mtl_path):
    """An MtlManager under umask ``umask``. ``dir_mode`` is the mode of a runtime
    dir made before it starts, or None to let the manager create it."""
    exe = os.path.join(get_host_mtl_path(dut, default=mtl_path), MTL_MANAGER_EXE)
    umask, dir_mode = getattr(request, "param", ("022", None))
    dut.connection.execute_command(f"sudo rm -rf {RUN_DIR}", shell=True)
    if dir_mode:
        dut.connection.execute_command(
            f"sudo mkdir -m {dir_mode} {RUN_DIR}", shell=True
        )
    # sudo applies its own umask, so set it inside. exec, so that the signal
    # sudo relays reaches the manager and not the shell.
    process = dut.connection.start_process(
        f"sudo sh -c 'umask {umask} && exec {exe}'", stderr_to_stdout=True
    )
    ready = threading.Event()

    def log_output():
        for line in process.get_stdout_iter():
            logger.debug(f"MtlManager: {line.rstrip()}")
            if READY in line:
                ready.set()

    threading.Thread(target=log_output, daemon=True).start()
    try:
        assert ready.wait(TIMEOUT), f"no '{READY}' from MtlManager in {TIMEOUT} s"
        yield process
        if process.running:
            stop_manager(dut, process, signal.SIGINT)
    finally:
        # Whatever failed, leave no manager behind. The session one is stopped,
        # so any MtlManager is this one.
        dut.connection.execute_command(
            "sudo pkill -KILL -x MtlManager", shell=True, expected_return_codes=None
        )


@pytest.mark.parametrize(
    "manager",
    [("000", None), ("077", None)],
    indirect=True,
    ids=["umask000", "umask077"],
)
def test_manager_runtime_files_modes(dut, manager):
    """The dir is 0755 and the socket 0666, and another user can connect.

    umask 000 leaves the manager nothing to rely on the umask to remove, and
    077 removes every bit the clients need.
    """
    modes = dut.connection.execute_command(
        f"sudo stat -c '%a %F' {RUN_DIR} {SOCKET}",
        shell=True,
        expected_return_codes=None,
    )
    assert modes.stdout.splitlines() == ["755 directory", "666 socket"], modes.stderr

    # setpriv, as sudo -u would need a sudoers rule for another run-as user.
    connect = dut.connection.execute_command(
        "sudo setpriv --reuid=65534 --regid=65534 --clear-groups python3 -c "
        f"\"import socket; socket.socket(socket.AF_UNIX).connect('{SOCKET}')\"",
        shell=True,
        stderr_to_stdout=True,
        expected_return_codes=None,
    )
    assert connect.return_code == 0, f"uid 65534 cannot connect:\n{connect.stdout}"


@pytest.mark.parametrize("manager", [("022", "0777")], indirect=True, ids=["dir0777"])
def test_existing_dir_others_cannot_remove_socket(dut, manager):
    """A dir every user can write loses write for others and keeps the rest of
    its mode, so no other user can replace the socket."""
    mode = dut.connection.execute_command(
        f"sudo stat -c %a {RUN_DIR}", shell=True, expected_return_codes=None
    )
    assert mode.stdout.strip() == "775", mode.stderr
    assert socket_exists(dut), f"no {SOCKET} to remove"
    removed = dut.connection.execute_command(
        "sudo setpriv --reuid=65534 --regid=65534 --clear-groups "
        f"env LC_ALL=C rm -f {SOCKET}",
        shell=True,
        stderr_to_stdout=True,
        expected_return_codes=None,
    )
    assert socket_exists(dut), f"uid 65534 removed {SOCKET}:\n{removed.stdout}"
    # rm must have run and been refused, not failed to start
    assert "Permission denied" in removed.stdout, removed.stdout


@pytest.mark.parametrize(
    "sig", [signal.SIGINT, signal.SIGTERM], ids=["SIGINT", "SIGTERM"]
)
def test_manager_removes_socket_on_exit(dut, manager, sig):
    """SIGINT (Ctrl+C) and SIGTERM (kill, systemd, docker stop) end the
    manager cleanly, and it removes its socket."""
    assert socket_exists(dut), f"no {SOCKET} to remove"
    status = stop_manager(dut, manager, sig)
    assert status == 0, f"MtlManager exit status {status}"
    assert not socket_exists(dut), f"{SOCKET} left behind"
