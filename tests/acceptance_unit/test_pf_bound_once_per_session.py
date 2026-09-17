# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation
"""A PF is bound to the PMD once per session, not once per test.

Handing a PF back to the kernel re-probes the out-of-tree ice driver, and that
path faults on repeated use -- ``ice_probe -> ice_vsi_setup ->
ice_add_rss_cfg -> ice_add_prof`` -- exiting a kworker with interrupts disabled,
so a spinlock stays held and the host stops answering. In CI that reads as a
runner losing communication rather than as a test failure.

``setup_interfaces`` is function-scoped, so a fresh InterfaceSetup per test used
to bind the PF and hand it straight back: one probe per parameter, where the
recorded fault needed only three within four minutes.

These tests drive the real InterfaceSetup with fake hosts and record the driver
transitions it asks for. Reintroducing a per-test rebind, or dropping the
on-demand restore that stops a parked PF from starving a netdev test, fails them.
"""

import ast
import sys
import types
import unittest
from pathlib import Path

ACCEPTANCE = Path(__file__).resolve().parents[1] / "acceptance"
sys.path.insert(0, str(ACCEPTANCE))


class HarnessGaveUp(Exception):
    """What the stubbed ``pytest.skip``/``pytest.fail`` raise.

    Stubbed unconditionally rather than only when pytest is missing, so this
    behaves the same on the CI runner (no pytest) and on a developer host (real
    pytest, whose skip derives from BaseException and would abort the unittest
    runner instead of failing a case). No path asserted on below is meant to
    reach these, so a test that trips one should say so loudly.
    """


def _module(name, **attrs):
    module = types.ModuleType(name)
    for attr, value in attrs.items():
        setattr(module, attr, value)
    return module


def _gave_up(*args, **kwargs):
    raise HarnessGaveUp(*args)


_STUBS = {
    "pytest": _module("pytest", skip=_gave_up, fail=_gave_up),
    "mfd_network_adapter": _module("mfd_network_adapter", NetworkInterface=object),
}
_SAVED = {name: sys.modules.get(name) for name in _STUBS}
sys.modules.update(_STUBS)
try:
    from common.nicctl import InterfaceSetup, restore_kernel_pfs
finally:
    # Leaving stubs installed would change how every later module in this
    # discover run sees pytest; the tier pops them (test_audio_frame_geometry).
    for _name, _prev in _SAVED.items():
        if _prev is None:
            sys.modules.pop(_name, None)
        else:
            sys.modules[_name] = _prev

PF0 = "0000:38:00.0"
PF1 = "0000:38:00.1"


class Recorder:
    """The driver transitions and netdev scans the code under test performs."""

    def __init__(self):
        self.events = []

    def of(self, kind):
        return [target for k, target in self.events if k == kind]


class FakeNicctl:
    """Records what the harness asks for, and reproduces nicctl.sh's own
    side effect on driver state.

    ``script/nicctl.sh`` runs its "suppose bind kernel should be called for
    following commands" branch before ``create_vf``/``create_tvf``/
    ``disable_vf``, so any of those on a PF with no netdev -- i.e. one parked
    on vfio-pci -- hands it back to the kernel. That happens whether or not
    the harness knows, which is exactly why the parked set cannot be trusted
    as driver state. Modelling it here is what makes this a real gate.
    """

    def __init__(self, recorder):
        self._rec = recorder
        self.on_pmd = set()

    def bind_pmd(self, pci_id):
        self._rec.events.append(("bind_pmd", str(pci_id)))
        self.on_pmd.add(str(pci_id))

    def bind_kernel(self, pci_id):
        self._rec.events.append(("bind_kernel", str(pci_id)))
        self.on_pmd.discard(str(pci_id))

    def create_vfs(self, pci_id, num_of_vfs=1):
        self._rec.events.append(("create_vfs", str(pci_id)))
        if str(pci_id) in self.on_pmd:
            # nicctl.sh's implicit rebind. Deliberately NOT recorded as a
            # bind_kernel event: the harness never asked for it, so counting it
            # would let the per-test-rebind assertions pass on a false premise.
            self.on_pmd.discard(str(pci_id))
        return [f"{pci_id}-vf{i}" for i in range(num_of_vfs)]


