# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

"""One stability run: start the workers, watch them, rotate EBU LIST, judge.

Order of a run:

1. check the host config, make the input files, write ``plan.json`` and the
   files of each worker into ``<run>/<worker>/``;
2. fence the CPUs (``fence.py``) and start MtlManager on the app CPUs;
3. start the receivers, then the senders; after ``SETTLE_S``, move each MTL
   scheduler thread into the isolated partition and each other thread to the
   app CPUs;
4. until the end: scan the logs, find a worker that stopped early, capture
   and judge one stream for EBU LIST after the other, write ``status.json``;
5. stop the workers, remove the fence, write ``summary.json`` and the reports.

The run passes when each worker ran to the end with exit status 0, each
RxTxApp session result is OK, no crash or SHA mismatch is in a log, no
session fps fell below ``logscan.STALL_RATIO`` of its median, and no EBU LIST
verdict is ``fail``. ``summary.json`` lists each reason of a failed run.
"""

from __future__ import annotations

import dataclasses
import json
import logging
import os
import signal
import subprocess
import time
from typing import Optional

from . import media
from .config import HostConfig, cpu_list
from .ebu import Rotation
from .fence import Fence, restore
from .logscan import LogScanner
from .report import write_reports
from .workload import Plan, Worker

logger = logging.getLogger(__name__)

SETTLE_S = 20.0  # sessions are up and the schedulers run
STOP_GRACE_S = 60.0
FAIL_CLASSES = ("crash", "sha-mismatch", "result-failed")
END_SLACK_S = 90.0  # a worker that stops this long before the end stopped early


@dataclasses.dataclass
class Proc:
    worker: Worker
    popen: subprocess.Popen
    log: str
    start: float
    end: Optional[float] = None
    rc: Optional[int] = None
    signalled: bool = False


