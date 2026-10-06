#!/usr/bin/env python3
# Wait design v2: a copy of the formal review's flush model; the v2 flush is this model's k = 1 (count bound).
"""Bounded flush (FLUSH F1-F8) with several objects on one library loop, budget = one wake
per flush (the worst case k = 1), WT waiters arming while marks are carried, and the
scheduler sleep check (TASKLET_SLEEP: the loop sleeps when no handler had work and the flush
returned 0). Variant no_return1: the flush's return is not added to `pending`.
Check: LOST = the loop asleep (or finished) while a waiter sleeps with its unit ready.
Also reports the largest number of iterations between a mark and its wake (fairness).
"""

N = 3


def run(cfg):
    script = cfg["script"]  # per iteration: tuple of objects completing a unit
    # global: R[N], WT[N], WSEQ[N], FIRED[N], BITS (int), cursor
    # loop thread: (pc, it, i, n, F, ws, ret, age...) kept simple
    g0 = (
        tuple([0] * N),
        tuple([0] * N),
        tuple([0] * N),
        tuple([0] * N),
        0,
        0,
        tuple([-1] * N),
    )
    L0 = (
        "H",
        0,
        0,
        0,
        0,
        0,
        0,
    )  # pc, iteration, idx, n(wakes this flush), F, ws, handlers-had-work
    Ws = tuple(("W0", o, 0) for o in range(N))
    start = (g0, L0, Ws)
    seen = {start}
    stack = [start]
    lost = []
    maxlag = 0

    def setv(t, i, v):
        t = list(t)
        t[i] = v
        return tuple(t)

    while stack:
        st = stack.pop()
        g, L, ws = st
        R, WT, WSEQ, FIRED, BITS, cur, MARKIT = g
        succ = []
        pc, it, idx, n, F, wsn, work = L
        # ---- the loop thread
        if pc == "H":  # handlers of iteration `it`
            comps = script[it] if it < len(script) else ()
            if idx < len(comps):
                o = comps[idx]
                R2 = setv(R, o, R[o] + 1)
                FI, B, MK = FIRED, BITS, MARKIT
                if WT[o] > 0:  # E3/E4, M1, M2
                    if FI[o] == 0:
                        B |= 1 << o
                        MK = setv(MK, o, it)
                    FI = setv(FI, o, FI[o] | 1)
                succ.append(
                    ((R2, WT, WSEQ, FI, B, cur, MK), ("H", it, idx + 1, 0, 0, 0, 1), ws)
                )
            else:
                succ.append((g, ("F", it, cur, 0, 0, 0, work), ws))
        elif (
            pc == "F"
        ):  # F3: next set bit at or after idx (wrapping once from the cursor)
            order = [(cur + j) % N for j in range(N)]
            rest = [
                o
                for o in order[order.index(idx) if idx in order else 0 :]
                if BITS & (1 << o)
            ]
            # rest: set bits from idx to the end of the wrap
            started = order.index(idx)
            rest = [o for o in order[started:] if BITS & (1 << o)]
            if not rest:
                succ.append(
                    (
                        (R, WT, WSEQ, FIRED, BITS, 0, MARKIT),
                        ("Z", it, 0, 0, 0, 0, work),
                        ws,
                    )
                )
            else:
                o = rest[0]
                if n > 0:  # F4: budget spent -> carry
                    succ.append(
                        (
                            (R, WT, WSEQ, FIRED, BITS, o, MARKIT),
                            ("Z1", it, 0, 0, 0, 0, work),
                            ws,
                        )
                    )
                else:  # F5, F6
                    f = FIRED[o]
                    lag = it - MARKIT[o]
                    maxlag = max(maxlag, lag)
                    g2 = (
                        R,
                        WT,
                        WSEQ,
                        setv(FIRED, o, 0),
                        BITS & ~(1 << o),
                        cur,
                        setv(MARKIT, o, -1),
                    )
                    nxt = (
                        ("W1", it, o, n, f, 0, work)
                        if f
                        else (
                            "F",
                            it,
                            order[(started + 1) % N] if started + 1 < N else o,
                            n,
                            0,
                            0,
                            work,
                        )
                    )
                    if not f and started + 1 >= N:
                        nxt = ("F", it, o, n, 0, 0, work)
                    succ.append((g2, nxt, ws))
        elif pc == "W1":  # WAKE_NOW: snapshot
            succ.append((g, ("W3", it, idx, n, F, 1 if WT[idx] else 0, work), ws))
        elif pc == "W3":
            if wsn:
                g2 = (R, WT, setv(WSEQ, idx, WSEQ[idx] + 1), FIRED, BITS, cur, MARKIT)
                succ.append((g2, ("W3b", it, idx, n, F, wsn, work), ws))
            else:
                succ.append((g, ("FN", it, idx, n + 1, 0, 0, work), ws))
        elif pc == "W3b":  # FUTEX_WAKE
            nws = tuple(
                ("W6", w[1], 0) if (w[0] == "WB" and w[1] == idx) else w for w in ws
            )
            succ.append((g, ("FN", it, idx, n + 1, 0, 0, work), nws))
        elif pc == "FN":  # continue the scan after idx
            order = [(cur + j) % N for j in range(N)]
            p = order.index(idx)
            if p + 1 >= N:
                succ.append(
                    (
                        (R, WT, WSEQ, FIRED, BITS, 0, MARKIT),
                        ("Z", it, 0, 0, 0, 0, work),
                        ws,
                    )
                )
            else:
                succ.append((g, ("F", it, order[p + 1], n, 0, 0, work), ws))
        elif pc in ("Z", "Z1"):  # end of iteration: sleep check
            ret = 1 if (pc == "Z1" and not cfg.get("no_return1")) else 0
            more = it + 1 < len(script)
            if work == 0 and ret == 0 and not more:
                succ.append((g, ("SLEEP", it, 0, 0, 0, 0, 0), ws))
            elif it + 1 < 2 * len(script) + 2 * N:
                succ.append((g, ("H", it + 1, 0, 0, 0, 0, 0), ws))
            else:
                succ.append((g, ("SLEEP", it, 0, 0, 0, 0, 0), ws))
        # ---- waiters (WT protocol on their own object)
        for j, w in enumerate(ws):
            wpc, o, v = w
            nw = None
            g2 = g
            if wpc == "W0":
                if R[o] > 0:
                    g2 = (setv(R, o, R[o] - 1), WT, WSEQ, FIRED, BITS, cur, MARKIT)
                    nw = ("DONE", o, 0)
                else:
                    g2 = (R, setv(WT, o, WT[o] + 1), WSEQ, FIRED, BITS, cur, MARKIT)
                    nw = ("W2", o, 0)
            elif wpc == "W2":
                nw = ("W3", o, WSEQ[o])
            elif wpc == "W3":
                nw = ("W6", o, 0) if R[o] > 0 else ("W4", o, v)
            elif wpc == "W4":
                nw = ("W6", o, 0) if WSEQ[o] != v else ("WB", o, v)
            elif wpc == "W6":
                g2 = (R, setv(WT, o, WT[o] - 1), WSEQ, FIRED, BITS, cur, MARKIT)
                nw = ("W0", o, 0)
            if nw:
                succ.append((g2, L, setv(ws, j, nw)))
        if not succ:
            if any(w[0] == "WB" and R[w[1]] > 0 for w in ws):
                lost.append(st)
            continue
        for s in succ:
            if s not in seen:
                seen.add(s)
                stack.append(s)
    return len(seen), lost, maxlag


if __name__ == "__main__":
    script = [(0, 1, 2)]
    for name, cfg in [
        ("budget k=1, burst of 3 then idle, design", dict(script=script)),
        (
            "budget k=1, burst of 3 then idle, return ignored",
            dict(script=script, no_return1=True),
        ),
        ("budget k=1, two bursts, design", dict(script=[(0, 1, 2), (), (1, 2)])),
        (
            "budget k=1, re-marks of object 0 every iteration",
            dict(script=[(0, 1, 2), (0,), (0,), (0,), ()]),
        ),
    ]:
        n, lost, lag = run(cfg)
        print(
            f"{name:58s} states={n:8d} LOST={len(lost)} max mark-to-wake lag={lag} iterations"
        )
        if lost:
            print("   e.g.", lost[0])
