#!/usr/bin/env python3
"""v2.1: the edge rule (a miss that leaves another lane pending signals once) is on by default.
v2: with cfg["always_drain"] the stale-signal counters are gone (wait design v2).
Exhaustive interleaving check (sequential consistency) of the wait/wake protocol of
engine.md §7.1-§7.3, and of the review's protocol (h) for comparison.

Every atomic operation and every syscall is one step. The checker explores every
interleaving of the threads of a configuration and reports:
  LOST  a terminal state where a thread sleeps (futex or epoll) while a unit it waits for
        is ready (or its target is interrupted) and nothing can wake it;
  SPIN  an event-loop thread whose epoll keeps returning after the producer finished and
        every unit was consumed (a level-triggered busy loop).
Weak-memory orderings are not modelled here; engine.md §7.1 argues them with the C11 fence
rules. Usage: python3 wait_model.py [CASE...] [--v1]: the design (always drain, edge rule),
or with --v1 the first version; every case by default, else the given indices of CASES.
"""

import sys

# global state layout
R0, R1, AH, WT0, WT1, PEND, FD, DONE, SEEN, WSEQ, FIRED, INTR, IINTR, SIG = range(14)
NG = 14


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


# ---------------------------------------------------------------------------------------
# Producer: a tasklet that publishes units and flushes (design: signal only on pend 0->x,
# then done++). ops: ('pub', (targets...)), ('flush',). variant 'h' = the review's waker:
# pend |= hf; write(efd) unconditionally, no done counter.


def producer_step(g, th, cfg):
    _, pc, k, F, hf = th
    ops = cfg["prod_ops"]
    if pc == "END":
        return []
    if k == len(ops):
        return [(g, ("P", "END", k, 0, 0))]
    op = ops[k]
    if op[0] == "pub":
        ts = op[1]
        if pc == 0:  # publish (release store)
            pairs = []
            for t in ts:
                pairs += [R0 + t, ready(g, t) + 1]
            return [(upd(g, *pairs), ("P", 1, k, 0, 0))]
        if pc == 1:  # fence; load armed; mark fired
            f = 0
            for t in ts:
                if wt(g, t) > 0 or (g[AH] & bit(t)):
                    f |= bit(t)
            g2 = upd(g, FIRED, g[FIRED] | f) if f else g
            return [(g2, ("P", 0, k + 1, 0, 0))]
    if op[0] == "flush":
        if pc == 0:  # F = xchg(fired, 0)
            f = g[FIRED]
            if f == 0:
                return [(g, ("P", 0, k + 1, 0, 0))]
            return [(upd(g, FIRED, 0), ("P", 3, k, f, 0))]
        if pc == 3:  # old = fetch_and(armed.h, ~F)
            old = g[AH]
            return [(upd(g, AH, old & ~F), ("P", 4, k, F, old & F))]
        if pc == 4:  # WT armed on a fired lane? bump wseq
            if any(wt(g, t) > 0 for t in (0, 1) if F & bit(t)):
                return [(upd(g, WSEQ, g[WSEQ] + 1), ("P", 5, k, F, hf))]
            return [(g, ("P", 6, k, F, hf))]
        if pc == 5:  # FUTEX_WAKE_BITSET(F): wake matching sleepers
            return [(g, ("P", 6, k, F, hf, "WAKE"))]
        if pc == 6:  # handle part
            if hf == 0:
                return [(g, ("P", 0, k + 1, 0, 0))]
            oldp = g[PEND]
            g2 = upd(g, PEND, oldp | hf)
            if cfg["variant"] == "h":
                return [(g2, ("P", 7, k, F, hf))]
            if oldp != 0:
                return [(g2, ("P", 0, k + 1, 0, 0))]
            return [(g2, ("P", 7 if cfg.get("always_drain") else 61, k, F, hf))]
        if pc == 61:  # h_sig++ (before the syscall)
            return [(upd(g, SIG, g[SIG] + 1), ("P", 7, k, F, hf))]
        if pc == 7:  # write(efd, 1)
            g2 = upd(g, FD, min(g[FD] + 1, 3))
            if cfg["variant"] == "h" or cfg.get("always_drain"):
                return [(g2, ("P", 0, k + 1, 0, 0))]
            return [(g2, ("P", 8, k, F, hf))]
        if pc == 8:  # done++
            return [(upd(g, DONE, g[DONE] + 1), ("P", 0, k + 1, 0, 0))]
    raise RuntimeError(("producer", th))