class _PciAddress:
    def __init__(self, lspci):
        self.lspci = lspci


class _Interface:
    def __init__(self, name, pci):
        self.name = name
        self.pci_address = _PciAddress(pci)


class _Result:
    def __init__(self, stdout):
        self.stdout = stdout
        self.return_code = 0


class FakeConnection:
    """Answers the ``ip -o -4 addr show`` probe used to find kernel netdevs."""

    def __init__(self, recorder):
        self._rec = recorder

    def path(self, *parts):
        return "/".join(parts)

    def execute_command(self, command, **kwargs):
        name = command.split()[-1]
        self._rec.events.append(("scan", name))
        return _Result(f"1: {name} inet 192.168.0.1/24")


class FakeHost:
    def __init__(self, recorder, interfaces=((("eth0", PF0), ("eth1", PF1)))):
        self.name = "host0"
        self.network_interfaces = [_Interface(n, p) for n, p in interfaces]
        self.connection = FakeConnection(recorder)
        self.topology = types.SimpleNamespace(extra_info=types.SimpleNamespace())


def _setup(host, recorder, nicctl=None):
    """A fresh InterfaceSetup, as the function-scoped fixture builds per test.

    *nicctl* carries driver state across those instances the way the real host
    does; leave it out when a case only reads the recorded events.
    """
    setup = InterfaceSetup({host.name: host}, "/opt/mtl")
    setup.nicctl_objs = {host.name: nicctl or FakeNicctl(recorder)}
    return setup


