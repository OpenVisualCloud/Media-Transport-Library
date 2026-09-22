# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

"""The perf sweep's core accounting has to mean what the report column says.

`--sch_session_quota N` is denominated in 1080p59 stream bandwidths -- one unit
is 2589 Mbps -- and st_rx_video_session.c charges `quota_mbs *= ops->num_port`
*outside* the `!mt_user_quota_active()` guard, so a redundant session costs two
units however the quota was set. Run 35577768555 shows the consequence in the
library's own scheduler log: at the single-core quota of 60 every redundant
session added 5178 Mbps, scheduler 0 filled at exactly 30 of them (155340 Mbps =
60 units), and session 31 onward went to scheduler 1. That sweep's first probe
was 32, so all six of its iterations ran on two cores, and the report published
the result in the single-core table as "32 sessions, 2 cores".

The same run shows the multi-core half. Phase 1 searched for the session ceiling
at the per-mode quota, 18 for +DMA against 16 for no-DMA, so +DMA put 36
sessions on 2 schedulers and failed while no-DMA spread the same 36 over 3 and
passed 36/36 -- the +DMA column lost three sessions to packing density, not to
DMA. The NIC's ceiling has to be found before cores are economised, not after.

Core counts here are the measured ones from that run's `[CPU_CORES]` lines --
multi-core adds a dedicated sys lcore, so a 3-scheduler placement measures 4.
"""

import importlib.util
import sys
import types
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# The sweep module pulls the whole acceptance framework in at import time; none
# of it is reachable from the pure helpers under test, so stub the lot.
_STUB_ATTRS = {
    "common.host_setup": ["optimize_cpu_cores_for_turbo", "restore_cpu_cores"],
    "common.nicctl": ["ensure_vfio_bound", "reset_vfio_bindings"],
    "conftest": ["get_host_mtl_path", "is_host_sut"],
    "mtl_engine.dma": ["setup_host_dma_all"],
    "mtl_engine.execute": ["kill_stale_processes", "read_remote_log", "run"],
    "mtl_engine.performance_monitoring": [
        "CpuCoreMonitor",
        "display_session_results",
        "log_cpu_core_results",
        "monitor_dev_rate",
        "monitor_rx_fps",
        "monitor_rx_frames_simple",
        "monitor_rx_throughput",
        "monitor_tx_fps",
        "monitor_tx_frames",
        "monitor_tx_throughput",
    ],
    "mtl_engine.rxtxapp": ["RxTxApp"],
}


_STUB_MODULES = [
    "common",
    "mfd_common_libs",
    "mfd_common_libs.log_levels",
    "mtl_engine",
    "mtl_engine.const",
    "mtl_engine.ip_pools",
    "mtl_engine.media_files",
    *_STUB_ATTRS,
]


class _Mark:
    """Stands in for one `pytest.mark.*`, as a decorator or a factory.

    `pytest.mark.performance` is applied straight to a function, while
    `pytest.mark.parametrize(...)` is called first and the result applied.  Both
    only have to return the function unchanged: nothing in this tier collects
    the sweep's cases, it reads its module-level helpers and constants.
    """

    def __call__(self, *args, **kwargs):
        if len(args) == 1 and not kwargs and callable(args[0]):
            return args[0]

        return self

    def __getattr__(self, _name):
        return _Mark()


def _load_sweep_module():
    """Import test_vf_perf_dualhost.py with its framework dependencies stubbed.

    Every stub is removed again afterwards.  `unittest discover` -- which is how
    unit_tests.yml runs this tier -- imports every module of it into one
    interpreter, so a stub left behind would answer another module's import.
    """
    saved = {name: sys.modules.get(name) for name in _STUB_MODULES}
    for name in _STUB_MODULES:
        module = types.ModuleType(name)
        module.__path__ = []
        sys.modules[name] = module

    sys.modules["mfd_common_libs.log_levels"].TEST_PASS = 25
    sys.modules["mtl_engine.const"].RXTXAPP_PATH = "RxTxApp"
    # Only the three keys the parametrize list indexes, and only as markers.
    sys.modules["mtl_engine.media_files"].yuv_files_422rfc10 = {
        "ParkJoy_1080p": {},
        "ParkJoy_4K": {},
        "Penguin_8K": {},
    }
    for name, attrs in _STUB_ATTRS.items():
        for attr in attrs:
            setattr(sys.modules[name], attr, object())

    # Another module of this tier stubs `pytest` for its own import and leaves
    # the stub in place, so the name being present says nothing about `mark`
    # being there.  Fill in only what is missing, and put it back as it was.
    pytest_mod = sys.modules.setdefault("pytest", types.ModuleType("pytest"))
    had_mark = hasattr(pytest_mod, "mark")
    if not had_mark:
        pytest_mod.mark = _Mark()

    path = ROOT / "tests/acceptance/tests/dual/performance/test_vf_perf_dualhost.py"
    spec = importlib.util.spec_from_file_location("perf_sweep_under_test", path)
    module = importlib.util.module_from_spec(spec)
    try:
        spec.loader.exec_module(module)
    finally:
        if not had_mark:
            del pytest_mod.mark
        for name, previous in saved.items():
            if previous is None:
                sys.modules.pop(name, None)
            else:
                sys.modules[name] = previous

    return module


