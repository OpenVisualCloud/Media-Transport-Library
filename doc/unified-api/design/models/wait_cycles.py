#!/usr/bin/env python3
"""Livelock check independent of both busy-loop detectors of the port (the 4-returns counter
and the solo-cycle run): the event loops' idle counters are zeroed so a busy loop becomes a
cycle, the full reachable graph is built, and every non-trivial strongly connected component
is reported with the threads that move inside it. Producers, wakers, closers and spurious
futex returns are finite, so any cycle is a run in which no unit is ever consumed.
A cycle is FAIR-LIVELOCK if no thread that is enabled in every state of the cycle is left out
of it (a weakly fair scheduler can stay in it forever).
Usage: wait_cycles.py CASE... [cfg=1 ...], CASE an index into wait_model2.py's CASES."""

import importlib.util
import os
import sys
import time

sys.setrecursionlimit(10000)
spec = importlib.util.spec_from_file_location(
    "m", os.path.join(os.path.dirname(os.path.abspath(__file__)), "wait_model2.py")
)
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)


def norm(th):
    if th[0] in ("E", "Q"):
        return th[:6] + (0,) + th[7:]
    return th


def succs(g, threads, cfg):
    quiet = m.is_quiet(threads)
    out = []
    for i, th in enumerate(threads):
        k = th[0]
        res = (
            m.p_step(g, th, cfg)
            if k == "P"
            else (
                m.a_step(g, th, cfg)
                if k == "A"
                else (
                    m.ic_step(g, th, cfg)
                    if k in ("I", "C")
                    else (
                        m.j_step(g, th, cfg)
                        if k == "J"
                        else (
                            [(g2, t2, 0) for g2, t2 in m.e_step(g, th, cfg, quiet)]
                            if k == "E"
                            else (
                                m.x_step(g, th, cfg)
                                if k == "X"
                                else (
                                    m.w_step(g, th, cfg)
                                    if k == "W"
                                    else m.q_step(g, th, cfg, quiet)
                                )
                            )
                        )
                    )
                )
            )
        )
        for g2, th2, wake in res:
            nts = list(threads)
            if wake:
                for j, o in enumerate(nts):
                    if o[0] == "W" and o[1] == "WB" and (wake & m.bit(o[2])):
                        nts[j] = ("W", "W6", o[2], 0, o[4], o[5], None)
            nts[i] = norm(th2)
            out.append(((g2, tuple(nts)), i))
    return out


def run(idx, cfg_extra=None):
    name, expect, cfg = m.CASES[idx]
    if cfg_extra:
        cfg = dict(cfg, **cfg_extra)
    cfg = dict(cfg, solo_spin=False)
    g0 = [0] * m.NG
    g0[m.EN] = 1
    start = (tuple(g0), tuple(norm(t) for t in cfg["threads"]))
    ids = {start: 0}
    nodes = [start]
    adj = []
    t0 = time.time()
    k = 0
    while k < len(nodes):
        g, ths = nodes[k]
        lst = []
        for st, i in succs(g, ths, cfg):
            if st[1][i][1] == "SPIN":
                continue
            j = ids.get(st)
            if j is None:
                j = len(nodes)
                ids[st] = j
                nodes.append(st)
            lst.append((j, i))
        adj.append(lst)
        k += 1
    n = len(nodes)
    # iterative Tarjan
    index = [-1] * n
    low = [0] * n
    onst = [False] * n
    st = []
    comp = [-1] * n
    c = 0
    counter = 0
    for root in range(n):
        if index[root] != -1:
            continue
        work = [(root, 0)]
        index[root] = low[root] = counter
        counter += 1
        st.append(root)
        onst[root] = True
        while work:
            v, pi = work[-1]
            if pi < len(adj[v]):
                work[-1] = (v, pi + 1)
                w = adj[v][pi][0]
                if index[w] == -1:
                    index[w] = low[w] = counter
                    counter += 1
                    st.append(w)
                    onst[w] = True
                    work.append((w, 0))
                elif onst[w]:
                    low[v] = min(low[v], index[w])
            else:
                work.pop()
                if work:
                    u = work[-1][0]
                    low[u] = min(low[u], low[v])
                if low[v] == index[v]:
                    while True:
                        w = st.pop()
                        onst[w] = False
                        comp[w] = c
                        if w == v:
                            break
                    c += 1
    sizes = {}
    for v in range(n):
        sizes.setdefault(comp[v], []).append(v)
    report = []
    for cid, vs in sizes.items():
        movers = set()
        selfloop = False
        for v in vs:
            for w, i in adj[v]:
                if comp[w] == cid:
                    movers.add(i)
                    if w == v:
                        selfloop = True
        if len(vs) == 1 and not selfloop:
            continue
        # threads enabled in every state of the component
        always = None
        for v in vs:
            en = {i for _, i in adj[v]}
            always = en if always is None else (always & en)
        fair = (always or set()) <= movers
        kinds = sorted({nodes[vs[0]][1][i][0] + str(i) for i in movers})
        report.append((len(vs), kinds, fair, nodes[vs[0]]))
    fairc = [r for r in report if r[2]]
    print(
        f"{idx:2d} {name:66s} states={n:9d} sccs={len(report):4d} fair_livelock={len(fairc):3d} "
        f"{time.time()-t0:.0f}s",
        flush=True,
    )
    for r in sorted(report, key=lambda r: -r[0])[:4]:
        print(
            f"     scc size={r[0]} movers={r[1]} fair={r[2]} e.g. g={r[3][0]} th={r[3][1]}"
        )


if __name__ == "__main__":
    extra = {}
    for a in [a for a in sys.argv[1:] if "=" in a]:
        kk, vv = a.split("=")
        extra[kk] = vv == "1"
    for a in [a for a in sys.argv[1:] if "=" not in a]:
        run(int(a), extra)