class PfBindingTests(unittest.TestCase):
    def setUp(self):
        self.rec = Recorder()
        self.host = FakeHost(self.rec)

    def test_no_pf_rebind_across_many_tests(self):
        """Ten tests on the same PF must cost zero rebinds.

        The count that matters is ``bind_kernel``, not ``bind_pmd``: only the
        rebind re-probes the driver. Asserting a single ``bind_pmd`` instead
        would forbid the repeat bind the next case shows to be necessary.
        """
        for _ in range(10):
            setup = _setup(self.host, self.rec)
            self.assertEqual(setup.get_interfaces_list_single("PF", count=1), [PF0])
            setup.cleanup()

        self.assertEqual(
            self.rec.of("bind_kernel"),
            [],
            "a test handed the PF back to the kernel; that rebind is the "
            f"ice_probe that faults the driver: {self.rec.events}",
        )

    def test_a_pf_the_shell_unparked_is_bound_again_before_dpdk_gets_it(self):
        """A VF subcommand silently returns a parked PF to the kernel driver.

        ``nicctl.sh`` does that in the branch it runs before ``create_vf``, so
        the harness must not skip ``bind_pmd`` on the strength of its own
        ledger: the next PF test would hand DPDK a port the kernel owns, which
        fails at EAL probe. A PF test followed by a VF test on the same PF is
        the ordering a full ``tests/single`` run produces.
        """
        nicctl = FakeNicctl(self.rec)

        pf_test = _setup(self.host, self.rec, nicctl)
        self.assertEqual(pf_test.get_interfaces_list_single("PF", count=1), [PF0])
        pf_test.cleanup()
        self.assertIn(PF0, nicctl.on_pmd)

        vf_test = _setup(self.host, self.rec, nicctl)
        vf_test.get_interfaces_list_single("VF", count=1)
        vf_test.cleanup()
        self.assertNotIn(PF0, nicctl.on_pmd, "the fake stopped modelling nicctl.sh")

        again = _setup(self.host, self.rec, nicctl)
        self.assertEqual(again.get_interfaces_list_single("PF", count=1), [PF0])
        self.assertIn(
            PF0,
            nicctl.on_pmd,
            "DPDK was handed a PF the kernel driver owns: the ledger said "
            f"'parked' so the bind was skipped. Events: {self.rec.events}",
        )
        self.assertEqual(
            self.rec.of("bind_kernel"),
            [],
            f"and the repair must still not cost a rebind: {self.rec.events}",
        )

    def test_mixed_pf_tx_vf_rx_does_not_rebind_either(self):
        """The shape test_st20p_pacing_way_phc uses: PF for TX, VF for RX."""
        for _ in range(5):
            setup = _setup(self.host, self.rec)
            tx, rx = setup.get_mixed_interfaces_list_single(
                tx_interface_type="PF", rx_interface_type="VF"
            )
            self.assertEqual(tx, PF0)
            self.assertEqual(rx, f"{PF1}-vf0")
            setup.cleanup()

        self.assertEqual(self.rec.of("bind_kernel"), [], self.rec.events)

    def test_session_restore_hands_the_pf_back_exactly_once(self):
        setup = _setup(self.host, self.rec)
        setup.get_interfaces_list_single("PF", count=1)
        setup.cleanup()

        nicctl = setup.nicctl_objs[self.host.name]
        restore_kernel_pfs(self.host, nicctl)
        self.assertEqual(self.rec.of("bind_kernel"), [PF0], self.rec.events)

        # Session teardown can follow an on-demand restore, so a second call
        # must not re-probe a PF the kernel already owns.
        restore_kernel_pfs(self.host, nicctl)
        self.assertEqual(
            self.rec.of("bind_kernel"),
            [PF0],
            f"restoring an already-restored PF re-probed it: {self.rec.events}",
        )

    def test_netdev_scan_restores_a_parked_pf_before_looking(self):
        """A parked PF has no netdev, so it must be handed back before the scan.

        Otherwise it drops out of the scan and the test skips for want of an
        interface -- trading a wedged host for a silently missing test.
        """
        setup = _setup(self.host, self.rec)
        setup.get_interfaces_list_single("PF", count=1)
        setup.cleanup()

        _setup(self.host, self.rec).get_native_af_xdp_interfaces(count=2)

        kinds = [kind for kind, _ in self.rec.events]
        self.assertIn("bind_kernel", kinds, self.rec.events)
        self.assertIn("scan", kinds, self.rec.events)
        self.assertLess(
            kinds.index("bind_kernel"),
            kinds.index("scan"),
            f"the netdev scan ran while the PF was still parked: {self.rec.events}",
        )

    def test_kernel_datapath_gets_its_interfaces_back_from_vfio(self):
        """MTL's kernel-socket datapath is handed netdev names, so it needs one."""
        setup = _setup(self.host, self.rec)
        setup.get_interfaces_list_single("PF", count=1)
        setup.cleanup()

        pair = _setup(self.host, self.rec).get_interfaces_list_single("kernel", count=2)
        self.assertEqual(pair, ["kernel:eth0", "kernel:eth1"])
        self.assertEqual(
            self.rec.of("bind_kernel"),
            [PF0],
            "a kernel-socket test was pointed at a netdev that vfio-pci still "
            f"owns, so MTL cannot open a socket on it: {self.rec.events}",
        )

    def test_a_pf_with_no_netdev_fails_loudly_instead_of_kernel_none(self):
        """Giving the driver back cannot revive a name cached as missing.

        ``host.network_interfaces`` is read once per session, so a PF that was
        parked when the session started keeps an unusable name all run. Naming
        it ``kernel:None`` to MTL would surface as an unrelated RxTxApp error.
        """
        host = FakeHost(self.rec, interfaces=(("eth0", PF0), (None, PF1)))

        for getter in (
            lambda s: s.get_interfaces_list_single("kernel", count=2),
            lambda s: s.get_pmd_kernel_interfaces("VF"),
        ):
            with self.assertRaises(HarnessGaveUp) as failure:
                getter(_setup(host, self.rec))
            self.assertIn(PF1, str(failure.exception))

    def test_pmd_kernel_pairing_restores_only_the_kernel_half(self):
        """The netdev half needs its driver back; the DPDK half must keep vfio.

        Restoring every parked PF here would hand back the one bound a line
        earlier, so with ``interface_type="PF"`` every test would pay a rebind
        -- the probe this change exists to remove.
        """
        setup = _setup(self.host, self.rec)
        setup.get_interfaces_list_single("PF", count=2)
        setup.cleanup()

        mark = len(self.rec.events)
        _setup(self.host, self.rec).get_pmd_kernel_interfaces("PF")
        pairing = self.rec.events[mark:]

        self.assertEqual(
            [target for kind, target in pairing if kind == "bind_kernel"],
            [PF1],
            "the kernel-socket half was left on vfio-pci with no netdev, or "
            f"the DPDK half was handed back after being bound: {pairing}",
        )
        kinds = [kind for kind, _ in pairing]
        self.assertIn("bind_pmd", kinds, f"the DPDK half was never bound: {pairing}")
        self.assertLess(
            kinds.index("bind_pmd"),
            kinds.index("bind_kernel"),
            f"the restore ran before the DPDK half was bound: {pairing}",
        )

    def test_the_fakes_are_actually_exercised(self):
        """Keeps the assertions above from passing on an inert recorder."""
        setup = _setup(self.host, self.rec)
        setup.get_interfaces_list_single("PF", count=1)
        self.assertTrue(self.rec.events, "InterfaceSetup issued no calls at all")


