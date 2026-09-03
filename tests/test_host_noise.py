import unittest
from unittest.mock import patch

from mxlperf.common import load_config
from mxlperf.host_noise import host_noise_plan, is_host_noise, ssh_target


class HostNoiseTests(unittest.TestCase):
    """host-a is the only host-scoped profile: one STREAM group per socket."""

    def test_host_a_pins_one_group_per_socket(self):
        cfg = load_config("pinned", [
            "LAB_CORES_PER_SOCKET=64", "LAB_CPU_NUMBERING=alternating",
            "LAB_RESERVED_CPUS=0-3",
        ], "host-a")
        cfg["NODE"] = cfg["LAB_DEFAULT_NODE"]
        self.assertTrue(is_host_noise(cfg))
        plan = host_noise_plan(cfg)
        self.assertEqual(plan["stressor"], "bandwidth")
        self.assertEqual(plan["workers_per_socket"], 2)
        self.assertEqual(plan["total_workers"], 4)
        self.assertEqual(len(plan["commands"]), 2)
        for socket_id, command in enumerate(plan["commands"]):
            argv = command["argv"]
            # One group per socket, memory bound to that socket's node, so the
            # pressure is local and the cross-socket picture stays readable.
            self.assertEqual(command["numa_node"], socket_id)
            self.assertIn(f"--membind={socket_id}", argv)
            self.assertEqual(argv[argv.index("--stream") + 1], "2")
            self.assertEqual(argv[argv.index("--stream-index") + 1], "0")
            self.assertEqual(argv[-2:], ["--timeout", "0"])
        # Socket 0 owns the even CPU IDs on the reference worker, socket 1 the odd.
        self.assertEqual(len(plan["commands"][0]["cpus"]), 62)
        self.assertEqual(len(plan["commands"][1]["cpus"]), 62)
        self.assertTrue(all(cpu % 2 == 0 for cpu in plan["commands"][0]["cpus"]))
        self.assertTrue(all(cpu % 2 == 1 for cpu in plan["commands"][1]["cpus"]))

    def test_host_a_uses_socket_local_contiguous_cpu_pools(self):
        cfg = load_config("pinned", [], "host-a")
        plan = host_noise_plan(cfg)
        self.assertEqual(plan["commands"][0]["cpus"], list(range(2, 32)))
        self.assertEqual(plan["commands"][1]["cpus"], list(range(32, 60)) + [62, 63])
        self.assertEqual(plan["commands"][0]["argv"][:5], [
            "taskset", "-c", "2-31", "numactl", "--membind=0",
        ])
        self.assertEqual(plan["commands"][1]["argv"][:5], [
            "taskset", "-c", "32-59,62-63", "numactl", "--membind=1",
        ])

    def test_host_noise_rejects_unsafe_extra_arguments(self):
        cfg = load_config("baseline", [], "host-a")
        cfg["NOISY_NEIGHBOR_HOST_EXTRA_ARGS"] = "--all 1"
        with self.assertRaisesRegex(ValueError, "broad stressor"):
            host_noise_plan(cfg)

    def test_host_noise_rejects_unsafe_equals_argument(self):
        cfg = load_config("baseline", [], "host-a")
        cfg["NOISY_NEIGHBOR_HOST_EXTRA_ARGS"] = "--stream=60"
        with self.assertRaisesRegex(ValueError, "broad stressor"):
            host_noise_plan(cfg)

    def test_host_noise_rejects_workers_above_socket_pool(self):
        cfg = load_config("baseline", [
            "LAB_CORES_PER_SOCKET=64", "LAB_CPU_NUMBERING=alternating",
            "LAB_RESERVED_CPUS=0-3",
        ], "host-a")
        cfg["NOISY_NEIGHBOR_HOST_WORKERS_PER_SOCKET"] = "63"
        with self.assertRaisesRegex(ValueError, "between 1 and 62"):
            host_noise_plan(cfg)

    @patch("mxlperf.host_noise.read_env")
    def test_ssh_target_uses_selected_node_inventory(self, read_env_mock):
        read_env_mock.return_value = {"LAB_SSH_USER": "tester", "WORKER_1_HOST": "192.0.2.10"}
        cfg = {"NODE": "worker-1", "LAB_DEFAULT_NODE": "worker-1"}
        self.assertEqual(ssh_target(cfg), "tester@192.0.2.10")


if __name__ == "__main__":
    unittest.main()
