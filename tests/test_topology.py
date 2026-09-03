import unittest

from mxlperf import topology
from mxlperf.common import load_config


def topo(**overrides):
    cfg = {
        "LAB_SOCKET_COUNT": "2",
        "LAB_CORES_PER_SOCKET": "4",
        "LAB_THREADS_PER_CORE": "2",
        "LAB_CPU_NUMBERING": "contiguous",
        "LAB_SOCKET0_PARITY": "even",
        "LAB_RESERVED_CPUS": "",
    }
    cfg.update(overrides)
    return topology.from_config(cfg)


class SiblingTests(unittest.TestCase):
    """The sibling of core N is CPU N + physical_cores - the Linux numbering."""

    def test_siblings_share_a_core_and_a_socket(self):
        t = topo()
        self.assertEqual(t.physical_cores, 8)
        self.assertEqual(t.logical_cpus, 16)
        for cpu in range(t.logical_cpus):
            sibling = (cpu + 8) % 16
            self.assertEqual(topology.core_id(t, cpu), topology.core_id(t, sibling))
            self.assertEqual(topology.socket_id(t, cpu), topology.socket_id(t, sibling))
            self.assertEqual(topology.siblings(t, cpu), sorted({cpu, sibling}))

    def test_sibling_threads_do_not_invent_extra_sockets(self):
        # The bug this replaced: cpu // cores_per_socket called CPU 8 socket 2 on
        # a two-socket machine, so a same-core pair looked like a cross-socket one.
        t = topo()
        self.assertEqual([topology.socket_id(t, cpu) for cpu in range(16)],
                         [0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1])

    def test_alternating_numbering_follows_core_parity(self):
        t = topo(LAB_CPU_NUMBERING="alternating")
        self.assertEqual([topology.socket_id(t, cpu) for cpu in range(16)],
                         [0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1])
        t = topo(LAB_CPU_NUMBERING="alternating", LAB_SOCKET0_PARITY="odd")
        self.assertEqual([topology.socket_id(t, cpu) for cpu in range(4)], [1, 0, 1, 0])


class PoolTests(unittest.TestCase):
    def test_cpu_pools_include_both_threads_of_every_free_core(self):
        pools = topology.cpu_pools(topo(LAB_RESERVED_CPUS="0,8"))
        self.assertEqual(pools[0], [1, 2, 3, 9, 10, 11])
        self.assertEqual(pools[1], [4, 5, 6, 7, 12, 13, 14, 15])

    def test_core_pools_drop_cores_with_a_reserved_sibling(self):
        # CPU 0 reserved but not its sibling 8: core 0 cannot be handed out
        # exclusively under full-pcpus-only, so it is not capacity.
        pools = topology.core_pools(topo(LAB_RESERVED_CPUS="0"))
        self.assertEqual(pools[0], [[1, 9], [2, 10], [3, 11]])
        self.assertEqual(len(pools[1]), 4)
        # Reserve the whole core and only that core is lost.
        pools = topology.core_pools(topo(LAB_RESERVED_CPUS="0,8"))
        self.assertEqual(pools[0], [[1, 9], [2, 10], [3, 11]])

    def test_partially_reserved_cores_are_reported(self):
        self.assertEqual(topology.partially_reserved_cores(topo(LAB_RESERVED_CPUS="0,3")), [0, 3])
        self.assertEqual(topology.partially_reserved_cores(topo(LAB_RESERVED_CPUS="0,3,8,11")), [])
        # Impossible without SMT: every core is its own sibling group.
        self.assertEqual(
            topology.partially_reserved_cores(topo(LAB_THREADS_PER_CORE="1", LAB_RESERVED_CPUS="0,3")),
            [],
        )


class RequestTests(unittest.TestCase):
    def test_a_core_costs_threads_per_core_cpus(self):
        self.assertEqual(topology.cpu_request_for_cores(topo(), 5), "10")
        self.assertEqual(topology.cpu_request_for_cores(topo(LAB_THREADS_PER_CORE="1"), 5), "5")


class ConfigTests(unittest.TestCase):
    def test_lab_env_defaults_to_no_smt_and_full_pcpus_only(self):
        cfg = load_config("pinned", [])
        self.assertEqual(topology.from_config(cfg).threads_per_core, 1)
        self.assertTrue(topology.full_pcpus_only(cfg))

    def test_bad_values_are_rejected(self):
        with self.assertRaisesRegex(ValueError, "exactly 2 sockets"):
            topo(LAB_SOCKET_COUNT="4")
        with self.assertRaisesRegex(ValueError, "LAB_THREADS_PER_CORE must be >= 1"):
            topo(LAB_THREADS_PER_CORE="0")
        with self.assertRaisesRegex(ValueError, "alternating or contiguous"):
            topo(LAB_CPU_NUMBERING="round-robin")

    def test_no_smt_is_identical_to_the_pre_smt_planner(self):
        # Regression guard for the refactor: with one thread per core the model
        # must reproduce the old cpu // cores_per_socket and even/odd formulas.
        for numbering in ("contiguous", "alternating"):
            with self.subTest(numbering=numbering):
                t = topo(LAB_THREADS_PER_CORE="1", LAB_CPU_NUMBERING=numbering,
                         LAB_CORES_PER_SOCKET="32")
                for cpu in range(64):
                    expected = cpu // 32 if numbering == "contiguous" else cpu % 2
                    self.assertEqual(topology.socket_id(t, cpu), expected)


if __name__ == "__main__":
    unittest.main()