# ---------------------------------------------------------------------------------------
# Event-loop consumer E (design): sweep every target until -EAGAIN, then epoll.
# A DP call on T: attempt; on a miss: consume pend T (drain if the last bit and done !=
# seen, re-signal if pend became non-zero), arm h(T) test-before-set, fence, re-check.
# th = ('E', pc, ti, now, d, s, idle)


def e_step(g, th, cfg, prod_done):
    _, pc, ti, now, d, s, idle = th
    ts = cfg["e_targets"]
    if pc == "EP":
        if g[FD] == 0:
            return []  # blocked in epoll_wait
        nidle = idle
        if prod_done(g) and all(ready(g, t) == 0 for t in ts):
            nidle = idle + 1
            if nidle >= 4:
                return [(g, ("E", "SPIN", 0, 0, 0, 0, nidle))]
        return [(g, ("E", "S", 0, 0, 0, 0, nidle))]
    if pc == "SPIN":
        return []
    t = ts[ti]
    b = bit(t)

    def next_target(g2):
        if ti + 1 == len(ts):
            return (g2, ("E", "EP", 0, 0, 0, 0, idle))
        return (g2, ("E", "S", ti + 1, 0, 0, 0, idle))

    if pc == "S":  # attempt
        if ready(g, t) > 0:
            return [(upd(g, R0 + t, ready(g, t) - 1), ("E", "S", ti, 0, 0, 0, 0))]
        return [(g, ("E", "C1", ti, 0, 0, 0, idle))]
    if pc == "C1":  # load pend
        p = g[PEND]
        if p & b:
            return [(g, ("E", "C1b", ti, 0, 0, 0, idle))]
        return [(g, ("E", "C2", ti, p, 0, 0, idle))]
    if pc == "C1b":  # old = fetch_and(pend, ~b): this call took the bit (s = 1)
        old = g[PEND]
        return [(upd(g, PEND, old & ~b), ("E", "C2", ti, old & ~b, 0, 1, idle))]
    if pc == "C2":  # last bit cleared (or none pending): load h_sig
        if now != 0:
            if cfg.get("edge_rule", True) and cfg.get(
                "always_drain"
            ):  # v2.1: K3, p != 0: signal once
                return [(upd(g, FD, min(g[FD] + 1, 3)), ("E", "A1", ti, 0, 0, s, idle))]
            return [(g, ("E", "A1", ti, 0, 0, s, idle))]
        if cfg.get("always_drain"):
            if cfg.get("mut_drain_only_if_took") and not s:  # mutant: (h)'s rule
                return [(g, ("E", "A1", ti, 0, 0, s, idle))]
            return [(g, ("E", "D1", ti, 0, 0, s, idle))]
        return [(g, ("E", "C3", ti, 0, g[SIG], s, idle))]
    if pc == "C3":  # load seen; compare with h_sig
        if d != g[SEEN]:
            return [(g, ("E", "C4", ti, 0, 0, s, idle))]
        return [(g, ("E", "A1", ti, 0, 0, s, idle))]
    if pc == "C4":  # d = load(h_done), the snapshot the read will cover
        return [(g, ("E", "D1", ti, 0, g[DONE], s, idle))]
    if pc == "D1":  # read(efd): counter -> 0
        return [
            (
                upd(g, FD, 0),
                ("E", "D3" if cfg.get("always_drain") else "D2", ti, 0, d, s, idle),
            )
        ]
    if pc == "D2":  # seen = d
        return [(upd(g, SEEN, d), ("E", "D3", ti, 0, 0, s, idle))]
    if pc == "D3":  # re-check pend after the read
        if g[PEND] != 0 and not cfg.get("mut_no_resignal"):
            return [
                (
                    g,
                    (
                        "E",
                        "D4b" if cfg.get("always_drain") else "D4",
                        ti,
                        0,
                        0,
                        s,
                        idle,
                    ),
                )
            ]
        return [(g, ("E", "A1", ti, 0, 0, s, idle))]
    if pc == "D4":  # re-signal: h_sig++
        return [(upd(g, SIG, g[SIG] + 1), ("E", "D4b", ti, 0, 0, s, idle))]
    if pc == "D4b":  # write(efd, 1)
        return [
            (
                upd(g, FD, min(g[FD] + 1, 3)),
                ("E", "A1" if cfg.get("always_drain") else "D5", ti, 0, 0, s, idle),
            )
        ]
    if pc == "D5":
        return [(upd(g, DONE, g[DONE] + 1), ("E", "A1", ti, 0, 0, s, idle))]
    if pc == "A1":  # test
        if cfg.get("mut_no_arm"):
            return [(g, ("E", "A3", ti, 0, 0, s, idle))]
        if g[AH] & b:
            return [(g, ("E", "A3", ti, 0, 0, s, idle))]
        return [(g, ("E", "A2", ti, 0, 0, s, idle))]
    if pc == "A2":  # set
        return [(upd(g, AH, g[AH] | b), ("E", "A3", ti, 0, 0, s, idle))]
    if pc == "A3":  # fence; re-check; one more attempt
        if ready(g, t) > 0 and not cfg.get("mut_no_recheck"):
            g2 = upd(g, R0 + t, ready(g, t) - 1)
            if s and cfg.get("repost", True):
                return [(g2, ("E", "RP1", ti, 0, 0, 0, 0))]
            if cfg.get("oneshot_e") is not None and th[0] == "E":
                pass
            return [(g2, ("E", "S", ti, 0, 0, 0, 0))]
        return [next_target(g)]
    if pc == "RP1":  # took a bit and returns a unit: post the bit again (pend first)
        oldp = g[PEND]
        g2 = upd(g, PEND, oldp | b)
        if oldp != 0:
            return [(g2, ("E", "S", ti, 0, 0, 0, 0))]
        return [
            (g2, ("E", "RP3" if cfg.get("always_drain") else "RP2", ti, 0, 0, 0, 0))
        ]
    if pc == "RP2":
        return [(upd(g, SIG, g[SIG] + 1), ("E", "RP3", ti, 0, 0, 0, 0))]
    if pc == "RP3":
        return [
            (
                upd(g, FD, min(g[FD] + 1, 3)),
                ("E", "S" if cfg.get("always_drain") else "RP4", ti, 0, 0, 0, 0),
            )
        ]
    if pc == "RP4":
        return [(upd(g, DONE, g[DONE] + 1), ("E", "S", ti, 0, 0, 0, 0))]
    raise RuntimeError(("E", th))