sweep = _load_sweep_module()

# Every (is_tx, use_dma, redundant) the sweep can ask a phase-2 quota for. TX
# never runs with DMA -- those ids skip -- so the combination is left out.
MC_MODES = [
    (is_tx, use_dma, redundant)
    for is_tx in (True, False)
    for use_dma in (True, False)
    for redundant in (True, False)
    if not (is_tx and use_dma)
]

# A redundant session charges one quota unit per port.
PORTS_REDUNDANT = 2

# The largest redundant single-core count the sweep has ever published, from the
# +DMA row of run 35577768555. The quota has to seat this on one scheduler.
OBSERVED_SC_REDUNDANT_SESSIONS = 32


def _iteration(num_sessions, passed, cores_used, quota):
    """Build one iteration_results entry as _run_one() records it."""
    return {
        "num_sessions": num_sessions,
        "passed": passed,
        "successful_count": num_sessions if passed else 0,
        "detail": "synthetic",
        "cores_used": cores_used,
        "quota": quota,
    }


class SingleCoreQuotaTests(unittest.TestCase):
    """The single-core quota has to absorb the redundant per-port charge."""

    def test_quota_seats_observed_redundant_count_on_one_scheduler(self):
        charge = OBSERVED_SC_REDUNDANT_SESSIONS * PORTS_REDUNDANT
        self.assertGreaterEqual(
            sweep.SCH_SESSION_QUOTA_SINGLE_CORE,
            charge,
            "a single-core sweep that splits over two schedulers is not a "
            "single-core measurement",
        )


class QuotaRangeTests(unittest.TestCase):
    """Every quota the sweep sets has to be one the library will accept."""

    def test_every_quota_is_inside_the_range_the_library_accepts(self):
        # args.c ST_ARG_SCH_SESSION_QUOTA keeps `nb > 0 && nb < 100` and drops
        # anything else silently, so an out-of-range quota does not fail the
        # run -- it leaves the library default in place, unlogged.
        quotas = {
            name: value
            for name, value in vars(sweep).items()
            if name.startswith("SCH_SESSION_QUOTA_")
        }
        self.assertTrue(quotas, "no quota constants found; has the prefix moved?")
        for name, value in quotas.items():
            with self.subTest(quota=name):
                self.assertIsInstance(value, int)
                self.assertGreater(value, 0)
                self.assertLess(value, 100)


class SingleCoreViolationTests(unittest.TestCase):
    """A single-core iteration that used more than one core is not a pass."""

    def test_two_cores_in_single_core_mode_is_a_violation(self):
        reason = sweep._single_core_violation(True, 2)
        self.assertIsNotNone(reason)
        self.assertIn("2", reason)

    def test_one_core_in_single_core_mode_is_clean(self):
        self.assertIsNone(sweep._single_core_violation(True, 1))

    def test_unmeasured_cores_are_not_a_violation(self):
        # cores_used is 0 when the CPU sampler produced nothing; inventing a
        # failure there would fail the sweep for a monitoring gap.
        self.assertIsNone(sweep._single_core_violation(True, 0))

    def test_multi_core_mode_may_use_many_cores(self):
        for cores in (2, 4, 10):
            with self.subTest(cores=cores):
                self.assertIsNone(sweep._single_core_violation(False, cores))


