from __future__ import annotations

from dataclasses import dataclass

from .common import expand_cpu_spec

# The worker's CPU topology, as configured in config/lab.env.
#
# Two independent axes decide which socket a logical CPU belongs to, and both
# have to be modelled or a cpuset lands on the wrong socket without any error:
#
#   numbering  - how the BIOS enumerates *cores*: "alternating" (socket 0 owns
#                the even IDs) or "contiguous" (socket 0 owns the first block).
#   threads    - how many hardware threads each core exposes. With SMT enabled
#                the sibling of core N is CPU N + physical_cores, so the ID
#                range is twice as wide as the core count and the naive
#                "socket = cpu // cores_per_socket" gives socket 2 and 3 on a
#                two-socket machine.
#
# Deriving everything from a core id instead of a raw CPU id makes the SMT case
# structural rather than accidental: siblings normalise onto the same core, so
# they always land in the same socket pool.


@dataclass(frozen=True)
class Topology:
    sockets: int
    cores_per_socket: int
    threads_per_core: int
    numbering: str
    socket0_even: bool
    reserved: frozenset[int]

    @property
    def physical_cores(self) -> int:
        return self.sockets * self.cores_per_socket

    @property
    def logical_cpus(self) -> int:
        return self.physical_cores * self.threads_per_core


def from_config(cfg: dict[str, str]) -> Topology:
    sockets = int(cfg["LAB_SOCKET_COUNT"])
    if sockets != 2:
        raise ValueError("current parity planner requires exactly 2 sockets")
    threads = int(cfg.get("LAB_THREADS_PER_CORE", "1"))
    if threads < 1:
        raise ValueError("LAB_THREADS_PER_CORE must be >= 1")
    numbering = cfg.get("LAB_CPU_NUMBERING", "alternating")
    if numbering not in ("alternating", "contiguous"):
        raise ValueError("LAB_CPU_NUMBERING must be alternating or contiguous")
    return Topology(
        sockets=sockets,
        cores_per_socket=int(cfg["LAB_CORES_PER_SOCKET"]),
        threads_per_core=threads,
        numbering=numbering,
        socket0_even=cfg.get("LAB_SOCKET0_PARITY", "even") == "even",
        reserved=frozenset(expand_cpu_spec(cfg.get("LAB_RESERVED_CPUS", ""))),
    )


def full_pcpus_only(cfg: dict[str, str]) -> bool:
    return cfg.get("LAB_FULL_PCPUS_ONLY", "true").lower() in ("1", "true", "yes")


def core_id(topology: Topology, cpu: int) -> int:
    """The physical core a logical CPU belongs to."""
    return cpu % topology.physical_cores


def siblings(topology: Topology, cpu: int) -> list[int]:
    """Every logical CPU sharing a physical core with this one, including itself."""
    core = core_id(topology, cpu)
    return [core + index * topology.physical_cores for index in range(topology.threads_per_core)]


def socket_id(topology: Topology, cpu: int) -> int:
    core = core_id(topology, cpu)
    if topology.numbering == "contiguous":
        return core // topology.cores_per_socket
    return 0 if ((core % 2 == 0) == topology.socket0_even) else 1


def cpu_pools(topology: Topology) -> list[list[int]]:
    """Allocatable logical CPUs per socket, reserved ones removed."""
    pools: list[list[int]] = [[] for _ in range(topology.sockets)]
    for cpu in range(topology.logical_cpus):
        if cpu in topology.reserved:
            continue
        pools[socket_id(topology, cpu)].append(cpu)
    return pools


def core_pools(topology: Topology) -> list[list[list[int]]]:
    """Per socket, the sibling groups of every *fully* free physical core.

    A core with one sibling reserved cannot be handed out exclusively under
    full-pcpus-only, so it does not count towards capacity.
    """
    pools: list[list[list[int]]] = [[] for _ in range(topology.sockets)]
    for core in range(topology.physical_cores):
        group = siblings(topology, core)
        if any(cpu in topology.reserved for cpu in group):
            continue
        pools[socket_id(topology, core)].append(group)
    return pools


def cpu_request_for_cores(topology: Topology, cores: int) -> str:
    """The Kubernetes CPU request that buys `cores` whole physical cores.

    full-pcpus-only admits a Guaranteed container only when its CPU request is a
    multiple of threads-per-core, so a request is always cores x threads.
    """
    return str(cores * topology.threads_per_core)


def partially_reserved_cores(topology: Topology) -> list[int]:
    """Cores with some, but not all, siblings in LAB_RESERVED_CPUS.

    Cannot happen without SMT, where every core is its own sibling group.
    """
    partial = []
    for core in range(topology.physical_cores):
        group = siblings(topology, core)
        reserved = sum(cpu in topology.reserved for cpu in group)
        if 0 < reserved < len(group):
            partial.append(core)
    return partial
