import unittest

from mxlperf.common import load_config
from mxlperf.render import (
    cpu_pools,
    noisy_neighbor_pod,
    noisy_neighbor_pods,
    plan_cpu_sets,
    pod_spec,
    sidecar,
)


class NoisyNeighborTests(unittest.TestCase):
    """The four kept neighbor profiles must render exactly what they document."""

    def test_pod_profiles_are_guaranteed_one_per_socket(self):
        expected = {
            # profile: (stream workers, working-set size, exclusive CPUs per pod)
            "pod-a": ("4", "32M", "5"),
            "pod-b": ("24", "196M", "12"),
            "pod-c": ("24", "512M", "24"),
        }
        for profile, (workers, l3_size, cpus) in expected.items():
            with self.subTest(profile=profile):
                cfg = load_config("pinned", [], profile)
                cfg["NODE"] = cfg["LAB_DEFAULT_NODE"]
                pods = noisy_neighbor_pods(cfg)
                # Two Pods, so one can land on each socket.
                self.assertEqual(len(pods), 2)
                self.assertEqual(
                    [pod["metadata"]["name"] for pod in pods],
                    ["mxl-noisy-neighbor-1", "mxl-noisy-neighbor-2"],
                )
                container = pods[0]["spec"]["containers"][0]
                args = container["args"]
                self.assertEqual(args[args.index("--stream") + 1], workers)
                self.assertEqual(args[args.index("--stream-l3-size") + 1], l3_size)
                # Guaranteed: integer CPU request equal to the limit, so the
                # kubelet hands out exclusive cores and the neighbor cannot
                # steal FFmpeg's CPU time - only its cache and bandwidth.
                self.assertEqual(container["resources"]["requests"]["cpu"], cpus)
                self.assertEqual(container["resources"]["limits"]["cpu"], cpus)
                self.assertEqual(
                    container["resources"]["requests"]["memory"],
                    container["resources"]["limits"]["memory"],
                )

    def test_pod_placement_follows_the_spread_switch(self):
        cfg = load_config("pinned", [], "pod-a")
        cfg["NODE"] = cfg["LAB_DEFAULT_NODE"]
        # Default: both neighbors are pinned to the measured worker, so which
        # socket each one lands on is the only free variable.
        default_spec = noisy_neighbor_pods(cfg)[0]["spec"]
        self.assertEqual(default_spec["nodeSelector"], {"kubernetes.io/hostname": cfg["NODE"]})
        self.assertNotIn("affinity", default_spec)

        # Opt in and the node selector is replaced by hostname anti-affinity
        # restricted to LAB_WORKERS: one neighbor per worker instead.
        cfg["NOISY_NEIGHBOR_SPREAD_BY_NODE"] = "1"
        spread_spec = noisy_neighbor_pods(cfg)[0]["spec"]
        self.assertNotIn("nodeSelector", spread_spec)
        anti_affinity = spread_spec["affinity"]["podAntiAffinity"][
            "requiredDuringSchedulingIgnoredDuringExecution"
        ][0]
        self.assertEqual(anti_affinity["topologyKey"], "kubernetes.io/hostname")
        self.assertEqual(
            spread_spec["affinity"]["nodeAffinity"][
                "requiredDuringSchedulingIgnoredDuringExecution"
            ]["nodeSelectorTerms"][0]["matchExpressions"][0]["values"],
            [node.strip() for node in cfg["LAB_WORKERS"].split(",") if node.strip()],
        )

    def test_pod_profiles_are_unprivileged(self):
        cfg = load_config("baseline", [], "pod-a")
        cfg["NODE"] = cfg["LAB_DEFAULT_NODE"]
        pod = noisy_neighbor_pod(cfg)
        container = pod["spec"]["containers"][0]
        self.assertEqual(container["workingDir"], "/tmp")
        self.assertTrue(pod["spec"]["securityContext"]["runAsNonRoot"])
        self.assertFalse(container["securityContext"]["allowPrivilegeEscalation"])
        self.assertEqual(container["securityContext"]["capabilities"]["drop"], ["ALL"])
        self.assertTrue(container["securityContext"]["readOnlyRootFilesystem"])

    def test_host_profile_is_not_a_pod(self):
        cfg = load_config("pinned", [], "host-a")
        self.assertEqual(cfg["NOISY_NEIGHBOR_SCOPE"], "host")
        # Host-scoped noise is started over SSH, not scheduled; asking for a Pod
        # must fail loudly rather than quietly render a different experiment.
        with self.assertRaisesRegex(ValueError, "NOISY_NEIGHBOR_SCOPE=pod"):
            noisy_neighbor_pods(cfg)

    def test_broad_stressor_selection_is_rejected(self):
        cfg = load_config("baseline", [], "pod-a")
        cfg["NODE"] = cfg["LAB_DEFAULT_NODE"]
        cfg["NOISY_NEIGHBOR_ARGS"] = "--all 1"
        with self.assertRaisesRegex(ValueError, "explicit bounded stressors"):
            noisy_neighbor_pod(cfg)