class FewestCoresTests(unittest.TestCase):
    """The reported core count is the cheapest placement that held the ceiling."""

    def test_picks_the_cheapest_placement_not_the_last_attempt(self):
        # Phase 2 can pass at the same session count while measuring *more*
        # cores than phase 1 did; reading the last entry publishes the worse one.
        results = [
            _iteration(36, True, 4, 12),
            _iteration(36, True, 6, 18),
        ]
        best = sweep._fewest_cores_result(results, 36)
        self.assertEqual(best["cores_used"], 4)
        self.assertEqual(best["quota"], 12)

    def test_prefers_the_denser_phase_two_packing_when_it_passes(self):
        # The intended path: phase 1 finds 36 thinly packed, phase 2 holds 36 on
        # fewer cores, and the denser one is what gets published.
        results = [
            _iteration(36, True, 4, 12),
            _iteration(36, True, 3, 24),
        ]
        best = sweep._fewest_cores_result(results, 36)
        self.assertEqual(best["cores_used"], 3)
        self.assertEqual(best["quota"], 24)

    def test_ignores_a_failed_denser_packing(self):
        # The +DMA shape of run 35577768555: 36 sessions need 3 schedulers, and
        # the 2-scheduler retry fails, so 4 cores stands.
        results = [
            _iteration(36, True, 4, 12),
            _iteration(36, False, 3, 18),
        ]
        best = sweep._fewest_cores_result(results, 36)
        self.assertEqual(best["cores_used"], 4)

    def test_ignores_iterations_at_other_session_counts(self):
        results = [
            _iteration(33, True, 2, 18),
            _iteration(36, True, 4, 12),
        ]
        self.assertEqual(sweep._fewest_cores_result(results, 36)["cores_used"], 4)

    def test_ignores_unmeasured_core_counts(self):
        results = [
            _iteration(36, True, 0, 12),
            _iteration(36, True, 4, 16),
        ]
        self.assertEqual(sweep._fewest_cores_result(results, 36)["cores_used"], 4)

    def test_returns_none_when_nothing_passed(self):
        self.assertIsNone(
            sweep._fewest_cores_result([_iteration(36, False, 4, 12)], 36)
        )

    def test_returns_none_on_an_empty_sweep(self):
        self.assertIsNone(sweep._fewest_cores_result([], 0))


class Phase1PacksThinnestTests(unittest.TestCase):
    """Phase 1 hunts the NIC ceiling, so it must not pack denser than phase 2."""

    def test_phase1_packs_no_denser_than_any_phase2_quota(self):
        # Falling through to the per-mode quota is what let packing density cap
        # the session count, and it made +DMA search at 18 while no-DMA searched
        # at 16 -- so the two columns of the report were not comparable. One
        # scalar for all modes is the property that fixes both.
        for is_tx, use_dma, redundant in MC_MODES:
            with self.subTest(is_tx=is_tx, use_dma=use_dma, redundant=redundant):
                self.assertLessEqual(
                    sweep.SCH_SESSION_QUOTA_PHASE1_MC,
                    sweep._select_mc_quota(is_tx, use_dma, redundant),
                    "phase 1 must spread at least as thinly as phase 2, or the "
                    "session ceiling it reports is a core limit",
                )


class CrashClassificationTests(unittest.TestCase):
    """A run that died is not a capacity result, whatever it reported first.

    The stat dumps stop wherever the process did, so a crash late in a run
    leaves a steady window that reads as full rate at the target count.  If
    that is published as a pass the sweep reads the crash as headroom and
    searches upward from it, so the crash has to veto the iteration -- and it
    has to keep scheduling the VF FLR that the next iteration needs.
    """

    def test_the_signals_rxtxapp_dies_with_veto_the_iteration(self):
        # 128 + signal, as the runner reports it. A SIGSEGV scored as a pass is
        # exactly how an inflated ceiling reaches the report.
        for code in (134, 139):  # SIGABRT, SIGSEGV
            with self.subTest(code=code):
                self.assertTrue(sweep._is_crash_code(code))

    def test_a_signal_is_a_crash(self):
        # The process runner reports a signal as its negation.
        self.assertTrue(sweep._is_crash_code(-11))

    def test_a_clean_exit_is_not_a_crash(self):
        self.assertFalse(sweep._is_crash_code(0))

    def test_a_late_teardown_error_is_not_a_crash(self):
        # The whole point of keeping a non-zero exit: RxTxApp returns 1 after a
        # complete run whose teardown failed, and that data is good.
        self.assertFalse(sweep._is_crash_code(1))

    def test_every_crash_code_also_triggers_the_vf_reset(self):
        # _run_one() re-reads the code out of the detail string to decide on the
        # FLR, so the two classifiers have to agree on every code.
        for code in sweep.CRASH_EXIT_CODES:
            with self.subTest(code=code):
                detail = f"32/32 sessions at 59 fps, exit code {code}"
                self.assertTrue(sweep._is_crash(detail))

    def test_a_clean_iteration_does_not_trigger_the_vf_reset(self):
        self.assertFalse(sweep._is_crash("32/32 sessions at 59 fps"))


if __name__ == "__main__":
    unittest.main()