# The review's protocol (h) for a DP call, same event loop around it.
def x_step(g, th, cfg, prod_done):
    """One DP call on target 0 (e.g. the final attempt of a timed-out WT call), then exit."""
    if th[1] == "DONE":
        return []
    sub = ("E",) + th[1:]
    xcfg = dict(cfg, e_targets=(0,))
    out = []
    for g2, t2 in e_step(g, sub, xcfg, prod_done):
        pc_from, pc_to = sub[1], t2[1]
        ended = (
            pc_to == "EP"  # the call returned -EAGAIN
            or (pc_from == "S" and pc_to == "S")  # the first attempt took a unit
            or (
                pc_from == "A3" and pc_to == "S"
            )  # the re-attempt took one, no re-post due
            or (pc_from in ("RP1", "RP3", "RP4") and pc_to == "S")  # re-post finished
        )
        out.append((g2, ("X", "DONE", 0, 0, 0, 0, 0) if ended else ("X",) + t2[1:]))
    return out


def xh_step(g, th, cfg, prod_done):
    """One DP call on target 0 with the review's protocol (h), then exit."""
    if th[1] == "DONE":
        return []
    sub = ("E",) + th[1:]
    xcfg = dict(cfg, e_targets=(0,))
    out = []
    for g2, t2 in h_step(g, sub, xcfg, prod_done):
        ended = t2[1] == "EP" or (sub[1] in ("H6", "H9") and t2[1] == "S")
        out.append((g2, ("XH", "DONE", 0, 0, 0, 0, 0) if ended else ("XH",) + t2[1:]))
    return out


