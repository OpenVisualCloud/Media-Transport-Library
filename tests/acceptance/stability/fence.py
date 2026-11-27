# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

"""The CPU fence of a stability run, as in the performance suite, for many processes.

``tests/tools/isolate/isolate.sh`` puts one command into one isolated
partition. In an isolated partition no load balancing runs, so each thread
that is not pinned stays on the CPU where it starts. That is correct for one
RxTxApp, but not for an FFmpeg JPEG XS encoder or a GStreamer pipeline, whose
worker threads would all run on one CPU. So this fence has three sets:

* housekeeping: each slice of systemd, the IRQs, the unbound workqueues;
* app: a threaded cgroup ``<CG>/app`` that the kernel balances, for each
  thread of each worker that is not an MTL scheduler;
* lcores: a threaded cgroup ``<CG>/lc``, an isolated partition after the
  workers start, for each MTL scheduler thread.

Each worker starts in ``<CG>`` (EAL pins its lcores there), and
``move_threads()`` then puts each thread into ``lc`` or ``app``. Each change
goes into a restore file first; ``restore()`` (also ``python -m stability
unfence``) puts back what the file holds, after a crash too.
"""

from __future__ import annotations

import glob
import json
import logging
import os
import subprocess
from typing import Iterable

from .config import CpuConfig, cpu_list, cpu_text

logger = logging.getLogger(__name__)

CG = "/sys/fs/cgroup/mtl-stability"
SLICES = ("user.slice", "system.slice", "init.scope", "machine.slice")
RESTORE = "/run/mtl-stability-fence.json"
# The names of the MTL scheduler threads: MTL names its own, DPDK names an
# EAL worker lcore that MTL launches a scheduler on.
LCORE_THREAD_PREFIXES = ("mtl_sch", "dpdk-worker", "lcore-worker", "rte-worker")
IDLE_MAX_LATENCY_US = 10
EAL_MAIN_CPU = 0  # mtl_init_params.main_lcore when the app does not set it


def _read(path: str) -> str:
    try:
        with open(path, encoding="ascii") as f:
            return f.read().strip()
    except OSError:
        return ""


def _write(path: str, value: str) -> bool:
    try:
        with open(path, "w", encoding="ascii") as f:
            f.write(f"{value}\n")
        return True
    except OSError as e:
        logger.debug("write %s=%s: %s", path, value, e)
        return False


