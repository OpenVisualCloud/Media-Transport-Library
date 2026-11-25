# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

"""The stability run as one pytest case, for the stability workflow.

The engine (``tests/acceptance/stability``) does the work; this case gives it
a host config, runs it as root, and fails with each reason of its summary.

* ``STABILITY_CONFIG``: a host config (default ``/etc/mtl-ci/stability.yaml``).
  Without one, the case makes 6 VFs on the test NIC (3 TX, 3 RX), the CPU sets
  from the NUMA node of the NIC, and takes the capture NIC and EBU LIST of
  test_config.yaml.
* ``STABILITY_PROFILE``: ``full`` (default) or ``smoke``.
* ``STABILITY_DURATION``: ``90s``, ``15m``, ``8h`` (default ``1h``).
"""

import dataclasses
import json
import logging
import os
import subprocess
import sys
import time

import pytest
import yaml
from common.nicctl import InterfaceSetup

from stability import config as stab_config

logger = logging.getLogger(__name__)

ACCEPTANCE = os.path.dirname(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
)
DEFAULT_CONFIG = "/etc/mtl-ci/stability.yaml"
RUNNER_ENV = "/etc/mtl-ci/runner.env"


def _auto_config(
    setup_interfaces: InterfaceSetup, test_config: dict, mtl_path: str, path: str
) -> str:
    vfs = setup_interfaces.get_interfaces_list_single("VF", count=6)
    capture = (test_config.get("capture_cfg") or {}).get("sniff_interface", "")
    host = stab_config.auto_host(
        mtl_path,
        vfs[:3],
        vfs[3:],
        capture_interface=capture,
        ebu_password_file=RUNNER_ENV if os.path.isfile(RUNNER_ENV) else "",
    )
    data = dataclasses.asdict(host)
    data["ports"] = {"tx": data.pop("tx_ports"), "rx": data.pop("rx_ports")}
    with open(path, "w", encoding="utf-8") as f:
        yaml.safe_dump(data, f, sort_keys=False)
    return path


@pytest.mark.stability
def test_stability(setup_interfaces: InterfaceSetup, test_config, mtl_path, tmp_path):
    cfg = os.environ.get("STABILITY_CONFIG", DEFAULT_CONFIG)
    if not os.path.isfile(cfg):
        cfg = _auto_config(
            setup_interfaces, test_config, mtl_path, str(tmp_path / "stability.yaml")
        )
    profile = os.environ.get("STABILITY_PROFILE", "full")
    duration = os.environ.get("STABILITY_DURATION", "1h")
    out = os.path.join(
        ACCEPTANCE, "logs", "stability", time.strftime("%Y%m%d-%H%M%S") + f"-{profile}"
    )
    cmd = [
        "sudo", "-n", "-E", sys.executable, "-m", "stability", "run",
        "-c", cfg, "-p", profile, "-d", duration, "-o", out,
    ]  # fmt: skip
    logger.info("stability: %s", " ".join(cmd))
    r = subprocess.run(cmd, cwd=ACCEPTANCE, check=False)
    summary_path = os.path.join(out, "summary.json")
    assert os.path.isfile(
        summary_path
    ), f"the engine wrote no summary (exit status {r.returncode})"
    with open(summary_path, encoding="utf-8") as f:
        summary = json.load(f)
    logger.info("stability report: %s", os.path.join(out, "report.html"))
    assert summary["passed"], "stability run failed:\n" + "\n".join(summary["reasons"])
