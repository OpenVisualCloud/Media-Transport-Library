#!/usr/bin/env python3
"""Wait design v2.1 (on the v2 port): Q6a (WAIT0 re-checks intr after its arm), the interrupt-delivery
LOST check, and the edge rule (a miss that leaves another lane pending signals once) are defaults;
ET mode (cfg et) and MTL_INTR_OFF (cfg intr_off) come from the A2 verification harnesses.
Wait design v2 port of the formal review's extended model. v2 semantics (cfg v2=True, the default here):
  * always-drain: no h_sig/h_done/h_seen steps anywhere;
  * mtl_wait(o, m, 0) that sees an interrupt returns -ECANCELED without touching the handle,
    and the loop stops waiting (state CXL, not a sleeper);
  * a narrow consumer (acquire_slot) re-posts by the lane predicate (M4 fix, cfg m4_fix).
Extended SC interleaving model of engine.md §7.1-§7.3 (it extends wait_model.py).

Differences from the design's model (fidelity fixes):
  * WAKE_NOW is split into W1 (load) and W2 (conditional fetch_and), and W3 uses the
    snapshot `a` of W1/W2 for the WT counts, as the pseudo-code says (the original merges
    W1+W2 and re-reads the live counts at W3);
  * application-thread wakers (A: release/reap as a source) run WAKE_NOW directly, racing
    the tasklet's flush;
  * the session interrupt (I) runs the full WAKE_NOW, including the handle post;
  * a closer (C) stores CLOSING and wakes every armed lane; E/W react to the code;
  * W threads may return spuriously (bounded) and time out (bounded), the timeout running
    the final DP attempt (T12) with the handle discipline;
  * a mtl_wait(o, mask, 0) event loop (Q) with the interrupt semantics of Q1-Q8;
  * EPOLLONESHOT (a shared enable bit re-armed by EPOLL_CTL_MOD after each sweep);
  * failure injection: a killable one-shot consumer, a killable WT waiter, a failing
    eventfd write.
Checks: LOST (terminal state with a sleeper whose target is ready / interrupted / closing),
SPIN (an event loop whose epoll returns 4 times in a row without consuming once the
producer-like threads are done), LEAK (a WT count left at a terminal state).
"""

import sys
import time

(
    R0,
    R1,
    AH,
    WT0,
    WT1,
    PEND,
    FD,
    DONE,
    SEEN,
    WSEQ,
    FIRED,
    INTR,
    IINTR,
    SIG,
    ST,
    EN,
    EDGE,
) = range(17)
NG = 17
ALL = 3


def upd(g, *pairs):
    g = list(g)
    for i in range(0, len(pairs), 2):
        g[pairs[i]] = pairs[i + 1]
    return tuple(g)


def ready(g, t):
    return g[R0 + t]


def wt(g, t):
    return g[WT0 + t]


def bit(t):
    return 1 << t


def wtmask(g):
    return (1 if g[WT0] else 0) | (2 if g[WT1] else 0)


def ready_mask(g):
    return (1 if g[R0] else 0) | (2 if g[R1] else 0)


def fd_write(g, cfg):
    """write(efd, 1); with cfg write_fail the write may fail (EAGAIN) and is ignored."""
    out = [upd(g, FD, min(g[FD] + 1, 3), EDGE, 1)]
    if cfg.get("write_fail"):
        out.append(g)
    return out


# ---------------------------------------------------------------------------------------
# WAKE_NOW(o, F) sub-machine. Local (wpc, F, ws, hf). Returns (g2, wpc2, ws2, hf2, wake).


def wake_steps(g, wpc, F, ws, hf, cfg):
    snap = cfg.get("snapshot", True)
    if wpc == "W1":  # a = load(armed, acquire)
        if not cfg.get("split_w1", True):  # original: unconditional fetch_and
            old = g[AH]
            return [(upd(g, AH, old & ~F), "W3", wtmask(g) & F, old & F, 0)]
        if g[AH] & F:
            return [(g, "W2", wtmask(g) & F, 0, 0)]
        return [(g, "W3", wtmask(g) & F, 0, 0)]
    if wpc == "W2":  # a = fetch_and(armed, ~HB(F))
        old = g[AH]
        return [(upd(g, AH, old & ~F), "W3", wtmask(g) & F, old & F, 0)]
    if wpc == "W3":  # if (a & WC(F)) bump wseq
        w = ws if snap else (wtmask(g) & F)
        if w:
            return [(upd(g, WSEQ, g[WSEQ] + 1), "W3b", ws, hf, 0)]
        return [(g, "W5", ws, hf, 0)]
    if wpc == "W3b":  # FUTEX_WAKE_BITSET(F)
        return [(g, "W5", ws, hf, F)]
    if wpc == "W5":  # POST(hf): P1
        if hf == 0:
            return [(g, "RET", 0, 0, 0)]
        oldp = g[PEND]
        g2 = upd(g, PEND, oldp | hf)
        return [
            (g2, "RET" if oldp else ("S4" if cfg.get("v2", True) else "S3"), ws, hf, 0)
        ]
    if wpc == "S3":
        return [(upd(g, SIG, g[SIG] + 1), "S4", ws, hf, 0)]
    if wpc == "S4":
        return [
            (g2, "RET" if cfg.get("v2", True) else "S5", ws, hf, 0)
            for g2 in fd_write(g, cfg)
        ]
    if wpc == "S5":
        return [(upd(g, DONE, g[DONE] + 1), "RET", 0, 0, 0)]
    raise RuntimeError(wpc)


# ---------------------------------------------------------------------------------------
# P: tasklet producer + flush. ('P', pc, k, F, wpc, ws, hf)


