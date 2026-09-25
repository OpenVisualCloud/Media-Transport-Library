# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

"""A MtlManager of the test case, with its log in the log folder of the case.

The session MtlManager writes no log. The ``manager_log`` fixture stops it,
starts a manager that writes to ``<case>_mtl_manager.log``, and starts the
session manager again after the case. The case reads the log with
``manager_log.check()``.
"""

import logging
import os
import re
import shlex
import time
from dataclasses import dataclass, field

import pytest
from common.mtl_manager.mtlManager import MtlManager
from conftest import get_host_mtl_path
from mtl_engine.const import LOG_FOLDER

logger = logging.getLogger(__name__)

MANAGER_READY = "MTL Manager is running on"
MANAGER_EXITED = "MTL Manager exited."
LINE_RE = re.compile(r"^\[[^\]]+\] \[(?P<level>[A-Z]+)\] (?P<message>.*)$")
INSTANCE_RE = re.compile(
    r"^\[Instance (?P<host>[^:\]]*):(?P<pid>-?\d+)\] (?P<text>.*)$"
)
LCORE_RE = re.compile(r"^(?P<action>Added|Removed) lcore (?P<lcore>\d+)$")
START_TIMEOUT_S = 10
STOP_TIMEOUT_S = 10
SETTLE_TIMEOUT_S = 10


@dataclass
class ManagerInstance:
    """What the log of the manager says about one libmtl instance."""

    pid: int
    registered: bool = False
    removed: bool = False
    added_lcores: list = field(default_factory=list)
    removed_lcores: list = field(default_factory=list)


class ManagerLog:
    """Read and check the log of the MtlManager of one test case."""

    def __init__(self, host, path: str):
        self.host = host
        self.path = path

    def read(self) -> list[str]:
        result = self.host.connection.execute_command(
            f"cat {shlex.quote(self.path)}", expected_return_codes=None
        )
        return result.stdout.splitlines() if result.return_code == 0 else []

    def contains(self, text: str) -> bool:
        return any(text in line for line in self.read())

    def instances(self, lines: list[str]) -> dict[int, ManagerInstance]:
        """Group the instance lines of the log by the pid of the instance.

        A line before the register carries pid -1, so it goes to no instance.
        """
        instances = {}
        for line in lines:
            match = LINE_RE.match(line)
            if not match:
                continue
            inst = INSTANCE_RE.match(match["message"])
            if not inst or int(inst["pid"]) < 0:
                continue
            pid = int(inst["pid"])
            entry = instances.setdefault(pid, ManagerInstance(pid=pid))
            text = inst["text"]
            lcore = LCORE_RE.match(text)
            if text.startswith("Registered,"):
                entry.registered = True
            elif text == "Remove client.":
                entry.removed = True
            elif lcore and lcore["action"] == "Added":
                entry.added_lcores.append(int(lcore["lcore"]))
            elif lcore:
                entry.removed_lcores.append(int(lcore["lcore"]))
        return instances

    def _settled(self, lines: list[str], min_instances: int) -> bool:
        instances = self.instances(lines).values()
        return len(instances) >= min_instances and all(i.removed for i in instances)

    def check(self, min_instances: int) -> dict[int, ManagerInstance]:
        """Fail the case unless the manager served every instance cleanly.

        Every instance must register, take at least one lcore, give back each
        lcore it took, and disconnect. The log must hold no ERROR line.
        """
        deadline = time.monotonic() + SETTLE_TIMEOUT_S
        lines = self.read()
        while not self._settled(lines, min_instances) and time.monotonic() < deadline:
            time.sleep(0.5)
            lines = self.read()

        logger.info(f"MtlManager log {self.path}:\n" + "\n".join(lines))

        errors = [line for line in lines if "] [ERROR] " in line]
        assert not errors, "MtlManager logged errors:\n" + "\n".join(errors)

        instances = self.instances(lines)
        assert len(instances) >= min_instances, (
            f"MtlManager served {len(instances)} instance(s), "
            f"expected at least {min_instances}"
        )
        for inst in instances.values():
            assert inst.registered, f"instance {inst.pid} did not register"
            assert inst.added_lcores, f"instance {inst.pid} took no lcore"
            assert sorted(inst.added_lcores) == sorted(inst.removed_lcores), (
                f"instance {inst.pid} took lcores {sorted(inst.added_lcores)} "
                f"and gave back {sorted(inst.removed_lcores)}"
            )
            assert inst.removed, f"instance {inst.pid} did not disconnect"
        return instances


def _wait_no_manager(host, timeout_s: float) -> bool:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        result = host.connection.execute_command(
            "pgrep -x MtlManager", expected_return_codes=None
        )
        if result.return_code != 0:
            return True
        time.sleep(0.2)
    return False


def _stop_all_managers(host) -> None:
    """Stop every MtlManager of the host, with SIGTERM first.

    SIGTERM lets the manager write its last lines. SIGKILL is the fallback.
    """
    host.connection.execute_command(
        "sudo pkill -TERM -x MtlManager", expected_return_codes=None
    )
    if not _wait_no_manager(host, STOP_TIMEOUT_S):
        logger.warning("MtlManager did not stop on SIGTERM, sending SIGKILL")
        host.connection.execute_command(
            "sudo pkill -KILL -x MtlManager", expected_return_codes=None
        )
        _wait_no_manager(host, STOP_TIMEOUT_S)


@pytest.fixture(scope="function")
def manager_log(hosts, mtl_manager, request):
    host = list(hosts.values())[0]
    log_folder = os.environ.get("MTL_LOG_FOLDER", LOG_FOLDER)
    path = os.path.abspath(
        os.path.join(log_folder, "latest", f"{request.node.nodeid}_mtl_manager.log")
    )
    os.makedirs(os.path.dirname(path), exist_ok=True)

    session_manager = mtl_manager[host.name]
    session_manager.stop()
    _stop_all_managers(host)

    manager = MtlManager(host, mtl_path=get_host_mtl_path(host), log_file=path)
    log = ManagerLog(host, path)
    if not manager.start():
        pytest.fail(f"Failed to start MtlManager on host {host.name}")
    deadline = time.monotonic() + START_TIMEOUT_S
    while not log.contains(MANAGER_READY):
        if time.monotonic() > deadline:
            pytest.fail(f"MtlManager did not report '{MANAGER_READY}' in {path}")
        time.sleep(0.2)

    yield log

    _stop_all_managers(host)
    if not log.contains(MANAGER_EXITED):
        logger.warning(f"MtlManager did not report '{MANAGER_EXITED}' in {path}")
    session_manager.mtl_manager_process = None
    if not session_manager.start():
        logger.error(f"Failed to start the session MtlManager on host {host.name}")