def _mask(cpus: Iterable[int]) -> str:
    """CPU list -> the comma-separated 32-bit hex groups of a kernel cpumask."""
    value = 0
    for c in cpus:
        value |= 1 << c
    text = f"{value:x}"
    text = text.zfill((len(text) + 7) // 8 * 8)
    return ",".join(text[i : i + 8] for i in range(0, len(text), 8))


class Fence:
    def __init__(self, cpus: CpuConfig):
        self.cpus = cpus
        self.house = cpu_list(cpus.housekeeping)
        self.app = cpu_list(cpus.app)
        self.lc = cpu_list(cpus.lcores)
        self.saved: dict = {}

    # -- setup -------------------------------------------------------------

    def _save(self, key: str, value) -> None:
        if key not in self.saved:
            self.saved[key] = value
            with open(RESTORE, "w", encoding="utf-8") as f:
                json.dump(self.saved, f)

    def apply(self) -> list[str]:
        """Fence the host; give each step that did not work, in words."""
        if os.path.exists(RESTORE):
            logger.warning("an old fence is still on; restoring it first")
            restore()
        notes: list[str] = []
        house = cpu_text(self.house)
        for s in SLICES:
            if not os.path.isdir(f"/sys/fs/cgroup/{s}"):
                continue
            # systemd drops AllowedCPUs on revert but leaves the cgroup file
            # as it is, so the file itself is what restore() puts back.
            self._save(f"slice:{s}", _read(f"/sys/fs/cgroup/{s}/cpuset.cpus"))
            r = subprocess.run(
                ["systemctl", "set-property", "--runtime", s, f"AllowedCPUs={house}"],
                capture_output=True,
                text=True,
            )
            if r.returncode:
                notes.append(f"{s}: AllowedCPUs not set ({r.stderr.strip()})")
        for path, value in (
            ("/proc/sys/kernel/numa_balancing", "0"),
            ("/proc/sys/vm/stat_interval", "120"),
            ("/sys/devices/virtual/workqueue/cpumask", _mask(self.house)),
            ("/proc/irq/default_smp_affinity", _mask(self.house)),
        ):
            old = _read(path)
            if not old:
                continue
            self._save(f"file:{path}", old)
            if not _write(path, value):
                notes.append(f"{path}: not written")
        moved = failed = 0
        irqs = {}
        for d in glob.glob("/proc/irq/[0-9]*"):
            old = _read(f"{d}/smp_affinity_list")
            if _write(f"{d}/smp_affinity_list", house):
                irqs[d] = old
                moved += 1
            else:
                failed += 1
        self._save("irqs", irqs)
        notes.append(
            f"IRQs moved to the housekeeping CPUs: {moved}, not movable: {failed}"
        )
        idle_off = []
        for c in self.app + self.lc:
            for st in glob.glob(f"/sys/devices/system/cpu/cpu{c}/cpuidle/state*"):
                lat = _read(f"{st}/latency")
                if (
                    lat.isdigit()
                    and int(lat) > IDLE_MAX_LATENCY_US
                    and _read(f"{st}/disable") == "0"
                ):
                    if _write(f"{st}/disable", "1"):
                        idle_off.append(st)
        self._save("idle_off", idle_off)
        gov = {}
        for c in self.lc:
            p = f"/sys/devices/system/cpu/cpu{c}/cpufreq/scaling_governor"
            old = _read(p)
            if old and old != "performance" and _write(p, "performance"):
                gov[p] = old
        self._save("governor", gov)
        if not gov and not glob.glob("/sys/devices/system/cpu/cpu*/cpufreq"):
            notes.append(
                "no cpufreq (a VM): the governor stays as the hypervisor sets it"
            )
        self._cgroups()
        return notes

    def _cgroups(self) -> None:
        self._save("cgroup", CG)
        os.makedirs(CG, exist_ok=True)
        _write("/sys/fs/cgroup/cgroup.subtree_control", "+cpuset")
        _write(f"{CG}/cgroup.subtree_control", "+cpuset")
        for sub in ("lc", "app"):
            os.makedirs(f"{CG}/{sub}", exist_ok=True)
            _write(f"{CG}/{sub}/cgroup.type", "threaded")
        mems = _read("/sys/fs/cgroup/cpuset.mems.effective") or "0"
        _write(f"{CG}/cpuset.mems", mems)
        _write(f"{CG}/cpuset.cpus", cpu_text(self.app + self.lc + [EAL_MAIN_CPU]))
        _write(f"{CG}/lc/cpuset.cpus", cpu_text(self.lc))
        _write(f"{CG}/app/cpuset.cpus", cpu_text(self.app))
        for c in (CG, f"{CG}/lc", f"{CG}/app"):
            _write(f"{c}/cgroup.pressure", "0")

    # -- workers -------------------------------------------------------------

    def enter(self, eal_main: bool = False, mask: str = "") -> None:
        """preexec_fn of a worker: into <CG>, on the app and the lcore CPUs.

        With ``--lcores``, MTL gives EAL ``-l <main_lcore>,<lcores>`` and
        main_lcore is 0 when the app does not set it, so EAL pins its main
        thread to CPU 0 (*eal_main*). move_threads() then puts that thread
        on the app CPUs.
        """
        _write(f"{CG}/cgroup.procs", str(os.getpid()))
        if mask:
            # A worker that takes no lcore list (FFmpeg, GStreamer): MTL takes the
            # lowest free CPUs of the mask, so the mask is the CPUs it may take.
            os.sched_setaffinity(0, set(cpu_list(mask)))
            return
        os.sched_setaffinity(
            0, set(self.app + self.lc + ([EAL_MAIN_CPU] if eal_main else []))
        )

    def move_threads(self, pids: Iterable[int]) -> tuple[int, int]:
        """Each MTL scheduler thread to lc, each other thread to app."""
        n_lc = n_app = 0
        for pid in pids:
            for task in glob.glob(f"/proc/{pid}/task/*"):
                tid = os.path.basename(task)
                comm = _read(f"{task}/comm")
                cg = _read(f"{task}/cgroup")
                want = "lc" if comm.startswith(LCORE_THREAD_PREFIXES) else "app"
                if cg.endswith(f"/mtl-stability/{want}"):
                    continue
                try:
                    pinned = os.sched_getaffinity(int(tid))
                except OSError:
                    continue  # the thread ended
                if not _write(f"{CG}/{want}/cgroup.threads", tid):
                    continue
                if want == "lc":
                    n_lc += 1
                    # The move resets the mask to the whole cpuset: pin the
                    # scheduler to its lcore again, as EAL did.
                    if pinned <= set(self.lc):
                        try:
                            os.sched_setaffinity(int(tid), pinned)
                        except OSError as e:
                            logger.warning(
                                "re-pin %s (%s) to %s: %s", tid, comm, pinned, e
                            )
                else:
                    n_app += 1
        return n_lc, n_app

    def isolate(self) -> str:
        # Linux 6.7 and later: a partition needs its CPUs in cpuset.cpus.exclusive
        # of each level, the parent first.
        lc = cpu_text(self.lc)
        if os.path.exists(f"{CG}/cpuset.cpus.exclusive"):
            _write(f"{CG}/cpuset.cpus.exclusive", lc)
            _write(f"{CG}/lc/cpuset.cpus.exclusive", lc)
        _write(f"{CG}/lc/cpuset.cpus.partition", "isolated")
        return _read(f"{CG}/lc/cpuset.cpus.partition")

    def foreign_tasks(self) -> list[str]:
        """Each user task that runs on an lcore CPU and is not in <CG>/lc."""
        lc = set(self.lc)
        out = []
        r = subprocess.run(
            ["ps", "-eLo", "pid,tid,psr,stat,comm", "--no-headers"],
            capture_output=True,
            text=True,
        )
        for line in r.stdout.splitlines():
            parts = line.split(None, 4)
            # A sleeping task shows the CPU it ran on last: only a running one counts.
            if (
                len(parts) < 5
                or not parts[2].isdigit()
                or int(parts[2]) not in lc
                or "R" not in parts[3]
            ):
                continue
            comm = parts[4]
            if comm.startswith(
                (
                    "kworker",
                    "ksoftirqd",
                    "migration",
                    "cpuhp",
                    "rcu",
                    "idle_inject",
                    "irq/",
                    "watchdog",
                )
            ):
                continue
            if comm.startswith(LCORE_THREAD_PREFIXES):
                continue
            out.append(line.strip())
        return out


def restore() -> list[str]:
    """Undo each change of the restore file."""
    if not os.path.exists(RESTORE):
        return ["no fence to restore"]
    with open(RESTORE, encoding="utf-8") as f:
        saved = json.load(f)
    notes = []
    cg = saved.get("cgroup")
    if cg and os.path.isdir(cg):
        _write(f"{cg}/lc/cpuset.cpus.partition", "member")
        for sub in ("lc", "app"):
            for t in _read(f"{cg}/{sub}/cgroup.threads").split():
                _write(f"{cg}/cgroup.threads", t)
        left = _read(f"{cg}/cgroup.procs").split()
        for pid in left:
            _write("/sys/fs/cgroup/cgroup.procs", pid)
        for sub in ("lc", "app", ""):
            try:
                os.rmdir(f"{cg}/{sub}" if sub else cg)
            except OSError as e:
                notes.append(f"rmdir {cg}/{sub}: {e}")
    for key, value in saved.items():
        if key.startswith("slice:"):
            s = key.split(":", 1)[1]
            subprocess.run(["systemctl", "revert", s], capture_output=True)
            path = f"/sys/fs/cgroup/{s}/cpuset.cpus"
            # An empty value (inherit) gives ENOSPC while the slice has tasks:
            # then each online CPU is the same set.
            if not (isinstance(value, str) and value and _write(path, value)):
                if not _write(path, "") and not _write(
                    path, _read("/sys/devices/system/cpu/online")
                ):
                    notes.append(f"{s}: cpuset.cpus not restored")
        elif key.startswith("file:"):
            _write(key.split(":", 1)[1], value)
    for d, old in (saved.get("irqs") or {}).items():
        if old:
            _write(f"{d}/smp_affinity_list", old)
    for st in saved.get("idle_off") or []:
        _write(f"{st}/disable", "0")
    for p, old in (saved.get("governor") or {}).items():
        _write(p, old)
    os.unlink(RESTORE)
    notes.append("fence removed")
    return notes