def p_step(g, th, cfg):
    _, pc, k, F, wpc, ws, hf = th
    ops = cfg["prod_ops"]
    if pc == "END":
        return []
    if k == len(ops):
        return [(g, ("P", "END", k, 0, 0, 0, 0), 0)]
    op = ops[k]
    if op[0] == "pub":
        if pc == 0:
            pairs = []
            for t in op[1]:
                pairs += [R0 + t, ready(g, t) + 1]
            return [(upd(g, *pairs), ("P", 1, k, 0, 0, 0, 0), 0)]
        if pc == 1:  # E2-E4 + M1
            f = 0
            for t in op[1]:
                if wt(g, t) > 0 or (g[AH] & bit(t)):
                    f |= bit(t)
            g2 = upd(g, FIRED, g[FIRED] | f) if f else g
            return [(g2, ("P", 0, k + 1, 0, 0, 0, 0), 0)]
    if op[0] == "flush":
        if pc == 0:
            f = g[FIRED]
            if f == 0:
                return [(g, ("P", 0, k + 1, 0, 0, 0, 0), 0)]
            return [(upd(g, FIRED, 0), ("P", "WK", k, f, "W1", 0, 0), 0)]
        if pc == "WK":
            out = []
            for g2, w2, ws2, hf2, wake in wake_steps(g, wpc, F, ws, hf, cfg):
                if w2 == "RET":
                    out.append((g2, ("P", 0, k + 1, 0, 0, 0, 0), wake))
                else:
                    out.append((g2, ("P", "WK", k, F, w2, ws2, hf2), wake))
            return out
    raise RuntimeError(th)


# A: an application-thread wake source (mtl_release / mtl_reap making lane t ready):
# publish, fence, load armed, WAKE_NOW directly. ('A', pc, k, F, wpc, ws, hf)


def a_step(g, th, cfg):
    _, pc, k, F, wpc, ws, hf = th
    ops = cfg["app_ops"]
    if pc == "END":
        return []
    if k == len(ops):
        return [(g, ("A", "END", k, 0, 0, 0, 0), 0)]
    t = ops[k]
    if pc == 0:
        return [(upd(g, R0 + t, ready(g, t) + 1), ("A", 1, k, 0, 0, 0, 0), 0)]
    if pc == 1:
        f = bit(t) if (wt(g, t) > 0 or g[AH] & bit(t)) else 0
        if not f:
            return [(g, ("A", 0, k + 1, 0, 0, 0, 0), 0)]
        return [(g, ("A", "WK", k, f, "W1", 0, 0), 0)]
    if pc == "WK":
        out = []
        for g2, w2, ws2, hf2, wake in wake_steps(g, wpc, F, ws, hf, cfg):
            if w2 == "RET":
                out.append((g2, ("A", 0, k + 1, 0, 0, 0, 0), wake))
            else:
                out.append((g2, ("A", "WK", k, F, w2, ws2, hf2), wake))
        return out
    raise RuntimeError(th)


# I: session interrupt (N7-N9, full WAKE_NOW); C: close (STATE_CHANGE(CLOSING)).
# (kind, pc, F, wpc, ws, hf)


def ic_step(g, th, cfg):
    kind, pc, F, wpc, ws, hf = th
    if pc == "END":
        if kind == "I" and cfg.get("intr_off") and g[INTR] and th[2] != -1:
            return [(upd(g, INTR, 0), (kind, "END", -1, 0, 0, 0), 0)]
        return []
    if pc == 0:
        g2 = upd(g, INTR, 1) if kind == "I" else upd(g, ST, 1)
        return [(g2, (kind, 1, 0, 0, 0, 0), 0)]
    if pc == 1:  # fence; for C: E3/E4 (the armed lanes); I: WAKE_NOW(all lanes)
        if kind == "C":
            f = (g[AH] | wtmask(g)) & ALL
            if not f:
                return [(g, (kind, "END", 0, 0, 0, 0), 0)]
        else:
            f = ALL
        return [(g, (kind, "WK", f, "W1", 0, 0), 0)]
    if pc == "WK":
        out = []
        for g2, w2, ws2, hf2, wake in wake_steps(g, wpc, F, ws, hf, cfg):
            if w2 == "RET":
                out.append((g2, (kind, "END", 0, 0, 0, 0), wake))
            else:
                out.append((g2, (kind, "WK", F, w2, ws2, hf2), wake))
        return out
    raise RuntimeError(th)


# J: the instance interrupt (the walk: WT only, no handle), as the design's model.
def j_step(g, th, cfg):
    kind, pc = th[:2]
    if pc == 0:
        return [(upd(g, IINTR, 1), ("J", 1), 0)]
    if pc == 1:
        if wtmask(g):
            return [(upd(g, WSEQ, g[WSEQ] + 1), ("J", 2), 0)]
        return [(g, ("J", "END"), 0)]
    if pc == 2:
        return [(g, ("J", "END"), ALL)]
    return []


# ---------------------------------------------------------------------------------------
# E: event loop sweeping its targets with DP calls (design D1-D10, K1-K4, R1-R7, H1-H3).
# ('E', pc, ti, now, d, s, idle)