class PlacementTests(unittest.TestCase):
    """One test per kept scenario: baseline, numa-pool, pinned."""

    def test_baseline_leaves_placement_to_the_scheduler(self):
        cfg = load_config("baseline", ["STREAMS=4"])
        self.assertEqual(cfg["PLACEMENT"], "free")
        plan = plan_cpu_sets(cfg)
        self.assertEqual(len(plan), 4)
        for item in plan:
            # No cpuset and no socket choice: Burstable Pods, kernel decides.
            self.assertEqual(item["decoder"], [])
            self.assertEqual(item["encoder"], [])

    def test_numa_pool_alternates_sockets(self):
        cfg = load_config("numa-pool", [
            "STREAMS=4", "LAB_CORES_PER_SOCKET=64",
            "LAB_CPU_NUMBERING=alternating", "LAB_RESERVED_CPUS=0-3",
        ])
        plan = plan_cpu_sets(cfg)
        self.assertEqual([row["socket"] for row in plan], [0, 1, 0, 1])
        # Whole socket minus the kubelet-reserved CPUs, shared by the pool.
        self.assertEqual(len(plan[0]["encoder"]), 62)

    def test_contiguous_cpu_numbering_keeps_socket_pools_separate(self):
        cfg = load_config("numa-pool", [
            "LAB_CORES_PER_SOCKET=32",
            "LAB_CPU_NUMBERING=contiguous",
            "LAB_RESERVED_CPUS=0-1,60-61",
        ])
        pools = cpu_pools(cfg)
        self.assertEqual(pools[0], list(range(2, 32)))
        self.assertEqual(pools[1], list(range(32, 60)) + [62, 63])

    def test_pinned_delegates_cpu_ids_to_the_kubelet(self):
        cfg = load_config("pinned", [
            "STREAMS=24", "DEC_CORES=1", "ENC_CORES=3",
            "LAB_CORES_PER_SOCKET=64", "LAB_CPU_NUMBERING=alternating",
            "LAB_RESERVED_CPUS=0-3",
        ])
        plan = plan_cpu_sets(cfg)
        for item in plan:
            self.assertEqual(item["socket"], "kubelet")
            self.assertEqual(item["decoder"], [])
            self.assertEqual(item["encoder"], [])

        cfg["NODE"] = cfg["LAB_DEFAULT_NODE"]
        cfg["INPUT_FILE"] = cfg["LAB_INPUT_1080P"]
        encoder = pod_spec(cfg, plan[0], "encoder")
        main, exporter = encoder["spec"]["containers"]
        # Guaranteed and integer: this is what makes the CPUs exclusive.
        self.assertEqual(main["resources"]["requests"]["cpu"], "3")
        self.assertEqual(main["resources"]["limits"]["cpu"], "3")
        self.assertEqual(exporter["resources"]["requests"], exporter["resources"]["limits"])
        # No taskset: the kubelet owns the cpuset, not the container command.
        self.assertNotIn("taskset", main["args"][0])

    def test_pinned_checks_node_capacity(self):
        # 128 CPUs - 4 reserved = 124 allocatable; 6 per stream fits 20 streams.
        reference_topology = [
            "LAB_CORES_PER_SOCKET=64", "LAB_CPU_NUMBERING=alternating",
            "LAB_RESERVED_CPUS=0-3",
        ]
        cfg = load_config("pinned", ["STREAMS=20", *reference_topology])
        self.assertEqual(len(plan_cpu_sets(cfg)), 20)

        cfg = load_config("pinned", ["STREAMS=21", *reference_topology])
        with self.assertRaisesRegex(ValueError, r"only 124 cores \(124 CPUs\) are allocatable; at most 20 streams"):
            plan_cpu_sets(cfg)

        cfg = load_config("pinned", [
            "STREAMS=10", "LAB_CORES_PER_SOCKET=32",
            "LAB_CPU_NUMBERING=contiguous", "LAB_RESERVED_CPUS=0-1,60-61",
        ])
        with self.assertRaisesRegex(ValueError, r"only 60 cores \(60 CPUs\) are allocatable; at most 9 streams"):
            plan_cpu_sets(cfg)

    def test_pinned_allows_more_encoder_threads_than_cpus(self):
        # 15 x264 threads on 5 exclusive CPUs is deliberate: threads are cheap,
        # slices bound the parallelism, and the cpuset bounds the CPU time.
        cfg = load_config("pinned", ["ENC_CORES=6", "ENC_THREADS=15", "SLICES=2"])
        plan = plan_cpu_sets(cfg)
        self.assertEqual(plan[0]["encoder"], [])
        self.assertEqual(cfg["ENC_THREADS"], "15")

    def test_sliced_threads_require_enough_slices(self):
        cfg = load_config(
            "pinned", ["ENC_CORES=5", "ENC_THREADS=5", "SLICES=2", "SLICED_THREADS=1"]
        )
        with self.assertRaisesRegex(ValueError, "SLICES >= ENC_THREADS"):
            plan_cpu_sets(cfg)

    def test_frame_threads_allow_fewer_slices_than_threads(self):
        cfg = load_config(
            "pinned", ["ENC_CORES=5", "ENC_THREADS=5", "SLICES=2", "SLICED_THREADS=0"]
        )
        self.assertEqual(plan_cpu_sets(cfg)[0]["encoder"], [])


