#!/usr/bin/env python3
"""Wait design v2 copy of the formal review's reuse model: adds always_drain (no stale-signal
counters); the v2 design is guard_all=True, always_drain=True.

Retire + reuse (Y1) racing a WAKE_NOW of the old session (a carried flush bit, or any
WAKE_NOW on a library loop, which holds neither the in-flight counter nor the guard).

Threads:
  F  the old session's flush: WAKE_NOW(o, lane 0) on an entry whose H(0) is set (a DP call
     armed lane 0 after the CLOSING wake, as D1 allows while it holds the in-flight count);
  C  the control plane: RETIRE (X1-X3) then CREATE on the reused entry (Y1, field by field,
     closed = 0 last), then GET_WAIT_HANDLE for the new session (G2, G3);
  E  the new session's event loop (starts once C is done): sweeps its mask with DP misses,
     then sleeps on the new eventfd;
  P  the new session's completer (starts once C is done): one unit on lane 1, E1-E5 + a
     direct WAKE_NOW.
Checks: LOST (E asleep, the new fd at 0, a unit ready), UAF (a write to the old, closed fd).
Variant guard_all=True: the whole WAKE_NOW runs inside guard/closed (the proposed fix).
"""

AH, PEND, SIG, DONE, SEEN, FDID, CNT, OLDCLOSED, CLOSED, GUARD, R0, R1, HMASK, UAF = (
    range(14)
)
NG = 14


def upd(g, *p):
    g = list(g)
    for i in range(0, len(p), 2):
        g[p[i]] = p[i + 1]
    return tuple(g)


def write_fd(g):
    if g[FDID] == 1:  # the old descriptor
        return upd(g, UAF, 1) if g[OLDCLOSED] else g
    if g[FDID] == 2:
        return upd(g, CNT, min(g[CNT] + 1, 3))
    return g  # write(-1): EBADF


def wake(g, pc, F, hf, cfg, tag):
    """WAKE_NOW + POST + SIGNAL with guard. Returns (g2, pc2, hf2); pc2 'RET' at the end."""
    ga = cfg.get("guard_all")
    if pc == "G0":
        if ga:
            return [(upd(g, GUARD, g[GUARD] + 1), "G1", hf)]
        return [(g, "W1", hf)]
    if pc == "G1":
        return [(g, "W1" if g[CLOSED] == 0 else "GX", hf)]
    if pc == "GX":
        return [(upd(g, GUARD, g[GUARD] - 1), "RET", 0)]
    if pc == "W1":
        return [(g, "W2" if g[AH] & F else "END0", 0)]
    if pc == "W2":
        old = g[AH]
        return [(upd(g, AH, old & ~F), "W5", old & F)]
    if pc == "W5":
        if not hf:
            return [(g, "END0", 0)]
        old = g[PEND]
        return [(upd(g, PEND, old | hf), "END0" if old else "S1", hf)]
    if pc == "S1":
        return [(upd(g, GUARD, g[GUARD] + 1), "S2", hf)]
    if pc == "S2":
        return [
            (
                g,
                ("S4" if cfg.get("always_drain") else "S3") if g[CLOSED] == 0 else "S6",
                hf,
            )
        ]
    if pc == "S3":
        return [(upd(g, SIG, g[SIG] + 1), "S4", hf)]
    if pc == "S4":
        return [(write_fd(g), "S6" if cfg.get("always_drain") else "S5", hf)]
    if pc == "S5":
        return [(upd(g, DONE, g[DONE] + 1), "S6", hf)]
    if pc == "S6":
        return [(upd(g, GUARD, g[GUARD] - 1), "END0", hf)]
    if pc == "END0":
        if ga:
            return [(upd(g, GUARD, g[GUARD] - 1), "RET", 0)]
        return [(g, "RET", 0)]
    raise RuntimeError(pc)


def f_step(g, th, cfg):
    _, pc, hf = th
    if pc == "RET":
        return []
    return [(g2, ("F", p2, h2)) for g2, p2, h2 in wake(g, pc, 1, hf, cfg, "F")]


CSEQ = [
    "X1",
    "X2",
    "X3",
    "Y_armed",
    "Y_pend",
    "Y_sig",
    "Y_fd",
    "Y_mask",
    "Y_closed",
    "G2",
    "G3",
    "END",
]