def e_step(g, th, cfg, quiet, ts=None):
    _, pc, ti, now, d, s, idle = th
    narrow = cfg.get("_narrow_now", False)
    if ts is None:
        ts = cfg["e_targets"]
    oneshot = cfg.get("oneshot")
    if pc == "EP" and cfg.get("et"):
        if not g[EDGE]:
            return []
        if g[FD] == 0:
            return [(upd(g, EDGE, 0), th)]
        g = upd(g, EDGE, 0)
    if pc == "EP":
        if g[FD] == 0 or (oneshot and not g[EN]):
            return []
        nidle = idle
        if quiet and all(ready(g, t) == 0 for t in ts):
            nidle = idle + 1
            if nidle >= 4:
                return [(g, ("E", "SPIN", 0, 0, 0, 0, nidle))]
        g2 = upd(g, EN, 0) if oneshot else g
        return [(g2, ("E", "S", 0, 0, 0, 0, nidle))]
    if pc == "MOD":  # EPOLL_CTL_MOD re-arms the one-shot registration
        return [(upd(g, EN, 1), ("E", "EP", 0, 0, 0, 0, idle))]
    if pc in ("SPIN", "GONE"):
        return []
    t = ts[ti]
    b = bit(t)

    def next_target(g2):
        if ti + 1 == len(ts):
            return (g2, ("E", "MOD" if oneshot else "EP", 0, 0, 0, 0, idle))
        return (g2, ("E", "S", ti + 1, 0, 0, 0, idle))

    if pc == "S":  # D1 (state), D2 attempt
        if g[ST]:
            return [(g, ("E", "GONE", 0, 0, 0, 0, 0))]
        if ready(g, t) > 0 and not narrow:
            return [(upd(g, R0 + t, ready(g, t) - 1), ("E", "S", ti, 0, 0, 0, 0))]
        return [(g, ("E", "C1", ti, 0, 0, 0, idle))]
    if pc == "C1":
        p = g[PEND]
        if p & b:
            return [(g, ("E", "C1b", ti, 0, 0, 0, idle))]
        return [(g, ("E", "C2", ti, p, 0, 0, idle))]
    if pc == "C1b":
        old = g[PEND]
        return [(upd(g, PEND, old & ~b), ("E", "C2", ti, old & ~b, 0, 1, idle))]
    if (
        pc == "C2" and now != 0 and cfg.get("edge_rule", True)
    ):  # v2.1: K3, p != 0: signal once
        return [(g2, ("E", "A1", ti, 0, 0, s, idle)) for g2 in fd_write(g, cfg)]
    if pc == "C2":
        if now == 0 and cfg.get("v2", True):  # v2: always drain, no counters
            if cfg.get("mut_drain_only_if_took") and not s:
                return [(g, ("E", "A1", ti, 0, 0, s, idle))]
            return [(g, ("E", "D1", ti, 0, 0, s, idle))]
        if now == 0 and cfg.get(
            "always_drain"
        ):  # simplification: no h_sig/h_done/h_seen
            return [(g, ("E", "C4", ti, 0, 0, s, idle))]
        if now != 0:
            return [(g, ("E", "A1", ti, 0, 0, s, idle))]
        return [(g, ("E", "C3", ti, 0, g[SIG], s, idle))]
    if pc == "C3":
        if d != g[SEEN]:
            return [(g, ("E", "C4", ti, 0, 0, s, idle))]
        return [(g, ("E", "A1", ti, 0, 0, s, idle))]
    if pc == "C4":
        return [(g, ("E", "D1", ti, 0, g[DONE], s, idle))]
    if pc == "D1":
        return [
            (
                upd(g, FD, 0),
                ("E", "D3" if cfg.get("v2", True) else "D2", ti, 0, d, s, idle),
            )
        ]
    if pc == "D2":
        return [(upd(g, SEEN, d), ("E", "D3", ti, 0, 0, s, idle))]
    if pc == "D3":
        if g[PEND] != 0:
            return [
                (g, ("E", "D4b" if cfg.get("v2", True) else "D4", ti, 0, 0, s, idle))
            ]
        return [(g, ("E", "A1", ti, 0, 0, s, idle))]
    if pc == "D4":
        return [(upd(g, SIG, g[SIG] + 1), ("E", "D4b", ti, 0, 0, s, idle))]
    if pc == "D4b":
        return [
            (g2, ("E", "A1" if cfg.get("v2", True) else "D5", ti, 0, 0, s, idle))
            for g2 in fd_write(g, cfg)
        ]
    if pc == "D5":
        return [(upd(g, DONE, g[DONE] + 1), ("E", "A1", ti, 0, 0, s, idle))]
    if pc == "A1":
        if g[AH] & b:
            return [(g, ("E", "A3", ti, 0, 0, s, idle))]
        return [(g, ("E", "A2", ti, 0, 0, s, idle))]
    if pc == "A2":
        return [(upd(g, AH, g[AH] | b), ("E", "A3", ti, 0, 0, s, idle))]
    if pc == "A3":  # fence; D7 re-check (a code counts as "not WAIT")
        if g[ST]:
            if s:
                return [(g, ("E", "RP1", ti, 0, 0, 0, 0))]
            return [(g, ("E", "S", ti, 0, 0, 0, 0))]
        if ready(g, t) > 0 and not (narrow and th[0] == "E" and cfg.get("_narrow_now")):
            g2 = upd(g, R0 + t, ready(g, t) - 1)
            if s and cfg.get("repost", True):
                return [(g2, ("E", "RP1", ti, 0, 0, 0, 0))]
            return [(g2, ("E", "S", ti, 0, 0, 0, 0))]
        if (
            narrow
            and cfg.get("_narrow_now")
            and s
            and ready(g, t) > 0
            and cfg.get("m4_fix")
        ):
            return [(g, ("E", "RP1", ti, 0, 0, 0, 0))]  # D8 by the lane predicate
        return [next_target(g)]
    if pc == "RP1":
        oldp = g[PEND]
        g2 = upd(g, PEND, oldp | b)
        if oldp != 0:
            return [(g2, ("E", "S", ti, 0, 0, 0, 0))]
        return [(g2, ("E", "RP3" if cfg.get("v2", True) else "RP2", ti, 0, 0, 0, 0))]
    if pc == "RP2":
        return [(upd(g, SIG, g[SIG] + 1), ("E", "RP3", ti, 0, 0, 0, 0))]
    if pc == "RP3":
        return [
            (g2, ("E", "S" if cfg.get("v2", True) else "RP4", ti, 0, 0, 0, 0))
            for g2 in fd_write(g, cfg)
        ]
    if pc == "RP4":
        return [(upd(g, DONE, g[DONE] + 1), ("E", "S", ti, 0, 0, 0, 0))]
    raise RuntimeError(("E", th))


def dp_once(g, sub, cfg, t):
    """One DP call on target t through e_step; yields (g2, sub2 or None when the call ended)."""
    out = []
    for g2, t2 in e_step(g, sub, cfg, False, ts=(t,)):
        pf, pt = sub[1], t2[1]
        ended = (
            pt in ("EP", "MOD", "GONE")
            or (pf == "S" and pt == "S")
            or (pf == "A3" and pt == "S")
            or (pf in ("RP1", "RP3", "RP4") and pt == "S")
        )
        out.append((g2, None if ended else t2))
    return out


