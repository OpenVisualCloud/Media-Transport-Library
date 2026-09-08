# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

STUB = r"""#!/usr/bin/env bash
set -euo pipefail
name=$(basename "$0")
echo "$name $*" >> "$FIXTURE/commands"
case "$name" in
id) echo 0 ;;
uname) echo fixture ;;
modinfo)
    case "$2" in
    srcversion) echo cached ;;
    vermagic) echo fixture ;;
    esac
    ;;
nm) echo '0 T ice_vc_cfg_q_bw' ;;
depmod) ;;
modprobe)
    if [[ "$*" == ice ]]; then
        for count in "$FIXTURE"/sys/bus/pci/drivers/ice/0000:*/sriov_numvfs; do
            [[ $(cat "$count") == 0 ]] || exit 90
        done
        touch "$FIXTURE/probed"
        if [[ ${LATE_VFS:-0} != 0 ]]; then
            echo "$LATE_VFS" > "$FIXTURE/sys/bus/pci/drivers/ice/$BAD_PF/sriov_numvfs"
        fi
    fi
    ;;
ethtool)
    [[ "$1" == -T && -e "$FIXTURE/probed" ]] || exit 91
    pf=$(basename "$(readlink -f "$FIXTURE/sys/class/net/$2/device")")
    if [[ "$pf" == "$BAD_PF" ]] &&
        { [[ $(cat "$FIXTURE/sys/bus/pci/drivers/ice/bind") != "$pf" ]] ||
          [[ ${PERSISTENT:-0} == 1 ]]; }; then
        case "${BAD_CAPS:-software}" in
        error) exit 1 ;;
        no_phc) printf '    hardware-receive\nPTP Hardware Clock: -1\n' ;;
        no_rx) printf '    software-receive\nPTP Hardware Clock: 0\n' ;;
        *) printf '    software-receive\nPTP Hardware Clock: none\n' ;;
        esac
    else
        printf '    hardware-receive\nPTP Hardware Clock: 0\n'
    fi
    ;;
timeout)
    [[ -e "$FIXTURE/probed" ]] || exit 92
    [[ "$1" == --kill-after=* ]] || exit 93
    target="${!#}"
    if [[ ${FAIL_WRITE:-} == "$(basename "$target")" ]]; then
        exit "${WRITE_STATUS:-124}"
    fi
    /usr/bin/timeout "$@"
    ;;
*) exit 94 ;;
esac
"""


