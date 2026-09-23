# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

import os
import subprocess
from types import SimpleNamespace

import pytest
from mtl_engine.execute import run

HOST_REGISTRY = "/home/gta/.kahawai.json"


class _SshConnection:
    """Runs a command as mfd-connect does over SSH. sshd drops ``env=``
    (AcceptEnv LANG LC_*), so the app sees only the host's own environment."""

    def __init__(self, host_env: dict):
        self.host_env = host_env

    def start_process(self, command, shell, stderr_to_stdout, cwd, env):
        done = subprocess.run(
            ["bash", "-c", f"cd {cwd}; {command} && true marker"],
            env={"PATH": os.environ["PATH"], **self.host_env},
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            check=False,
        )
        return SimpleNamespace(return_code=done.returncode, stdout_text=done.stdout)


def _run_app(tmp_path, host_env: dict):
    host = SimpleNamespace(connection=_SshConnection(host_env))
    return run(
        "printenv KAHAWAI_CFG_PATH; (exit 7)",
        cwd=str(tmp_path),
        host=host,
        background=True,
    )


def test_run_carries_job_registry_across_ssh(tmp_path, monkeypatch):
    registry = tmp_path / "job temp" / "kahawai_ci.json"
    registry.parent.mkdir()
    registry.write_text("{}")
    monkeypatch.setenv("KAHAWAI_CFG_PATH", str(registry))

    process = _run_app(tmp_path, host_env={})

    assert process.stdout_text == f"{registry}\n"
    assert process.return_code == 7


@pytest.mark.parametrize(
    "job_registry", ["/nonexistent/kahawai_ci.json", None], ids=["missing", "unset"]
)
def test_run_keeps_host_registry_without_job_file(tmp_path, monkeypatch, job_registry):
    if job_registry:
        monkeypatch.setenv("KAHAWAI_CFG_PATH", job_registry)
    else:
        monkeypatch.delenv("KAHAWAI_CFG_PATH", raising=False)

    process = _run_app(tmp_path, host_env={"KAHAWAI_CFG_PATH": HOST_REGISTRY})

    assert process.stdout_text == f"{HOST_REGISTRY}\n"
    assert process.return_code == 7