# X: one DP call on target 0, then exit; optionally killable at any step.
def x_step(g, th, cfg):
    if th[1] in ("DONE", "DEAD"):
        return []
    sub = ("E",) + th[1:]
    out = []
    if cfg.get("x_narrow"):
        cfg = dict(cfg, _narrow_now=True)
    for g2, s2 in dp_once(g, sub, cfg, 0):
        out.append(
            (g2, ("X", "DONE", 0, 0, 0, 0, 0) if s2 is None else ("X",) + s2[1:], 0)
        )
    if cfg.get("kill_x") and th[1] != "S":
        out.append((g, ("X", "DEAD", 0, 0, 0, 0, 0), 0))
    return out


# W: WT call on lane t. ('W', pc, t, v, sp, to, sub)
def w_step(g, th, cfg):
    _, pc, t, v, sp, to, sub = th
    if pc in ("WB", "CANCELED", "GONE", "DONE", "DEAD"):
        out = []
        if pc == "WB":
            if sp > 0:
                out.append((g, ("W", "W6", t, 0, sp - 1, to, None), 0))
            if to > 0:
                out.append((g, ("W", "W6T", t, 0, sp, 0, None), 0))
        return out
    out = []
    if cfg.get("kill_w") and pc in ("W2", "W3", "W4"):
        out.append((g, ("W", "DEAD", t, 0, 0, 0, None), 0))
    if pc == "W0":  # T3, T4
        if g[ST]:
            return [(g, ("W", "GONE", t, 0, 0, 0, None), 0)]
        if g[INTR] or g[IINTR]:
            return [(g, ("W", "CANCELED", t, 0, 0, 0, None), 0)]
        if ready(g, t) > 0:
            nxt = (
                ("W", "DONE", t, 0, 0, 0, None)
                if cfg.get("w_once")
                else ("W", "W0", t, 0, sp, to, None)
            )
            return [(upd(g, R0 + t, ready(g, t) - 1), nxt, 0)]
        return [(g, ("W", "W1", t, 0, sp, to, None), 0)]
    if pc == "W1":  # T5 arm (+ T6 fence)
        return out + [
            (upd(g, WT0 + t, wt(g, t) + 1), ("W", "W2", t, 0, sp, to, None), 0)
        ]
    if pc == "W2":  # T7
        return out + [(g, ("W", "W3", t, g[WSEQ], sp, to, None), 0)]
    if pc == "W3":  # T8, T9
        if ready(g, t) > 0 or g[INTR] or g[IINTR] or g[ST]:
            return out + [(g, ("W", "W6", t, 0, sp, to, None), 0)]
        res = out + [(g, ("W", "W4", t, v, sp, to, None), 0)]
        if to > 0:
            res.append((g, ("W", "W6T", t, 0, sp, 0, None), 0))
        return res
    if pc == "W4":  # T10 compare and queue
        if g[WSEQ] != v:
            return out + [(g, ("W", "W6", t, 0, sp, to, None), 0)]
        return out + [(g, ("W", "WB", t, v, sp, to, None), 0)]
    if pc == "W6":  # T11 disarm, goto T3
        return [(upd(g, WT0 + t, wt(g, t) - 1), ("W", "W0", t, 0, sp, to, None), 0)]
    if (
        pc == "W6T"
    ):  # disarm, then T12 = the final DP attempt (with the handle if in mask)
        g2 = upd(g, WT0 + t, wt(g, t) - 1)
        if cfg.get("w_final_handle"):
            return [(g2, ("W", "F", t, 0, sp, 0, ("E", "S", 0, 0, 0, 0, 0)), 0)]
        if ready(g2, t) > 0:
            g2 = upd(g2, R0 + t, ready(g2, t) - 1)
        return [(g2, ("W", "DONE", t, 0, 0, 0, None), 0)]
    if pc == "F":
        res = []
        for g2, s2 in dp_once(g, sub, cfg, t):
            res.append(
                (
                    g2,
                    (
                        ("W", "DONE", t, 0, 0, 0, None)
                        if s2 is None
                        else ("W", "F", t, 0, sp, 0, s2)
                    ),
                    0,
                )
            )
        return res
    raise RuntimeError(("W", th))