def h_step(g, th, cfg, prod_done):
    _, pc, ti, now, d, s, idle = th
    ts = cfg["e_targets"]
    if pc in ("EP", "SPIN"):
        return e_step(g, th, cfg, prod_done)
    t = ts[ti]
    b = bit(t)

    def next_target(g2):
        if ti + 1 == len(ts):
            return (g2, ("E", "EP", 0, 0, 0, 0, idle))
        return (g2, ("E", "S", ti + 1, 0, 0, 0, idle))

    if pc == "S":  # p = load(h_pend)
        if g[PEND] & b:
            return [(g, ("E", "H2", ti, 0, 0, 0, idle))]
        return [(g, ("E", "H6", ti, 0, 0, 0, idle))]
    if pc == "H2":
        old = g[PEND]
        g2 = upd(g, PEND, old & ~b)
        if not (old & ~b):
            return [(g2, ("E", "H3", ti, 0, 0, 0, idle))]
        return [(g2, ("E", "H6", ti, 0, 0, 0, idle))]
    if pc == "H3":  # read(efd)
        return [(upd(g, FD, 0), ("E", "H4", ti, 0, 0, 0, idle))]
    if pc == "H4":
        if g[PEND]:
            return [(g, ("E", "H5", ti, 0, 0, 0, idle))]
        return [(g, ("E", "H6", ti, 0, 0, 0, idle))]
    if pc == "H5":
        return [(upd(g, FD, min(g[FD] + 1, 3)), ("E", "H6", ti, 0, 0, 0, idle))]
    if pc == "H6":  # DP attempt
        if ready(g, t) > 0:
            return [(upd(g, R0 + t, ready(g, t) - 1), ("E", "S", ti, 0, 0, 0, 0))]
        return [(g, ("E", "H7", ti, 0, 0, 0, idle))]
    if pc == "H7":
        if g[AH] & b:
            return [next_target(g)]  # (h): armed already -> return -EAGAIN at once
        return [(g, ("E", "H8", ti, 0, 0, 0, idle))]
    if pc == "H8":
        return [(upd(g, AH, g[AH] | b), ("E", "H9", ti, 0, 0, 0, idle))]
    if pc == "H9":
        if ready(g, t) > 0:
            return [(upd(g, R0 + t, ready(g, t) - 1), ("E", "S", ti, 0, 0, 0, 0))]
        return [next_target(g)]
    raise RuntimeError(("H", th))


# ---------------------------------------------------------------------------------------
# WT consumer W on target t (futex). th = ('W', pc, t, v)


def w_step(g, th, cfg):
    _, pc, t, v = th
    if pc == "W0":
        if g[INTR] or g[IINTR]:
            return [(g, ("W", "CANCELED", t, 0))]
        if ready(g, t) > 0:
            return [(upd(g, R0 + t, ready(g, t) - 1), ("W", "W0", t, 0))]
        return [(g, ("W", "W1", t, 0))]
    if pc == "W1":  # arm (seq_cst RMW), fence
        return [(upd(g, WT0 + t, wt(g, t) + 1), ("W", "W2", t, 0))]
    if pc == "W2":  # v = load(wseq)
        return [(g, ("W", "W3", t, g[WSEQ]))]
    if pc == "W3":  # re-check
        if ready(g, t) > 0 or g[INTR] or g[IINTR]:
            return [(g, ("W", "W6", t, 0))]
        return [(g, ("W", "W4", t, v))]
    if pc == "W4":  # futex_wait(v): compare and queue, atomically
        if g[WSEQ] != v:
            return [(g, ("W", "W6", t, 0))]
        return [(g, ("W", "WB", t, v))]
    if pc == "WB":
        return []  # asleep; only a wake moves it
    if pc == "W6":  # disarm
        return [(upd(g, WT0 + t, wt(g, t) - 1), ("W", "W0", t, 0))]
    if pc == "CANCELED":
        return []
    raise RuntimeError(("W", th))


