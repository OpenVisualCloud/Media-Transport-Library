#!/usr/bin/env python3
"""Mutation check of wait_model.py: each mutant of the v2 handle path must be killed by at
least one case (a violation). Usage: wait_mutants.py"""

import importlib.util
import os
import sys

spec = importlib.util.spec_from_file_location(
    "m", os.path.join(os.path.dirname(os.path.abspath(__file__)), "wait_model.py")
)
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
MUTANTS = {
    "mut_drain_only_if_took": "no drain without a taken bit (N1)",
    "mut_no_resignal": "no re-signal after the read (S-i)",
    "mut_no_arm": "no H arm on a miss (I4)",
    "mut_no_recheck": "no re-check after the arm (I4)",
    "repost_off": "no re-post (N3)",
}
cases = [i for i, c in enumerate(m.CASES) if c[1] == "d"]
bad = 0
for mut, what in MUTANTS.items():
    killed = []
    for i in cases:
        name, variant, ets, threads, ops = m.CASES[i]
        cfg = {
            "variant": "d",
            "e_targets": ets,
            "threads": threads,
            "prod_ops": ops,
            "repost": mut != "repost_off",
            "always_drain": True,
            mut: True,
        }
        n, v = m.explore(cfg)
        if v:
            killed.append((i, sorted({k for k, _ in v})))
    print(f"{mut:24s} {what:36s} killed by cases {killed}", flush=True)
    bad += 0 if killed else 1
sys.exit(1 if bad else 0)