# Q: event loop that sweeps with mtl_wait(o, {0,1}, 0) (Q1-Q8), mask = h_mask = both lanes.
# ('Q', pc, took, p, d, R, idle, canc)
def q_step(g, th, cfg, quiet):
    _, pc, took, p, d, R, idle, canc = th
    M = ALL

    def T(pc2, g2=g, took=took, p=p, d=d, R=R, idle=idle, canc=canc):
        return (g2, ("Q", pc2, took, p, d, R, idle, canc), 0)

    if pc == "CXL" and cfg.get("intr_off") and not (g[INTR] or g[IINTR]):
        return [T("Q1", took=0, p=0, d=0, R=0, idle=0, canc=0)]
    if pc in ("SPIN", "GONE", "CXL"):
        return []
    if pc == "EP":
        if g[FD] == 0:
            return []
        nidle = idle + 1 if quiet else idle
        if nidle >= 4:
            return [T("SPIN", idle=nidle)]
        return [T("Q1", took=0, p=0, d=0, R=0, idle=nidle, canc=0)]
    if pc == "Q1":
        if g[ST]:
            return [T("GONE")]
        if cfg.get("v2", True) and (g[INTR] or g[IINTR]):
            return [
                T("CXL")
            ]  # v2: -ECANCELED, handle untouched; the loop stops waiting
        return [T("Q3", canc=1 if (g[INTR] or g[IINTR]) else 0)]
    if pc == "Q3":
        r = 0 if canc else ready_mask(g)
        if r:  # the application consumes one unit of a ready target (a DP success)
            t = 0 if r & 1 else 1
            return [T("Q1", g2=upd(g, R0 + t, ready(g, t) - 1), idle=0)]
        return [T("K1")]
    if pc == "K1":
        pp = g[PEND]
        if pp & M:
            return [T("K2", took=0, p=pp)]
        return [T("K3", took=0, p=pp)]
    if pc == "K2":
        old = g[PEND]
        return [T("K3", g2=upd(g, PEND, old & ~M), took=old & M, p=old & ~M)]
    if pc == "K3":
        if p and cfg.get("edge_rule", True):
            return [T("H1", g2=g2) for g2 in fd_write(g, cfg)]
        if p:
            return [T("H1")]
        if cfg.get("v2", True):
            return [T("R3")]
        return [T("K3b", d=g[SIG])]
    if pc == "K3b":
        return [T("R2" if d != g[SEEN] else "H1")]
    if pc == "R2":
        return [T("R3", d=g[DONE])]
    if pc == "R3":
        return [T("R6" if cfg.get("v2", True) else "R4", g2=upd(g, FD, 0))]
    if pc == "R4":
        return [T("R6", g2=upd(g, SEEN, d))]
    if pc == "R6":
        return [T(("RS4" if cfg.get("v2", True) else "RS3") if g[PEND] else "H1")]
    if pc == "RS3":
        return [T("RS4", g2=upd(g, SIG, g[SIG] + 1))]
    if pc == "RS4":
        return [
            T("H1" if cfg.get("v2", True) else "RS5", g2=g2) for g2 in fd_write(g, cfg)
        ]
    if pc == "RS5":
        return [T("H1", g2=upd(g, DONE, g[DONE] + 1))]
    if pc == "H1":
        return [T("H3" if (g[AH] & M) == M else "H2")]
    if pc == "H2":
        return [T("H3", g2=upd(g, AH, g[AH] | M))]
    if pc == "H3" and cfg.get("q6a", True) and (g[INTR] or g[IINTR]):  # v2.1 Q6a
        oldp = g[PEND]
        g2 = upd(g, PEND, oldp | took)
        if (
            took == 0 or oldp == 0
        ):  # POST(took) signals on 0 -> non-0; with nothing taken, SIGNAL
            return [T("CXL", g2=g3) for g3 in fd_write(g2, cfg)]
        return [T("CXL", g2=g2)]
    if pc == "H3":  # fence; Q6
        r = ready_mask(g)
        post = took & r
        if cfg.get("q_fix") and canc:
            post = 0
        return [T("Q7" if post else "Q8", R=r)]
    if pc == "Q7":
        pl = took & R
        oldp = g[PEND]
        g2 = upd(g, PEND, oldp | pl)
        return [T("Q8" if oldp else ("PS4" if cfg.get("v2", True) else "PS3"), g2=g2)]
    if pc == "PS3":
        return [T("PS4", g2=upd(g, SIG, g[SIG] + 1))]
    if pc == "PS4":
        return [
            T("Q8" if cfg.get("v2", True) else "PS5", g2=g2) for g2 in fd_write(g, cfg)
        ]
    if pc == "PS5":
        return [T("Q8", g2=upd(g, DONE, g[DONE] + 1))]
    if pc == "Q8":
        if canc:  # -MTL_ECANCELED: the loop goes back to sleep
            return [T("EP")]
        if R:
            return [T("Q1")]
        return [T("EP")]
    raise RuntimeError(("Q", th))


# ---------------------------------------------------------------------------------------

PLIKE = ("P", "A", "I", "J", "C")


def is_quiet(threads):
    for th in threads:
        if th[0] in PLIKE and th[1] != "END":
            return False
    return True


def blocked_lost(g, th, cfg):
    k = th[0]
    if k == "W" and th[1] == "WB":
        if ready(g, th[2]) > 0 or g[INTR] or g[IINTR] or g[ST]:
            return "LOST(W)"
    if k == "E" and th[1] in ("EP", "MOD") and not cfg.get("et"):
        asleep = g[FD] == 0 or (cfg.get("oneshot") and not g[EN])
        if asleep and (any(ready(g, t) > 0 for t in cfg["e_targets"]) or g[ST]):
            return "LOST(E)"
    if k == "Q" and th[1] == "EP" and g[FD] == 0:
        if g[ST] or (ready_mask(g) and not (g[INTR] or g[IINTR])):
            return "LOST(Q)"
        if cfg.get("q_intr_check", True) and (g[INTR] or g[IINTR]):
            return "LOST(Q-intr)"
    if k == "E" and th[1] == "EP" and cfg.get("et") and not g[EDGE]:
        if any(ready(g, t) > 0 for t in cfg["e_targets"]) or g[ST]:
            return "LOST(E-ET)"
    return None


def explore(cfg, limit=None):
    g0 = [0] * NG
    g0[EN] = 1
    g0 = tuple(g0)
    start = (g0, tuple(cfg["threads"]))
    parent = {start: None}
    stack = [start]
    viol = []
    spin4 = [0]
    while stack:
        g, threads = stack.pop()
        quiet = is_quiet(threads)
        succ = []
        for i, th in enumerate(threads):
            k = th[0]
            if k == "P":
                res = p_step(g, th, cfg)
            elif k == "A":
                res = a_step(g, th, cfg)
            elif k in ("I", "C"):
                res = ic_step(g, th, cfg)
            elif k == "J":
                res = j_step(g, th, cfg)
            elif k == "E":
                res = [(g2, t2, 0) for g2, t2 in e_step(g, th, cfg, quiet)]
            elif k == "X":
                res = x_step(g, th, cfg)
            elif k == "W":
                res = w_step(g, th, cfg)
            elif k == "Q":
                res = q_step(g, th, cfg, quiet)
            else:
                raise RuntimeError(k)
            for g2, th2, wake in res:
                nts = list(threads)
                if wake:
                    for j, o in enumerate(nts):
                        if o[0] == "W" and o[1] == "WB" and (wake & bit(o[2])):
                            nts[j] = ("W", "W6", o[2], 0, o[4], o[5], None)
                nts[i] = th2
                st = (g2, tuple(nts))
                if th2[1] == "SPIN":
                    if cfg.get("solo_spin", True):
                        spin4[
                            0
                        ] += 1  # bounded spurious wakes: reported, not a violation
                        continue
                    if st not in parent:
                        parent[st] = ((g, threads), i)
                    viol.append(("SPIN", st))
                    continue
                succ.append((st, i))
        # solo-cycle livelock check: a loop at epoll with the descriptor readable while every other
        # thread is blocked or done; if its own sweep returns to epoll with the descriptor still
        # readable, it spins forever.
        if cfg.get("solo_spin", True):
            movers = {i for (_, i) in succ}
            for i, th in enumerate(threads):
                if (
                    th[0] in ("E", "Q")
                    and th[1] == "EP"
                    and g[FD] > 0
                    and movers <= {i}
                    and not cfg.get("et")
                ):
                    if solo_spins(g, th, cfg):
                        viol.append(("SPIN", (g, threads)))
        if not succ:
            kinds = set()
            for th in threads:
                v = blocked_lost(g, th, cfg)
                if v:
                    kinds.add(v)
            nw = sum(1 for th in threads if th[0] == "W" and th[1] in ("WB",))
            if g[WT0] + g[WT1] != nw:
                kinds.add("LEAK")
            for kd in kinds:
                viol.append((kd, (g, threads)))
            continue
        for st, i in succ:
            if st not in parent:
                parent[st] = ((g, threads), i)
                stack.append(st)
        if limit and len(parent) > limit:
            break
    explore.spin4 = spin4[0]
    return len(parent), viol, parent