def c_step(g, th, cfg):
    _, pc = th
    if pc == "END":
        return []
    nxt = CSEQ[CSEQ.index(pc) + 1]
    if pc == "X1":
        return [(upd(g, CLOSED, 1), ("C", nxt))]
    if pc == "X2":
        return [(g, ("C", nxt))] if g[GUARD] == 0 else []
    if pc == "X3":
        return [(upd(g, OLDCLOSED, 1), ("C", nxt))]
    if pc == "Y_armed":
        return [(upd(g, AH, 0), ("C", nxt))]
    if pc == "Y_pend":
        return [(upd(g, PEND, 0), ("C", nxt))]
    if pc == "Y_sig":
        return [(upd(g, SIG, 0, DONE, 0, SEEN, 0), ("C", nxt))]
    if pc == "Y_fd":
        return [(upd(g, FDID, 0), ("C", nxt))]
    if pc == "Y_mask":
        return [(upd(g, HMASK, 0), ("C", nxt))]
    if pc == "Y_closed":
        return [(upd(g, CLOSED, 0), ("C", nxt))]
    if pc == "G2":
        return [(upd(g, FDID, 2, CNT, 0), ("C", nxt))]
    if pc == "G3":
        return [(upd(g, HMASK, cfg["mask"]), ("C", nxt))]
    raise RuntimeError(pc)


def e_step(g, th, cfg, cdone):
    _, pc, li, took, p, d, sweeps = th
    if not cdone:
        return []
    lanes = [ln for ln in (0, 1) if cfg["mask"] & (1 << ln)]
    if pc == "EP":
        if g[CNT] == 0 or sweeps >= 3:
            return []
        return [(g, ("E", "S", 0, 0, 0, 0, sweeps + 1))]
    ln = lanes[li]
    b = 1 << ln
    r = R0 + ln

    def T(pc2, g2=g, li=li, took=took, p=p, d=d):
        return (g2, ("E", pc2, li, took, p, d, sweeps))

    if pc == "S":
        if g[r]:
            return [T("S", g2=upd(g, r, g[r] - 1))]
        return [T("K1")]
    if pc == "K1":
        return [T("K2" if g[PEND] & b else "K3", p=g[PEND], took=0)]
    if pc == "K2":
        old = g[PEND]
        return [T("K3", g2=upd(g, PEND, old & ~b), took=old & b, p=old & ~b)]
    if pc == "K3":
        if p:
            return [T("H1")]
        if cfg.get("always_drain"):
            return [T("R1")]
        return [T("K3b", d=g[SIG])]
    if pc == "K3b":
        return [T("R1" if d != g[SEEN] else "H1")]
    if pc == "R1":
        g2 = upd(g, GUARD, g[GUARD] + 1)
        return [T("R2" if g[CLOSED] == 0 else "R7", g2=g2)]
    if pc == "R2":
        return [T("R3", d=g[DONE])]
    if pc == "R3":
        return [
            T(
                "R6" if cfg.get("always_drain") else "R4",
                g2=upd(g, CNT, 0) if g[FDID] == 2 else g,
            )
        ]
    if pc == "R4":
        return [T("R6", g2=upd(g, SEEN, d))]
    if pc == "R6":
        return [T(("RS4" if cfg.get("always_drain") else "RS3") if g[PEND] else "R7")]
    if pc == "RS3":
        return [T("RS4", g2=upd(g, SIG, g[SIG] + 1))]
    if pc == "RS4":
        return [T("R7" if cfg.get("always_drain") else "RS5", g2=write_fd(g))]
    if pc == "RS5":
        return [T("R7", g2=upd(g, DONE, g[DONE] + 1))]
    if pc == "R7":
        return [T("H1", g2=upd(g, GUARD, g[GUARD] - 1))]
    if pc == "H1":
        return [T("H3" if g[AH] & b else "H2")]
    if pc == "H2":
        return [T("H3", g2=upd(g, AH, g[AH] | b))]
    if pc == "H3":
        if g[r]:
            g2 = upd(g, r, g[r] - 1)
            if took:
                return [T("RP", g2=g2)]
            return [T("S", g2=g2)]
        if li + 1 == len(lanes):
            return [(g, ("E", "EP", 0, 0, 0, 0, sweeps))]
        return [T("S", li=li + 1, took=0, p=0, d=0)]
    if pc == "RP":  # D8 (re-post, simplified to one step + SIGNAL as a write)
        old = g[PEND]
        g2 = upd(g, PEND, old | b)
        if not old:
            g2 = write_fd(upd(g2, SIG, g2[SIG] + 1, DONE, g2[DONE] + 1))
        return [T("S", g2=g2, took=0)]
    raise RuntimeError(pc)