class ConftestWiringTests(unittest.TestCase):
    """The one link above cannot reach: how conftest.py schedules the restore.

    A fixture is only importable with the suite's pytest plugins installed, which
    this tier does not have, so it is read from source instead. Each property
    below is load-bearing on its own: at function scope the PF goes back after
    every test, which is the per-test re-probe this change removes; without
    autouse nothing requests the fixture, so it is dead code and no PF ever goes
    back; ahead of the yield it runs at session start and restores nothing.
    """

    def setUp(self):
        tree = ast.parse((ACCEPTANCE / "conftest.py").read_text())
        self.fixtures = [
            node
            for node in ast.walk(tree)
            if isinstance(node, ast.FunctionDef) and self._restore_calls(node)
        ]
        self.assertTrue(
            self.fixtures, "no conftest.py function calls restore_kernel_pfs"
        )

    @staticmethod
    def _restore_calls(node):
        return [
            n
            for n in ast.walk(node)
            if isinstance(n, ast.Call)
            and isinstance(n.func, ast.Name)
            and n.func.id == "restore_kernel_pfs"
        ]

    @staticmethod
    def _decorator_arg(fixture, name):
        return {
            kw.arg: kw.value.value
            for deco in fixture.decorator_list
            if isinstance(deco, ast.Call)
            for kw in deco.keywords
            if isinstance(kw.value, ast.Constant)
        }.get(name)

    def test_the_restore_fixture_is_session_scoped(self):
        for fixture in self.fixtures:
            self.assertEqual(
                self._decorator_arg(fixture, "scope"),
                "session",
                f"conftest.py:{fixture.lineno} {fixture.name} restores PFs at "
                "function scope (or the default); anything but session "
                "re-probes the ice driver once per test",
            )

    def test_the_restore_fixture_is_autouse(self):
        for fixture in self.fixtures:
            self.assertIs(
                self._decorator_arg(fixture, "autouse"),
                True,
                f"conftest.py:{fixture.lineno} {fixture.name} is not autouse, "
                "and no test requests it, so no PF is ever handed back",
            )

    def test_the_restore_runs_at_teardown_not_setup(self):
        for fixture in self.fixtures:
            yields = [n.lineno for n in ast.walk(fixture) if isinstance(n, ast.Yield)]
            self.assertTrue(
                yields,
                f"conftest.py:{fixture.lineno} {fixture.name} does not yield, so "
                "it has no teardown half",
            )
            self.assertGreater(
                min(call.lineno for call in self._restore_calls(fixture)),
                max(yields),
                f"conftest.py:{fixture.lineno} {fixture.name} restores PFs "
                "before the yield, i.e. at session start, when nothing is "
                "parked yet",
            )


if __name__ == "__main__":
    unittest.main()
