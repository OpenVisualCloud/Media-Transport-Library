# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

"""Fake host for the mtl_engine adapter tests.

The fake connection runs each command on the local machine, inside a pytest
tmp_path, so the real stat / rm / video_integrity.py code paths run. No NIC,
no sudo, no MTL process.
"""

import os
import pathlib
import re
import subprocess
import sys

import pytest

REPO = pathlib.Path(__file__).resolve().parents[4]
ACCEPTANCE = REPO / "tests" / "acceptance"
sys.path.insert(0, str(ACCEPTANCE))

from mtl_engine import ip_pools  # noqa: E402

if not ip_pools.rx:
    ip_pools.init(session_id=50)


class _Result:
    def __init__(self, rc, out):
        self.return_code = rc
        self.stdout = out
        self.stdout_text = out

    def wait(self, timeout=None):
        return self.return_code


class LocalConnection:
    """Records every command; runs it locally with a shell."""

    def __init__(self, deny=()):
        self.commands = []
        self._deny = deny

    def path(self, *parts):
        return pathlib.Path(*[str(p) for p in parts])

    def _run(self, cmd, cwd=None):
        self.commands.append(cmd)
        for pat in self._deny:
            if re.search(pat, cmd):
                raise AssertionError(f"forbidden command in a unit test: {cmd}")
        p = subprocess.run(
            cmd,
            shell=True,
            cwd=cwd,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=120,
            env={**os.environ, "PYTHONDONTWRITEBYTECODE": "1"},
        )
        return _Result(p.returncode, p.stdout)

    def execute_command(self, cmd, shell=True, expected_return_codes=(0,), **kw):
        return self._run(cmd, cwd=kw.get("cwd"))

    def start_process(self, cmd, shell=True, stderr_to_stdout=True, cwd=None, env=None):
        return self._run(cmd, cwd=cwd)


class FakeHost:
    def __init__(self):
        self.name = "fakehost"
        # sudo, mount, dpdk, ffmpeg, RxTxApp must never run here.
        self.connection = LocalConnection(
            deny=(r"\bsudo\b", r"\bmount\b", r"dpdk", r"ffmpeg ", r"RxTxApp")
        )


@pytest.fixture
def host():
    return FakeHost()


@pytest.fixture
def repo():
    return str(REPO)