class HyperThreadingTests(unittest.TestCase):
    """SMT changes the price of a core, not how many cores a stream needs."""

    # Same physical machine as PlacementTests: 2 x 64 cores. With SMT on it
    # reports 256 CPUs, and the kubelet reservation covers whole cores.
    SMT_TOPOLOGY = [
        "LAB_CORES_PER_SOCKET=64", "LAB_CPU_NUMBERING=alternating",
        "LAB_THREADS_PER_CORE=2", "LAB_RESERVED_CPUS=0-3,128-131",
    ]

    def test_a_core_request_becomes_a_two_thread_cpu_request(self):
        cfg = load_config("pinned", ["DEC_CORES=1", "ENC_CORES=5", *self.SMT_TOPOLOGY])
        cfg["NODE"] = cfg["LAB_DEFAULT_NODE"]
        cfg["INPUT_FILE"] = cfg["LAB_INPUT_1080P"]
        plan = plan_cpu_sets(cfg)
        decoder = pod_spec(cfg, plan[0], "decoder")["spec"]["containers"][0]
        encoder = pod_spec(cfg, plan[0], "encoder")["spec"]["containers"][0]
        # 1 and 5 physical cores, so 2 and 10 CPUs - and both are even, which is
        # what full-pcpus-only requires or admission fails with SMTAlignmentError.
        self.assertEqual(decoder["resources"]["requests"]["cpu"], "2")
        self.assertEqual(decoder["resources"]["limits"]["cpu"], "2")
        self.assertEqual(encoder["resources"]["requests"]["cpu"], "10")
        self.assertEqual(encoder["resources"]["limits"]["cpu"], "10")

    def test_stream_capacity_is_unchanged_by_smt(self):
        # 124 free cores / 6 cores per stream = 20 streams, SMT on or off.
        self.assertEqual(len(plan_cpu_sets(load_config("pinned", ["STREAMS=20", *self.SMT_TOPOLOGY]))), 20)
        cfg = load_config("pinned", ["STREAMS=21", *self.SMT_TOPOLOGY])
        with self.assertRaisesRegex(
            ValueError, r"126 exclusive cores \(252 CPUs at 2 thread\(s\) per core\).*at most 20 streams"
        ):
            plan_cpu_sets(cfg)

    def test_half_reserved_cores_are_refused(self):
        # Reserving 0-3 without their siblings 128-131 silently costs four cores
        # under full-pcpus-only, so say so instead of quietly losing them.
        cfg = load_config("pinned", [
            "STREAMS=20", "LAB_CORES_PER_SOCKET=64",
            "LAB_THREADS_PER_CORE=2", "LAB_RESERVED_CPUS=0-3",
        ])
        with self.assertRaisesRegex(ValueError, r"only part of core\(s\) 0-3"):
            plan_cpu_sets(cfg)
        # Opting out of full-pcpus-only makes thread-granular reservation legal.
        cfg["LAB_FULL_PCPUS_ONLY"] = "false"
        self.assertEqual(len(plan_cpu_sets(cfg)), 20)

    def test_socket_pools_keep_sibling_threads_together(self):
        cfg = load_config("numa-pool", [
            "LAB_CORES_PER_SOCKET=4", "LAB_THREADS_PER_CORE=2",
            "LAB_CPU_NUMBERING=contiguous", "LAB_RESERVED_CPUS=0,8",
        ])
        pools = cpu_pools(cfg)
        self.assertEqual(pools[0], [1, 2, 3, 9, 10, 11])
        self.assertEqual(pools[1], [4, 5, 6, 7, 12, 13, 14, 15])

    def test_guaranteed_neighbor_keeps_its_physical_footprint(self):
        cfg = load_config("pinned", [*self.SMT_TOPOLOGY], "pod-a")
        cfg["NODE"] = cfg["LAB_DEFAULT_NODE"]
        # pod-a asks for 5 cores; on an SMT host that is 10 CPUs, still 5 cores
        # of cache and bandwidth pressure - and no longer an odd request that
        # full-pcpus-only would reject.
        self.assertEqual(cfg["NOISY_NEIGHBOR_CPU_REQUEST"], "5")
        container = noisy_neighbor_pods(cfg)[0]["spec"]["containers"][0]
        self.assertEqual(container["resources"]["requests"]["cpu"], "10")
        self.assertEqual(container["resources"]["limits"]["cpu"], "10")

    def test_burstable_neighbor_millicores_are_untouched(self):
        cfg = load_config("baseline", [*self.SMT_TOPOLOGY], "pod-a")
        cfg["NODE"] = cfg["LAB_DEFAULT_NODE"]
        cfg["NOISY_NEIGHBOR_GUARANTEED"] = "0"
        cfg["NOISY_NEIGHBOR_CPU_REQUEST"] = "500m"
        container = noisy_neighbor_pods(cfg)[0]["spec"]["containers"][0]
        self.assertEqual(container["resources"]["requests"]["cpu"], "500m")


class SidecarTests(unittest.TestCase):
    def test_exporter_sidecar_does_not_reserve_benchmark_cpu(self):
        # The FPS exporter must never compete for a measured core.
        self.assertNotIn("cpu", sidecar("encoder")["resources"]["requests"])


if __name__ == "__main__":
    unittest.main()