def solo_spins(g, th, cfg):
    """Run one event-loop thread alone from epoll (descriptor readable) until it is back at
    epoll; True if the descriptor is still readable then (a solo cycle: it spins forever).
    """
    cur, left = th, False
    for _ in range(400):
        if cur[0] == "E":
            res = [(g2, t2) for g2, t2 in e_step(g, cur, cfg, True)]
        else:
            res = [(g2, t2) for g2, t2, _w in q_step(g, cur, cfg, True)]
        if not res:
            return False
        g, cur = res[0]
        if (
            cur[1] == "SPIN"
        ):  # the 4-returns counter fired inside the solo run: keep sweeping
            cur = cur[:1] + (("S",) if cur[0] == "E" else ("Q1",)) + cur[2:]
        if cur[1] not in ("EP", "MOD"):
            left = True
        elif left:
            if cur[1] == "MOD":
                continue
            return g[FD] > 0
    return True


NAMES = "R0 R1 AH WT0 WT1 PEND FD DONE SEEN WSEQ FIRED INTR IINTR SIG ST EN".split()


def trace(parent, st, cfg):
    """Path to st (DFS path, not the shortest)."""
    path = []
    while parent[st]:
        p, i = parent[st]
        path.append((i, p, st))
        st = p
    lines = []
    for i, p, s in reversed(path):
        g = s[0]
        th = s[1][i]
        diff = [f"{n}={g[j]}" for j, n in enumerate(NAMES) if g[j] != p[0][j]]
        lines.append(
            f"T{i} {th[0]}:{str(p[1][i][1]):>5s}->{str(th[1]):6s} " + " ".join(diff)
        )
    return lines


def E():
    return ("E", "S", 0, 0, 0, 0, 0)


def W(t, sp=0, to=0):
    return ("W", "W0", t, 0, sp, to, None)


def P():
    return ("P", 0, 0, 0, 0, 0, 0)


def A():
    return ("A", 0, 0, 0, 0, 0, 0)


def X():
    return ("X", "S", 0, 0, 0, 0, 0)


def Q():
    return ("Q", "Q1", 0, 0, 0, 0, 0, 0)


pub0, pub1, pub01, fl = ("pub", (0,)), ("pub", (1,)), ("pub", (0, 1)), ("flush",)