class IceActivationTests(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory(prefix="ice-activation-")
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        scripts = self.root / ".github/scripts/ci"
        scripts.mkdir(parents=True)
        self.script = scripts / "activate-ice.sh"
        source = (ROOT / ".github/scripts/ci/activate-ice.sh").read_text()
        # Redirect only the full script's absolute paths; production has no test hooks.
        self.script.write_text(
            source.replace("/sys/", f"{self.root}/sys/").replace(
                "/lib/modules/", f"{self.root}/lib/modules/"
            )
        )
        shutil.copyfile(
            ROOT / ".github/scripts/ci/validate-ice.sh", scripts / "validate-ice.sh"
        )
        artifact = self.root / ".local_install/ice/fixture/fixture/ice.ko"
        artifact.parent.mkdir(parents=True)
        artifact.write_text("cached module")
        self.module = self.root / "sys/module/ice/srcversion"
        self.module.parent.mkdir(parents=True)
        self.module.write_text("cached\n")
        self.driver = self.root / "sys/bus/pci/drivers/ice"
        self.driver.mkdir(parents=True)
        for action in ("bind", "unbind"):
            (self.driver / action).touch()
        self.pfs = ("0000:af:00.0", "0000:af:00.1")
        for index, pf in enumerate(self.pfs):
            device = self.driver / pf
            (device / "net" / f"ice{index}").mkdir(parents=True)
            (device / "driver").symlink_to(self.driver)
            (device / "sriov_numvfs").write_text("4\n")
            interface = self.root / "sys/class/net" / f"ice{index}"
            interface.mkdir(parents=True)
            (interface / "device").symlink_to(device)
        bin_dir = self.root / "bin"
        bin_dir.mkdir()
        for name in (
            "id",
            "uname",
            "modinfo",
            "nm",
            "depmod",
            "modprobe",
            "ethtool",
            "timeout",
        ):
            stub = bin_dir / name
            stub.write_text(STUB)
            stub.chmod(0o755)
        self.env = {
            **os.environ,
            "PATH": f"{bin_dir}:/usr/bin:/bin",
            "FIXTURE": str(self.root),
            "BAD_PF": self.pfs[1],
            "ICE_BUNDLE_ROOT": str(self.root / ".local_install/ice"),
        }

    def activate(self, **env):
        result = subprocess.run(
            ["/bin/bash", str(self.script)],
            env={**self.env, **env},
            capture_output=True,
            text=True,
            timeout=10,
            check=False,
        )
        self.commands = (self.root / "commands").read_text().splitlines()
        return result

    def assert_rebound_once(self, pf):
        writes = [line for line in self.commands if line.startswith("timeout ")]
        self.assertEqual(len(writes), 2, self.commands)
        for action in ("unbind", "bind"):
            self.assertEqual((self.driver / action).read_text(), pf + "\n")
        self.assertTrue(writes[0].endswith("/unbind"))
        self.assertTrue(writes[1].endswith("/bind"))

    def test_secondary_recovers_after_all_probes(self):
        result = self.activate()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_rebound_once(self.pfs[1])
        self.assertGreaterEqual(self.commands.count("ethtool -T ice1"), 2)

    def test_does_not_assume_function_zero_owns_clock(self):
        result = self.activate(BAD_PF=self.pfs[0])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_rebound_once(self.pfs[0])

    def test_healthy_shared_phc_needs_no_rebind(self):
        result = self.activate(BAD_PF="")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(any(line.startswith("timeout ") for line in self.commands))
        for index in range(2):
            self.assertIn(f"ethtool -T ice{index}", self.commands)

    def test_persistent_failure_is_reported_not_fatal(self):
        # Only the sniff PF's timestamps decide a compliance verdict, and the
        # acceptance suite gates that interface itself. Every job that validates
        # an ICE host runs this script, so a PF that stays without a PHC is
        # named on stderr and the activation still succeeds.
        result = self.activate(PERSISTENT="1")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_rebound_once(self.pfs[1])
        self.assertIn(self.pfs[1], result.stderr)
        self.assertIn("software timestamps", result.stderr)

    def test_missing_phc_or_receive_or_ethtool_error_needs_recovery(self):
        for caps in ("no_phc", "no_rx", "error"):
            with self.subTest(caps=caps):
                (self.driver / "bind").write_text("")
                (self.root / "commands").write_text("")
                result = self.activate(BAD_CAPS=caps)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assert_rebound_once(self.pfs[1])

    def test_missing_netdev_still_fails_without_rebind(self):
        # The unhealthy PF is the other one, so a rebind before the netdev check
        # would show up here: an incomplete probe must abort before any PF is
        # taken down, or VF configuration blocks on the half-probed one.
        shutil.rmtree(self.driver / self.pfs[1] / "net")
        result = self.activate(BAD_PF=self.pfs[0])
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("registered no netdev", result.stderr)
        self.assertFalse(any(line.startswith("timeout ") for line in self.commands))

    def test_missing_ethtool_fails_before_touching_any_pf(self):
        # Without ethtool every PF reads as unhealthy, so continuing would
        # unbind and rebind the whole card to prove nothing.
        (self.root / "bin/ethtool").unlink()
        result = self.activate()
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("requires ethtool", result.stderr)
        self.assertFalse(any(line.startswith("timeout ") for line in self.commands))

    def test_cached_module_identity_is_still_required(self):
        self.module.write_text("different\n")
        result = self.activate()
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("ICE did not come back up as the cached module", result.stderr)
        self.assertFalse(any(line.startswith("timeout ") for line in self.commands))

    def test_skips_rebind_with_vfs(self):
        # Unbinding a PF that already carries VFs would take them down under
        # whatever is using them, so recovery is skipped and reported.
        result = self.activate(LATE_VFS="1")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(self.pfs[1], result.stderr)
        self.assertIn("sriov_numvfs is 1", result.stderr)
        self.assertFalse(any(line.startswith("timeout ") for line in self.commands))

    def test_write_failure_or_timeout_aborts(self):
        for action in ("unbind", "bind"):
            for status in ("1", "124"):
                with self.subTest(action=action, status=status):
                    (self.driver / "bind").write_text("")
                    (self.root / "commands").write_text("")
                    result = self.activate(FAIL_WRITE=action, WRITE_STATUS=status)
                    self.assertNotEqual(result.returncode, 0, result.stdout)
                    self.assertIn(self.pfs[1], result.stderr)
                    writes = [s for s in self.commands if s.startswith("timeout ")]
                    self.assertEqual(len(writes), 1 if action == "unbind" else 2)


if __name__ == "__main__":
    unittest.main()