# Interrupter: session ('I') or instance ('J'). th = (kind, pc)
def i_step(g, th, cfg):
    kind, pc = th
    flag = INTR if kind == "I" else IINTR
    if pc == 0:
        return [(upd(g, flag, 1), (kind, 1))]
    if pc == 1:  # fence; load wt_armed of the session (the walk visits it)
        if wt(g, 0) > 0 or wt(g, 1) > 0:
            return [(upd(g, WSEQ, g[WSEQ] + 1), (kind, 2))]
        return [(g, (kind, "END"))]
    if pc == 2:
        return [(g, (kind, "END", "WAKEALL"))]
    return []


# ---------------------------------------------------------------------------------------


def explore(cfg):
    threads0 = []
    for th in cfg["threads"]:
        threads0.append(th)
    g0 = [0] * NG
    for t, n in cfg.get("initial_ready", {}).items():
        g0[R0 + t] = n
    g0 = tuple(g0)
    start = (g0, tuple(threads0))
    seen = {start}
    stack = [start]
    violations = []

    def prod_done(g, threads):
        return all(th[0] != "P" or th[1] == "END" for th in threads)

    while stack:
        g, threads = stack.pop()
        succ = []
        for i, th in enumerate(threads):
            kind = th[0]
            if kind == "P":
                res = producer_step(g, th, cfg)
            elif kind == "E":
                fn = h_step if cfg["variant"] == "h" else e_step
                res = fn(g, th, cfg, lambda gg: prod_done(gg, threads))
            elif kind == "W":
                res = w_step(g, th, cfg)
            elif kind == "X":
                res = x_step(g, th, cfg, lambda gg: prod_done(gg, threads))
            elif kind == "XH":
                res = xh_step(g, th, cfg, lambda gg: prod_done(gg, threads))
            elif kind in ("I", "J"):
                res = i_step(g, th, cfg)
            else:
                raise RuntimeError(kind)
            for g2, th2 in res:
                nts = list(threads)
                # wake actions: FUTEX_WAKE_BITSET(F) or MATCH_ANY
                if kind == "P" and len(th2) == 6 and th2[5] == "WAKE":
                    F = th2[3]
                    th2 = th2[:5]
                    for j, o in enumerate(nts):
                        if o[0] == "W" and o[1] == "WB" and (F & bit(o[2])):
                            nts[j] = ("W", "W6", o[2], 0)
                if kind in ("I", "J") and len(th2) == 3:
                    th2 = th2[:2]
                    for j, o in enumerate(nts):
                        if o[0] == "W" and o[1] == "WB":
                            nts[j] = ("W", "W6", o[2], 0)
                nts[i] = th2
                st = (g2, tuple(nts))
                if th2[0] == "E" and th2[1] == "SPIN":
                    violations.append(("SPIN", st))
                    continue
                succ.append(st)
        if not succ:
            # terminal: check for a lost wake-up
            for th in threads:
                if th[0] == "W" and th[1] == "WB":
                    if ready(g, th[2]) > 0 or g[INTR] or g[IINTR]:
                        violations.append(("LOST(W)", (g, threads)))
                if th[0] == "E" and th[1] == "EP" and g[FD] == 0:
                    if any(ready(g, t) > 0 for t in cfg["e_targets"]):
                        violations.append(("LOST(E)", (g, threads)))
            continue
        for st in succ:
            if st not in seen:
                seen.add(st)
                stack.append(st)
    return len(seen), violations


def E(variant_targets=(0, 1)):
    return ("E", "S", 0, 0, 0, 0, 0)


def W(t):
    return ("W", "W0", t, 0)


def P(ops):
    return ("P", 0, 0, 0, 0)