# name, expect, cfg
CASES = [
    # regression: the design's cases with the fidelity fixes (split W1/W2, snapshot W3)
    (
        "R0 N9 one-shot DP consumer beside a sleeping loop",
        "ok",
        dict(
            e_targets=(0,), threads=[E(), X(), P()], prod_ops=[pub0, fl, pub0, pub0, fl]
        ),
    ),
    (
        "R1 N9 without the re-post",
        "bug",
        dict(
            e_targets=(0,),
            threads=[E(), X(), P()],
            prod_ops=[pub0, fl, pub0, pub0, fl],
            repost=False,
        ),
    ),
    (
        "R2 I3 single sweep, flush each",
        "ok",
        dict(
            e_targets=(0, 1),
            threads=[E(), P()],
            prod_ops=[pub01, fl, pub1, fl, pub0, fl],
        ),
    ),
    (
        "R5 L10 two loops, same target",
        "ok",
        dict(
            e_targets=(0,), threads=[E(), E(), P()], prod_ops=[pub0, pub0, fl, pub0, fl]
        ),
    ),
    (
        "R7 I1' WT + loop, same target",
        "ok",
        dict(
            e_targets=(0,),
            threads=[E(), W(0), P()],
            prod_ops=[pub0, fl, pub0, fl, pub0, fl],
        ),
    ),
    # 2.1 two handle users in epoll + a WT waiter
    (
        "X1 two loops (LT) + WT waiter, lanes {0,1}",
        "ok",
        dict(
            e_targets=(0, 1),
            threads=[E(), E(), W(0), P()],
            prod_ops=[pub01, fl, pub0, fl],
        ),
    ),
    (
        "X1b same with EPOLLONESHOT",
        "ok",
        dict(
            e_targets=(0, 1),
            threads=[E(), E(), W(0), P()],
            prod_ops=[pub01, fl, pub0, fl],
            oneshot=True,
        ),
    ),
    (
        "X1c two loops + WT waiter that times out (final attempt arms the handle)",
        "ok",
        dict(
            e_targets=(0,),
            threads=[E(), E(), W(0, 0, 1), P()],
            prod_ops=[pub0, fl, pub0, pub0, fl],
            w_final_handle=True,
            w_once=True,
        ),
    ),
    (
        "X1d two loops, one shot, same lane, 3 units",
        "ok",
        dict(
            e_targets=(0,),
            threads=[E(), E(), P()],
            prod_ops=[pub0, fl, pub0, pub0, fl],
            oneshot=True,
        ),
    ),
    # 2.2 interrupt racing a drain
    (
        "X2 session interrupt (full WAKE_NOW) racing a loop's drain",
        "ok",
        dict(
            e_targets=(0, 1),
            threads=[E(), ("I", 0, 0, 0, 0, 0), P()],
            prod_ops=[pub0, fl, pub1, fl],
        ),
    ),
    (
        "X2b interrupt + two loops",
        "ok",
        dict(
            e_targets=(0,),
            threads=[E(), E(), ("I", 0, 0, 0, 0, 0), P()],
            prod_ops=[pub0, fl, pub0, fl],
        ),
    ),
    (
        "X2c mtl_wait(0) loop, interrupt ON, units arrive (v2: stops on -ECANCELED)",
        "ok",
        dict(
            e_targets=(0, 1),
            threads=[Q(), ("I", 0, 0, 0, 0, 0), P()],
            prod_ops=[pub0, fl, pub1, fl],
        ),
    ),
    (
        "X2d same, Q7 skipped while cancelled (fix)",
        "ok",
        dict(
            e_targets=(0, 1),
            threads=[Q(), ("I", 0, 0, 0, 0, 0), P()],
            prod_ops=[pub0, fl, pub1, fl],
            q_fix=True,
        ),
    ),
    (
        "X2e mtl_wait(0) loop, no interrupt",
        "ok",
        dict(
            e_targets=(0, 1),
            threads=[Q(), P()],
            prod_ops=[pub0, fl, pub1, fl, pub01, fl],
        ),
    ),
    # 2.3 close racing a WT waiter and a handle user
    (
        "X3 close vs WT waiter vs loop",
        "ok",
        dict(
            e_targets=(0, 1),
            threads=[E(), W(0), ("C", 0, 0, 0, 0, 0), P()],
            prod_ops=[pub01, fl, pub1, fl],
        ),
    ),
    (
        "X3b close vs two loops + WT waiter",
        "ok",
        dict(
            e_targets=(0,),
            threads=[E(), E(), W(0), ("C", 0, 0, 0, 0, 0), P()],
            prod_ops=[pub0, fl],
        ),
    ),
    # 2.5 spurious futex returns
    (
        "X5 two WT waiters same lane, 2 spurious returns each",
        "ok",
        dict(
            e_targets=(), threads=[W(0, 2), W(0, 2), P()], prod_ops=[pub0, fl, pub0, fl]
        ),
    ),
    (
        "X5b WT (spurious, timeout) + loop on the same lane",
        "ok",
        dict(
            e_targets=(0,),
            threads=[E(), W(0, 2, 1), P()],
            prod_ops=[pub0, fl, pub0, fl, pub0, fl],
            w_final_handle=True,
        ),
    ),
    # 2.6 eventfd write fails (EAGAIN) and is ignored
    (
        "X6 write() may fail, single loop",
        "bug",
        dict(
            e_targets=(0,),
            threads=[E(), P()],
            prod_ops=[pub0, fl, pub0, fl],
            write_fail=True,
        ),
    ),
    # 2.7 a thread killed between steps
    (
        "X7 one-shot DP consumer killed at any step",
        "bug",
        dict(
            e_targets=(0,),
            threads=[E(), X(), P()],
            prod_ops=[pub0, fl, pub0, fl],
            kill_x=True,
        ),
    ),
    (
        "X7b WT waiter killed between arm and sleep",
        "bug",
        dict(e_targets=(), threads=[W(0), P()], prod_ops=[pub0, fl], kill_w=True),
    ),
    # two concurrent wakers: tasklet flush + application release on the same lane
    (
        "X9 flush + app-thread waker + loop + WT, same lane",
        "ok",
        dict(
            e_targets=(0,),
            threads=[E(), W(0), P(), A()],
            prod_ops=[pub0, fl, pub0, fl],
            app_ops=[0],
        ),
    ),
    (
        "X9b flush + app waker + two loops, lanes {0,1}",
        "ok",
        dict(
            e_targets=(0, 1),
            threads=[E(), E(), P(), A()],
            prod_ops=[pub1, fl, pub0, fl],
            app_ops=[0],
        ),
    ),
    (
        "X9c instance interrupt + WT waiters + completions",
        "ok",
        dict(
            e_targets=(),
            threads=[W(0, 1), W(1, 1), ("J", 0), P()],
            prod_ops=[pub0, fl, pub1, fl],
        ),
    ),
]