class Run:
    def __init__(self, host: HostConfig, plan: Plan, run_dir: str):
        self.host = host
        self.plan = plan
        self.dir = run_dir
        self.procs: dict[str, Proc] = {}
        self.fence: Optional[Fence] = None
        self.manager: Optional[subprocess.Popen] = None
        self.verdicts: list = []
        self.notes: list[str] = []
        self.t0 = 0.0
        self.stop_asked = False

    # -- setup -----------------------------------------------------------------

    def _wdir(self, w: Worker) -> str:
        return os.path.join(self.dir, w.name)

    def _log(self, name: str) -> str:
        return os.path.join(self.dir, name, "worker.log")

    def prepare(self) -> None:
        problems = self.host.check()
        if problems:
            raise RuntimeError("host config: " + "; ".join(problems))
        if os.geteuid() != 0:
            raise RuntimeError(
                "run as root: the fence, vfio, tc and netsniff-ng need it"
            )
        os.makedirs(self.dir, exist_ok=True)
        made = media.prepare(self.host)
        if made:
            self.notes.append(f"media made: {', '.join(made)}")
        with open(os.path.join(self.dir, "plan.json"), "w", encoding="utf-8") as f:
            f.write(self.plan.to_json())
        for w in self.plan.workers:
            os.makedirs(self._wdir(w), exist_ok=True)
            for name, text in w.files.items():
                with open(
                    os.path.join(self._wdir(w), name), "w", encoding="utf-8"
                ) as f:
                    f.write(text)
            with open(
                os.path.join(self._wdir(w), "cmd.txt"), "w", encoding="utf-8"
            ) as f:
                f.write(" ".join(w.argv) + "\n")

    def _env(self, w: Worker) -> dict:
        env = dict(os.environ)
        if self.host.ld_library_path:
            env["LD_LIBRARY_PATH"] = (
                self.host.ld_library_path + ":" + env.get("LD_LIBRARY_PATH", "")
            )
        env.update(w.env)
        return env

    def _start_manager(self) -> None:
        if not self.host.mtl_manager:
            return
        if os.path.exists("/var/run/imtl/mtl_manager.sock"):
            self.notes.append("MtlManager was already running; the run uses it")
            return
        log = open(os.path.join(self.dir, "mtl_manager.log"), "wb")
        app = set(cpu_list(self.host.cpus.app)) or None

        def pre():
            os.setsid()
            if app:
                os.sched_setaffinity(0, app)

        self.manager = subprocess.Popen(
            [self.host.mtl_manager],
            stdout=log,
            stderr=subprocess.STDOUT,
            stdin=subprocess.DEVNULL,
            preexec_fn=pre,
        )
        for _ in range(40):
            if os.path.exists("/var/run/imtl/mtl_manager.sock"):
                return
            time.sleep(0.25)
        self.notes.append("MtlManager did not open its socket in 10 s")

    def _start(self, w: Worker) -> None:
        log = self._log(w.name)
        fh = open(log, "wb")
        fence = self.fence

        def pre():
            os.setsid()
            if fence:
                fence.enter()

        p = subprocess.Popen(
            w.argv,
            cwd=self._wdir(w),
            env=self._env(w),
            stdout=fh,
            stderr=subprocess.STDOUT,
            stdin=subprocess.DEVNULL,
            preexec_fn=pre,
        )
        self.procs[w.name] = Proc(w, p, log, time.time())
        logger.info("started %s (pid %d)", w.name, p.pid)

    # -- the loop --------------------------------------------------------------

    def _reap(self) -> None:
        for p in self.procs.values():
            if p.rc is None and p.popen.poll() is not None:
                p.rc = p.popen.returncode
                p.end = time.time()
                left = self.t0 + self.plan.duration_s - p.end
                level = logging.WARNING if left > END_SLACK_S else logging.INFO
                logger.log(
                    level,
                    "%s stopped, status %s, %.0f s before the end",
                    p.worker.name,
                    p.rc,
                    max(0, left),
                )

    def _status(self, scanner: LogScanner) -> None:
        status = {
            "t": round(time.time() - self.t0, 1),
            "duration_s": self.plan.duration_s,
            "workers": {
                n: {"pid": p.popen.pid, "rc": p.rc} for n, p in self.procs.items()
            },
            "ebu": [dataclasses.asdict(v) for v in self.verdicts[-20:]],
            "errors": {f"{w}/{c}": n for (w, c), n in scanner.counts.items()},
            "anomalies": len(scanner.anomalies),
        }
        tmp = os.path.join(self.dir, "status.json.tmp")
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(status, f, indent=1)
        os.replace(tmp, os.path.join(self.dir, "status.json"))

    def execute(self) -> dict:
        self.prepare()
        signal.signal(signal.SIGTERM, self._ask_stop)
        signal.signal(signal.SIGINT, self._ask_stop)
        scanner = LogScanner(
            [w.name for w in self.plan.workers], self._log, self.host.max_error_lines
        )
        rotation = Rotation(self.host, self.plan.streams, self.dir)
        if not rotation.enabled:
            self.notes.append(
                "EBU LIST rotation off (no capture interface, no EBU LIST, or no stream)"
            )
        try:
            if self.host.cpus.fence:
                self.fence = Fence(self.host.cpus)
                self.notes += self.fence.apply()
            self._start_manager()
            self.t0 = time.time()
            rx_first = sorted(
                self.plan.workers, key=lambda w: 0 if w.receives and not w.sends else 1
            )
            for w in rx_first:
                self._start(w)
                time.sleep(2.0)
            self._loop(scanner, rotation)
        finally:
            self._stop_all()
            if self.fence:
                self.notes += restore()
            if self.manager:
                self.manager.send_signal(signal.SIGINT)
                try:
                    self.manager.wait(10)
                except subprocess.TimeoutExpired:
                    self.manager.kill()
        scanner.scan()
        summary = self._summary(scanner)
        write_reports(self.dir, self.plan, summary, scanner.timeline(), self.verdicts)
        return summary

    def _ask_stop(self, signum, frame) -> None:
        logger.warning("signal %d: stopping the run", signum)
        self.stop_asked = True

    def _loop(self, scanner: LogScanner, rotation: Rotation) -> None:
        end = self.t0 + self.plan.duration_s
        isolated = False
        next_capture = self.t0 + SETTLE_S + 10
        next_scan = 0.0
        ebu_log = open(os.path.join(self.dir, "ebu.jsonl"), "a", encoding="utf-8")
        while time.time() < end and not self.stop_asked:
            now = time.time()
            self._reap()
            if all(p.rc is not None for p in self.procs.values()):
                logger.warning("each worker stopped; the run ends early")
                break
            if self.fence and now - self.t0 > SETTLE_S:
                live = [p.popen.pid for p in self.procs.values() if p.rc is None]
                n_lc, n_app = self.fence.move_threads(live)
                if not isolated:
                    state = self.fence.isolate()
                    isolated = True
                    self.notes.append(
                        f"lcore partition: {state}; threads moved: {n_lc} lc, {n_app} app"
                    )
                    foreign = self.fence.foreign_tasks()
                    if foreign:
                        self.notes.append(
                            f"{len(foreign)} other tasks on lcore CPUs: {foreign[:5]}"
                        )
            if now >= next_scan:
                scanner.scan()
                self._status(scanner)
                next_scan = now + self.host.scan_interval_s
            if rotation.enabled and now >= next_capture and end - now > 30:
                v = rotation.next()
                if v:
                    self.verdicts.append(v)
                    ebu_log.write(json.dumps(dataclasses.asdict(v)) + "\n")
                    ebu_log.flush()
                    logger.info(
                        "EBU %s: %s %s", v.sid, v.verdict, "; ".join(v.reasons)[:200]
                    )
                next_capture = time.time() + self.host.capture.gap_s
                continue
            time.sleep(1.0)
        ebu_log.close()

    def _stop_all(self) -> None:
        deadline = self.t0 + self.plan.duration_s + 30 if self.t0 else time.time()
        # RxTxApp stops by itself at --test_time; give it that chance first.
        while time.time() < deadline and not self.stop_asked:
            self._reap()
            if all(p.rc is not None for p in self.procs.values()):
                return
            if all(
                p.rc is not None or p.worker.kind != "rxtxapp"
                for p in self.procs.values()
            ):
                break
            time.sleep(1.0)
        for p in self.procs.values():
            if p.rc is None:
                p.signalled = True
                sig = getattr(signal, f"SIG{p.worker.stop_signal}", signal.SIGINT)
                try:
                    os.killpg(p.popen.pid, sig)
                except ProcessLookupError:
                    pass
        stop_by = time.time() + STOP_GRACE_S
        while time.time() < stop_by and any(
            p.popen.poll() is None for p in self.procs.values()
        ):
            time.sleep(0.5)
        for p in self.procs.values():
            if p.popen.poll() is None:
                logger.warning(
                    "%s did not stop in %.0f s: SIGKILL", p.worker.name, STOP_GRACE_S
                )
                os.killpg(p.popen.pid, signal.SIGKILL)
                p.popen.wait()
        self._reap()

    # -- the verdict -------------------------------------------------------------

    def _summary(self, scanner: LogScanner) -> dict:
        logs = scanner.summary()
        reasons = []
        end = self.t0 + self.plan.duration_s
        workers = {}
        for n, p in self.procs.items():
            early = (
                p.end is not None
                and not p.signalled
                and end - p.end > END_SLACK_S
                and not self.stop_asked
            )
            ok_rc = p.rc == 0 or (p.signalled and p.rc in (0, -2, 130, 255, -15, 143))
            workers[n] = {
                "kind": p.worker.kind,
                "rc": p.rc,
                "ran_s": round((p.end or time.time()) - p.start, 1),
                "stopped_early": early,
                "signalled": p.signalled,
                "note": p.worker.note,
            }
            if early:
                reasons.append(
                    f"{n} stopped {end - p.end:.0f} s before the end (status {p.rc})"
                )
            elif not ok_rc:
                reasons.append(f"{n} exit status {p.rc}")
        for key, r in logs["results"].items():
            if r != "OK":
                reasons.append(f"{key}: {r}")
        for key, n in logs["error_counts"].items():
            if key.rsplit("/", 1)[1] in FAIL_CLASSES:
                reasons.append(f"{key}: {n} lines")
        if logs["anomalies"]:
            reasons.append(
                f"{len(logs['anomalies'])} fps drops below {int(100 * 0.98)} % of the median"
            )
        ebu = {}
        for v in self.verdicts:
            e = ebu.setdefault(
                v.sid,
                {
                    "pass": 0,
                    "pass-wide": 0,
                    "fail": 0,
                    "capture-loss": 0,
                    "no-stream": 0,
                    "error": 0,
                },
            )
            e[v.verdict] += 1
        for sid, e in ebu.items():
            if e["fail"] or e["no-stream"]:
                reasons.append(
                    f"EBU LIST {sid}: {e['fail']} fail, {e['no-stream']} no stream"
                )
        never = [s.sid for s in self.plan.streams if s.ebu and s.sid not in ebu]
        return {
            "profile": self.plan.profile,
            "duration_s": self.plan.duration_s,
            "ran_s": round(time.time() - self.t0, 1) if self.t0 else 0,
            "stopped_by_signal": self.stop_asked,
            "passed": not reasons and not self.stop_asked,
            "reasons": reasons,
            "notes": self.notes,
            "workers": workers,
            "ebu": ebu,
            "ebu_never_captured": never,
            "logs": logs,
        }