CASES = [
    # name, variant, e_targets, threads, producer ops
    (
        "N9 one-shot DP consumer beside a sleeping event loop",
        "d",
        (0,),
        [E(), ("X", "S", 0, 0, 0, 0, 0), P(None)],
        [("pub", (0,)), ("flush",), ("pub", (0,)), ("pub", (0,)), ("flush",)],
    ),
    (
        "N9 the same without the re-post",
        "d-norepost",
        (0,),
        [E(), ("X", "S", 0, 0, 0, 0, 0), P(None)],
        [("pub", (0,)), ("flush",), ("pub", (0,)), ("pub", (0,)), ("flush",)],
    ),
    (
        "I3 single sweep, units on both targets, flush each",
        "d",
        (0, 1),
        [E(), P(None)],
        [
            ("pub", (0, 1)),
            ("flush",),
            ("pub", (1,)),
            ("flush",),
            ("pub", (0,)),
            ("flush",),
        ],
    ),
    (
        "I3 single sweep, one flush for three units",
        "d",
        (0, 1),
        [E(), P(None)],
        [("pub", (0,)), ("pub", (1,)), ("pub", (0, 1)), ("flush",)],
    ),
    (
        "L10 two event-loop threads share one handle",
        "d",
        (0, 1),
        [E(), E(), P(None)],
        [
            ("pub", (0,)),
            ("flush",),
            ("pub", (1,)),
            ("flush",),
            ("pub", (0,)),
            ("flush",),
        ],
    ),
    (
        "L10 two threads, same target, two units one flush",
        "d",
        (0,),
        [E(), E(), P(None)],
        [("pub", (0,)), ("pub", (0,)), ("flush",), ("pub", (0,)), ("flush",)],
    ),
    (
        "I1 WT thread + event loop on other target",
        "d",
        (1,),
        [E(), W(0), P(None)],
        [("pub", (0, 1)), ("flush",), ("pub", (0, 1)), ("flush",)],
    ),
    (
        "I1' WT thread + event loop on the same target",
        "d",
        (0,),
        [E(), W(0), P(None)],
        [
            ("pub", (0,)),
            ("flush",),
            ("pub", (0,)),
            ("flush",),
            ("pub", (0,)),
            ("flush",),
        ],
    ),
    (
        "I2 two WT waiters, different targets",
        "d",
        (),
        [W(0), W(1), P(None)],
        [
            ("pub", (1,)),
            ("flush",),
            ("pub", (0,)),
            ("flush",),
            ("pub", (1,)),
            ("flush",),
        ],
    ),
    (
        "I2 two WT waiters, same target, one flush",
        "d",
        (),
        [W(0), W(0), P(None)],
        [("pub", (0,)), ("pub", (0,)), ("flush",)],
    ),
    ("I2b/L6 session interrupt vs two WT waiters", "d", (), [W(0), W(1), ("I", 0)], []),
    ("F1/L6 instance interrupt vs a WT waiter", "d", (), [W(0), ("J", 0)], []),
    (
        "interrupt + completions + WT",
        "d",
        (),
        [W(0), ("I", 0), P(None)],
        [("pub", (0,)), ("flush",)],
    ),
    # the review's (h), for comparison
    (
        "(h) N3 one-shot DP consumer beside a sleeping event loop",
        "h",
        (0,),
        [E(), ("XH", "S", 0, 0, 0, 0, 0), P(None)],
        [("pub", (0,)), ("flush",), ("pub", (0,)), ("pub", (0,)), ("flush",)],
    ),
    (
        "(h) I3 single sweep",
        "h",
        (0, 1),
        [E(), P(None)],
        [("pub", (0, 1)), ("flush",), ("pub", (1,)), ("flush",)],
    ),
    (
        "(h) two event-loop threads share one handle",
        "h",
        (0, 1),
        [E(), E(), P(None)],
        [("pub", (0,)), ("flush",), ("pub", (1,)), ("flush",)],
    ),
]


def main():
    bad = 0
    idx = [int(a) for a in sys.argv[1:] if not a.startswith("--")] or range(len(CASES))
    for i in idx:
        name, variant, ets, threads, ops = CASES[i]
        cfg = {
            "variant": "d" if variant == "d-norepost" else variant,
            "e_targets": ets,
            "threads": threads,
            "prod_ops": ops,
            "repost": variant != "d-norepost",
            "always_drain": "--v1" not in sys.argv,
        }
        n, v = explore(cfg)
        kinds = sorted({k for k, _ in v})
        print(f"{name:55s} states={n:8d} violations={len(v)} {kinds}")
        if v:
            k, (g, ths) = v[0]
            print("    first:", k, "g=", g, "threads=", ths)
            if variant == "d":
                bad += 1
    print("design violations:", bad)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
