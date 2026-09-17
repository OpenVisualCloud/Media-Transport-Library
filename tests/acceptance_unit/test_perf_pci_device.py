# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

"""The perf rig's PCI_DEVICE step has to declare every port of the DUT card.

perf-pytest.yml generates its topology from what `pytest-setup.sh pci-env`
exports, and the acceptance framework resolves interface_index within a
vendor:device group -- so a single entry means a single interface. The lab file
on the perf hosts names one PF of a two-port E830, which left conftest.py no
port to build host.vfs_r on, and every ST2022-7 case skipped with "Redundant
requires VFs on TX port 1" inside a perf run that reported success.
"""

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / ".github/scripts/ci/pytest-setup.sh"

# Stand-in for lspci: FAKE_DEV is the card, FAKE_PORTS how many ports it has,
# FAKE_BDF_KNOWN whether the host admits to the BDF being asked about.
FAKE_LSPCI = """#!/usr/bin/env bash
args="$*"
case "${args}" in
*"-s "*)
    # `lspci -Dn -s <bdf>`: one line, whose third field is the vendor:device.
    [[ ${FAKE_BDF_KNOWN} == 1 ]] || exit 0
    printf '%s 0200: %s\\n' "${args##*-s }" "$FAKE_DEV"
    ;;
*"-d "*)
    # `lspci -Dn -d <vendor:device>`: one line per port of that card. A filter
    # with no colon is a syntax error to real lspci, which exits 1 -- not an
    # empty match -- so the stub must not answer one with a clean zero.
    filter=${args##*-d }
    [[ ${filter} == *:* ]] || exit 1
    [[ ${filter} == "$FAKE_DEV" ]] || exit 0
    for ((i = 0; i < FAKE_PORTS; i++)); do
        printf '0000:15:00.%d 0200: %s\\n' "$i" "$FAKE_DEV"
    done
    ;;
esac
"""


class PerfCardPortsTests(unittest.TestCase):
    stderr = ""

    def pci_env(self, declared, ports, bdf_known=True):
        """Run `pytest-setup.sh pci-env` against a fake card.

        Returns (exit code, the PCI_DEVICE values written to GITHUB_ENV), and
        leaves the step's stderr in self.stderr for the diagnostic it prints.
        """
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            (tmp / "bin").mkdir()
            lspci = tmp / "bin" / "lspci"
            lspci.write_text(FAKE_LSPCI)
            lspci.chmod(0o755)

            runner_env = tmp / "runner.env"
            if declared is not None:
                runner_env.write_text(f"PERF_PCI_DEVICE='{declared}'\n")
            github_env = tmp / "github.env"
            github_env.touch()

            env = {
                **os.environ,
                "PATH": f"{tmp / 'bin'}{os.pathsep}{os.environ['PATH']}",
                "MTL_CI_RUNNER_ENV": str(runner_env),
                "GITHUB_ENV": str(github_env),
                "FAKE_DEV": "8086:12d2",
                "FAKE_PORTS": str(ports),
                "FAKE_BDF_KNOWN": "1" if bdf_known else "0",
            }
            # PCI_DEVICE from the environment outranks the lab file, so a stray
            # one here would shadow the fixture and every case would agree.
            env.pop("PCI_DEVICE", None)
            env.pop("PERF_PCI_DEVICE", None)

            proc = subprocess.run(
                ["bash", str(SCRIPT), "pci-env"],
                env=env,
                capture_output=True,
                text=True,
            )
            exported = [
                line.split("=", 1)[1]
                for line in github_env.read_text().splitlines()
                if line.startswith("PCI_DEVICE=")
            ]
        self.stderr = proc.stderr
        return proc.returncode, exported

    def test_one_pf_of_a_two_port_card_declares_both(self):
        # The perf fleet's actual lab file. Without the second entry the
        # topology has one interface and every redundant case skips.
        self.assertEqual(
            self.pci_env("0000:15:00.0", ports=2),
            (0, ["8086:12d2,8086:12d2"]),
        )

    def test_single_port_card_declares_one(self):
        # No second port to put the redundant leg on, so the redundant cases
        # must keep skipping rather than fail against a port that is not there.
        self.assertEqual(
            self.pci_env("0000:15:00.0", ports=1),
            (0, ["8086:12d2"]),
        )

    def test_explicit_pair_is_taken_as_given(self):
        # A list is the operator's own answer, so it is not re-expanded: four
        # ports on the card still yield the two entries the lab file asked for.
        self.assertEqual(
            self.pci_env("8086:12d2,8086:12d2", ports=4),
            (0, ["8086:12d2,8086:12d2"]),
        )

    def test_vendor_device_form_expands_too(self):
        self.assertEqual(
            self.pci_env("8086:12d2", ports=2),
            (0, ["8086:12d2,8086:12d2"]),
        )

    def test_host_without_a_lab_file_uses_the_default_card(self):
        self.assertEqual(self.pci_env(None, ports=2), (0, ["8086:12d2,8086:12d2"]))

    def test_card_absent_from_the_host_fails_the_step(self):
        # Exporting an empty PCI_DEVICE would defer the failure to the first
        # test that tries to bind a VF, hundreds of log lines later.
        self.assertEqual(
            self.pci_env("0000:15:00.0", ports=2, bdf_known=False),
            (1, []),
        )
        # The exit code alone would send the operator to the job's first test.
        # Naming the card that was asked for is the whole point of the branch.
        self.assertIn("perf card (0000:15:00.0)", self.stderr)


if __name__ == "__main__":
    unittest.main()