def p_step(g, th, cfg, cdone):
    _, pc, hf = th
    if not cdone or pc == "RET":
        return []
    if pc == "PUB":
        return [(upd(g, R1, g[R1] + 1), ("P", "E3", 0))]
    if pc == "E3":
        return [(g, ("P", "G0" if g[AH] & 2 else "RET", 0))]
    res = []
    for g2, p2, h2 in wake(g, pc, 2, hf, cfg, "P"):
        res.append((g2, ("P", p2, h2)))
    return res


def explore(cfg):
    g0 = [0] * NG
    g0[AH] = 1  # the old session's H(0), armed after the CLOSING wake
    g0[FDID] = 1  # the old descriptor
    start = (
        tuple(g0),
        (("F", "G0", 0), ("C", "X1"), ("E", "S", 0, 0, 0, 0, 0), ("P", "PUB", 0)),
    )
    parent = {start: None}
    stack = [start]
    viol = []
    while stack:
        g, ths = stack.pop()
        cdone = ths[1][1] == "END"
        succ = []
        for i, th in enumerate(ths):
            k = th[0]
            res = (
                f_step(g, th, cfg)
                if k == "F"
                else (
                    c_step(g, th, cfg)
                    if k == "C"
                    else (
                        e_step(g, th, cfg, cdone)
                        if k == "E"
                        else p_step(g, th, cfg, cdone)
                    )
                )
            )
            for g2, t2 in res:
                n = list(ths)
                n[i] = t2
                succ.append(((g2, tuple(n)), i))
        if not succ:
            e = ths[2]
            if g[UAF]:
                viol.append(("UAF", (g, ths)))
            if e[1] == "EP" and g[CNT] == 0 and (g[R0] or g[R1]):
                viol.append(("LOST(E)", (g, ths)))
            continue
        for s, i in succ:
            if s not in parent:
                parent[s] = ((g, ths), i)
                stack.append(s)
    return len(parent), viol, parent


NAMES = "AH PEND SIG DONE SEEN FDID CNT OLDCLOSED CLOSED GUARD R0 R1 HMASK UAF".split()


def show(parent, st):
    path = []
    while parent[st]:
        p, i = parent[st]
        path.append((i, p, st))
        st = p
    for i, p, s in reversed(path):
        diff = [f"{n}={s[0][j]}" for j, n in enumerate(NAMES) if s[0][j] != p[0][j]]
        print(
            f"     T{i} {s[1][i][0]}:{str(p[1][i][1]):>8s}->{str(s[1][i][1]):8s} {' '.join(diff)}"
        )


if __name__ == "__main__":
    for name, cfg in [
        ("v1: mask {1}", dict(mask=2)),
        ("v1: mask {0,1}", dict(mask=3)),
        ("always-drain, guard as v1: mask {1}", dict(mask=2, always_drain=True)),
        (
            "v2 (always-drain, WAKE_NOW in the guard): mask {1}",
            dict(mask=2, guard_all=True, always_drain=True),
        ),
        ("v2: mask {0,1}", dict(mask=3, guard_all=True, always_drain=True)),
    ]:
        n, v, parent = explore(cfg)
        print(f"{name:48s} states={n:7d} viol={len(v)} {sorted({k for k, _ in v})}")
        if v:
            # shortest
            best = min(
                v,
                key=lambda kv: (
                    len(
                        list(
                            iter(
                                lambda s=[kv[1]]: (
                                    s.__setitem__(0, parent[s[0]][0])
                                    if parent[s[0]]
                                    else None
                                )
                                or s[0],
                                None,
                            )
                        )
                    )
                    if False
                    else 0
                ),
            )
            k, st = v[0]
            print(f"   {k}:")
            show(parent, st)
            print("   final:", st)