SIMPLE = [
    (
        "S0 always-drain: N9 one-shot consumer beside a loop",
        "ok",
        dict(
            e_targets=(0,),
            threads=[E(), X(), P()],
            prod_ops=[pub0, fl, pub0, pub0, fl],
            always_drain=True,
        ),
    ),
    (
        "S2 always-drain: I3 single sweep",
        "ok",
        dict(
            e_targets=(0, 1),
            threads=[E(), P()],
            prod_ops=[pub01, fl, pub1, fl, pub0, fl],
            always_drain=True,
        ),
    ),
    (
        "S5 always-drain: two loops same target",
        "ok",
        dict(
            e_targets=(0,),
            threads=[E(), E(), P()],
            prod_ops=[pub0, pub0, fl, pub0, fl],
            always_drain=True,
        ),
    ),
    (
        "S7 always-drain: WT + loop same target",
        "ok",
        dict(
            e_targets=(0,),
            threads=[E(), W(0), P()],
            prod_ops=[pub0, fl, pub0, fl, pub0, fl],
            always_drain=True,
        ),
    ),
]
CASES += SIMPLE
REDUCED = [
    (
        "X1r two loops (LT) + WT waiter, same lane",
        "ok",
        dict(
            e_targets=(0,), threads=[E(), E(), W(0), P()], prod_ops=[pub0, fl, pub0, fl]
        ),
    ),
    (
        "X1br same with EPOLLONESHOT",
        "ok",
        dict(
            e_targets=(0,),
            threads=[E(), E(), W(0), P()],
            prod_ops=[pub0, fl, pub0, fl],
            oneshot=True,
        ),
    ),
    (
        "X1cr two loops + WT that times out, final attempt on the handle",
        "ok",
        dict(
            e_targets=(0,),
            threads=[E(), E(), W(0, 0, 1), P()],
            prod_ops=[pub0, fl, pub0, fl],
            w_final_handle=True,
            w_once=True,
        ),
    ),
    (
        "X2br session interrupt + two loops",
        "ok",
        dict(
            e_targets=(0,),
            threads=[E(), E(), ("I", 0, 0, 0, 0, 0), P()],
            prod_ops=[pub0, fl],
        ),
    ),
    (
        "X3br close vs loop + WT waiter, same lane, completions",
        "ok",
        dict(
            e_targets=(0,),
            threads=[E(), W(0), ("C", 0, 0, 0, 0, 0), P()],
            prod_ops=[pub0, fl, pub0, fl],
        ),
    ),
    (
        "X3cr close vs two loops",
        "ok",
        dict(
            e_targets=(0,),
            threads=[E(), E(), ("C", 0, 0, 0, 0, 0), P()],
            prod_ops=[pub0, fl],
        ),
    ),
    (
        "X5br WT (1 spurious, timeout) + loop, same lane",
        "ok",
        dict(
            e_targets=(0,),
            threads=[E(), W(0, 1, 1), P()],
            prod_ops=[pub0, fl, pub0, fl],
            w_final_handle=True,
        ),
    ),
    (
        "X9br flush + app-thread waker + two loops",
        "ok",
        dict(
            e_targets=(0,),
            threads=[E(), E(), P(), A()],
            prod_ops=[pub0, fl],
            app_ops=[0],
        ),
    ),
]
CASES += REDUCED
V2 = [
    (
        "V1 M4: acquire_slot-like consumer takes the lane bit beside a sleeping loop (no fix)",
        "bug",
        dict(
            e_targets=(0,),
            threads=[E(), X(), P()],
            prod_ops=[pub0, fl, pub0, pub0, fl],
            x_narrow=True,
        ),
    ),
    (
        "V2 M4: the same with D8 by the lane predicate",
        "ok",
        dict(
            e_targets=(0,),
            threads=[E(), X(), P()],
            prod_ops=[pub0, fl, pub0, pub0, fl],
            x_narrow=True,
            m4_fix=True,
        ),
    ),
    (
        "V3 mtl_wait(0) loop and a DP loop on one handle, interrupt, units",
        "ok",
        dict(
            e_targets=(0, 1),
            threads=[Q(), E(), ("I", 0, 0, 0, 0, 0), P()],
            prod_ops=[pub0, fl, pub1, fl],
            q_intr_check=False,
        ),
    ),
]
CASES += V2
_I = ("I", 0, 0, 0, 0, 0)
V21 = [
    (
        "N1 mtl_wait(0) loop alone, interrupt, no data (v2.1 Q6a)",
        "ok",
        dict(e_targets=(0, 1), threads=[Q(), _I], prod_ops=[]),
    ),
    (
        "N1m the same without Q6a (v2)",
        "bug",
        dict(e_targets=(0, 1), threads=[Q(), _I], prod_ops=[], q6a=False),
    ),
    (
        "N2 X2c with the delivery check",
        "ok",
        dict(e_targets=(0, 1), threads=[Q(), _I, P()], prod_ops=[pub0, fl, pub1, fl]),
    ),
    (
        "N3 Q + DP loop on one handle, interrupt (contract: not delivered to Q)",
        "bug",
        dict(
            e_targets=(0, 1), threads=[Q(), E(), _I, P()], prod_ops=[pub0, fl, pub1, fl]
        ),
    ),
    (
        "N4 two mtl_wait(0) loops on one handle, interrupt",
        "ok",
        dict(e_targets=(0, 1), threads=[Q(), Q(), _I, P()], prod_ops=[pub0, fl]),
    ),
    (
        "N5 X2c then MTL_INTR_OFF, sweep after OFF",
        "ok",
        dict(
            e_targets=(0, 1),
            threads=[Q(), _I, P()],
            prod_ops=[pub0, fl, pub1, fl],
            intr_off=True,
        ),
    ),
    (
        "N6 Q + DP loop, interrupt then OFF",
        "ok",
        dict(
            e_targets=(0, 1),
            threads=[Q(), E(), _I, P()],
            prod_ops=[pub0, fl, pub1, fl],
            intr_off=True,
            q_intr_check=False,
        ),
    ),
]
CASES += V21
ET = []
for _i in (0, 2, 4, 9, 14, 17, 21, 34):
    _n, _e, _c = CASES[_i]
    ET.append(("ET " + _n, _e, dict(_c, et=True)))
    ET.append(
        (
            "ETm " + _n + " (no edge rule)",
            "bug" if _i in (2, 9) else "any",
            dict(_c, et=True, edge_rule=False),
        )
    )
CASES += ET


def run(i):
    name, expect, cfg = CASES[i]
    t = time.time()
    n, v, parent = explore(cfg)
    kinds = sorted({k for k, _ in v})
    ok = True if expect == "any" else ((not v) if expect == "ok" else bool(v))
    print(
        f"{i:2d} {name:72s} states={n:9d} viol={len(v):5d} {kinds} "
        f"spurious4={getattr(explore, 'spin4', 0)} expect={expect} "
        f"{'PASS' if ok else 'UNEXPECTED'} {time.time() - t:.0f}s",
        flush=True,
    )
    if v:
        # shortest-looking: pick the violation with the shortest parent chain
        best = None
        for k, st in v:
            ln = 0
            s = st
            while parent.get(s):
                s = parent[s][0]
                ln += 1
            if best is None or ln < best[0]:
                best = (ln, k, st)
        print(f"   first {best[1]} trace ({best[0]} steps):")
        for line in trace(parent, best[2], cfg):
            print("     " + line)
        print("   final threads:", best[2][1])
    return ok


if __name__ == "__main__":
    extra = {}
    for a in [a for a in sys.argv[1:] if "=" in a]:
        k, v = a.split("=")
        extra[k] = v == "1"
    if extra:
        CASES[:] = [
            (n, "bug" if extra.get("expect_bug") else e, dict(c, **extra))
            for n, e, c in CASES
        ]
    idx = [int(a) for a in sys.argv[1:] if "=" not in a] or range(len(CASES))
    bad = sum(0 if run(i) else 1 for i in idx)
    sys.exit(1 if bad else 0)
