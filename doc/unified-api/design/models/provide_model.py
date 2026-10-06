#!/usr/bin/env python3
"""Exhaustive interleaving model of the mtl_rx_provide hand-back (D-152, contract.md §9.11).

Every atomic action of the C11 implementation (seq_cst atomics) is one step. All interleavings
of the actors are explored by a DFS over hashable states (memoised: equal states are explored
once, and the count is of distinct interleavings).

Actors
  provider k - mtl_rx_provide of one destination into its own free entry k
  handback   - one or two hand-backs in sequence, each one of
               stop    : acc = 0, T = G+1, sweep, acc = 1 (STOPPED accepts), P = G+1
               close   : acc = 0, T = G+1, sweep, P = G+1 (acc stays 0)
               discard : (RUNNING, acc stays 1) T = G+1, sweep, P = G+1
               detach  : (STOPPED, acc stays 1) the same steps
               noop    : a stop in STOPPED (acc stays 1) the same steps
  tasklet    - a unit takes a queued destination (QUEUED -> ASSIGNED), at any time

Entry states: ('E',) | ('Q', g) | ('A',) | ('R', g)

Ledger rule checked at every hand-back's return (after it published P) and at the end:
  provide returned g, no unit took it:  g < P  => entry RETURNED (MTL never writes it again)
                                         g >= P => entry QUEUED (still held)
  provide returned -ESHUTDOWN          => entry EMPTY (nothing taken)
  after close                           => no entry QUEUED (nothing held by a closed session)
The variant "v2" is v2's order (sweep against the next value, then store T), which must fail
(review N1).
"""

import sys
from functools import lru_cache

ESHUTDOWN = -108


def make_scenario(kinds, n_prov, with_tasklet, variant):
    hb = []
    for kind in kinds:
        if kind in ("stop", "close"):
            hb.append(("store_acc", 0))
        if variant == "v3":
            hb.append(("bump_T", None))  # T = G + 1 first
            hb += [("sweep", i) for i in range(n_prov)]
        else:
            hb.append(("load_T", None))
            hb += [("sweep", i) for i in range(n_prov)]
            hb.append(("store_T", None))
        if kind == "stop":
            hb.append(("store_acc", 1))
        hb.append(("publish", None))
        hb.append(("check", None))
    hb = tuple(hb)
    closed = "close" in kinds
    bad = set()

    def violation(st, final):
        T, P, acc, ents, provs, hbpc, loc, tpc = st
        for k, (pc, g, a, t, res) in enumerate(provs):
            if pc != 9:
                continue
            e = ents[k]
            if res == ESHUTDOWN:
                if e != ("E",):
                    return "provide %d returned ESHUTDOWN, entry %s" % (k, e)
                continue
            if e == ("A",):
                continue
            if res < P and e[0] != "R":
                return "provide %d returned g=%d < P=%d, entry %s still held" % (
                    k,
                    res,
                    P,
                    e,
                )
            if res >= P and e[0] != "Q":
                return "provide %d returned g=%d >= P=%d, entry %s" % (k, res, P, e)
        if final and closed:
            for e in ents:
                if e[0] == "Q":
                    return "entry %s held after close" % (e,)
        return None

    @lru_cache(maxsize=None)
    def dfs(st):
        T, P, acc, ents, provs, hbpc, loc, tpc = st
        moves = []
        for k, (pc, g, a, t, res) in enumerate(provs):
            if pc == 9:
                continue
            e2, p2 = list(ents), list(provs)
            if pc == 0:  # state accepts?
                p2[k] = (1, g, a, t, None) if acc else (9, g, a, t, ESHUTDOWN)
            elif pc == 1:  # g = T
                p2[k] = (2, T, a, t, None)
            elif pc == 2:  # CAS EMPTY -> QUEUED(g)
                e2[k] = ("Q", g)
                p2[k] = (3, g, a, t, None)
            elif pc == 3:  # a = accepts
                p2[k] = (4, g, acc, t, None)
            elif pc == 4:  # t = T, decide
                if not a:
                    p2[k] = (5, g, a, T, None)
                elif T == g:
                    p2[k] = (9, g, a, T, g)
                else:
                    p2[k] = (6, g, a, T, None)
            elif pc == 5:  # withdraw: CAS QUEUED(g) -> EMPTY
                if e2[k] == ("Q", g):
                    e2[k] = ("E",)
                    p2[k] = (9, g, a, t, ESHUTDOWN)
                else:
                    p2[k] = (9, g, a, t, g)
            elif pc == 6:  # re-tag: CAS QUEUED(g) -> QUEUED(t)
                if e2[k] == ("Q", g):
                    e2[k] = ("Q", t)
                    p2[k] = (3, t, a, t, None)
                else:
                    p2[k] = (9, g, a, t, g)
            moves.append((T, P, acc, tuple(e2), tuple(p2), hbpc, loc, tpc))
        if hbpc < len(hb):
            op, arg = hb[hbpc]
            T2, P2, acc2, e2, loc2 = T, P, acc, list(ents), loc
            if op == "store_acc":
                acc2 = arg
            elif op == "bump_T":
                T2 = T + 1
                loc2 = T2
            elif op == "load_T":
                loc2 = T + 1
            elif op == "store_T":
                T2 = loc
            elif op == "sweep":
                e = e2[arg]
                if e[0] == "Q" and e[1] < loc:
                    e2[arg] = ("R", e[1])
            elif op == "publish":
                P2 = loc
            elif op == "check":
                v = violation(st, False)
                if v:
                    bad.add(v)
            moves.append((T2, P2, acc2, tuple(e2), provs, hbpc + 1, loc2, tpc))
        if with_tasklet and tpc < n_prov:
            e2 = list(ents)
            if e2[tpc][0] == "Q":
                e2[tpc] = ("A",)
            moves.append((T, P, acc, tuple(e2), provs, hbpc, loc, tpc + 1))
        if not moves:
            v = violation(st, True)
            if v:
                bad.add(v)
            return 1
        return sum(dfs(m) for m in moves)

    init = (
        1,
        1,
        1,
        tuple([("E",)] * n_prov),
        tuple([(0, 0, 0, 0, None)] * n_prov),
        0,
        0,
        0,
    )
    return dfs(init), sorted(bad)


def main():
    kinds_list = (
        ("discard",),
        ("detach",),
        ("noop",),
        ("stop",),
        ("close",),
        ("discard", "discard"),
        ("stop", "detach"),
        ("noop", "close"),
        ("discard", "stop"),
    )
    result = {}
    for variant in ("v3", "v2"):
        total = nbad = 0
        example = None
        for kinds in kinds_list:
            for n_prov in (1, 2, 3):
                for with_tasklet in (False, True):
                    n, bad = make_scenario(kinds, n_prov, with_tasklet, variant)
                    total += n
                    if bad:
                        nbad += 1
                        example = example or (kinds, n_prov, with_tasklet, bad[0])
        result[variant] = nbad
        print(
            "%s: %d scenarios, %d interleavings, %d scenarios with a violation"
            % (variant, len(kinds_list) * 6, total, nbad)
        )
        if example:
            print("   e.g.", example)
    M = 2**31 - 1

    def nxt(x):
        return 1 if x == M else x + 1

    def before(g, G):
        d = (G - g) % M
        return 0 < d < 2**30

    for g in (1, 2, M - 1, M):
        G = nxt(g)
        assert before(g, G) and not before(G, g) and not before(g, g)
    print("31-bit wrap compare ok")
    return 1 if result["v3"] or not result["v2"] else 0


if __name__ == "__main__":
    sys.exit(main())
