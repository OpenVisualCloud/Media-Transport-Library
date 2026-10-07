#!/usr/bin/env python3
"""final_model.py (v2): every interleaving, under sequential consistency, of the two paths of
core.md §6.1 and §6.6, with their step labels.  Each atomic operation and each syscall is one step.

Object path (core.md §6.1).  The event word ec = {seq, one WT count per lane}.  A producer bumps seq
with one RMW (E2) and reads the counts; a WT waiter loads ec before its attempt (T2), arms with
CAS(ec, e, e + ONE) (T7), sleeps in futex_wait on seq only (the real 32-bit compare, T8) and
removes its own count (T9).  Row readers (MS2) also arm `want` in the slot's progress word by
CAS before T7 (RW2); the binding fires EVENT(L1) only when the rows reach `want` (RW1).  An
interrupt may be cleared again (N5's OFF: no E2), after which its waiter calls again (o7).
Mutant `bits` is the rejected counters design (core.md §6.1): a sleeper bit cleared by a second
producer RMW.

Queue path (core.md §6.6).  An attachment's H bit is in the object's ec.  A producer bumps seq (E2),
claims H with fetch_and (E4) and pushes with one CAS on the queue word {list, qgen, ARMED,
CLOSED} (P2); a push whose node carries another qgen, or that finds CLOSED, drops its node.  The
descriptor is never closed while the entry exists (closing a queue keeps it for the next queue
on the entry), so a late write is a spurious wake, never a write to a closed descriptor.  A
consumer takes the list and reports one node per call under the lock; when it finds nothing it
unlocks, resets the eventfd (A7), relocks and arms with CAS(list and local empty) (A8), then
re-checks the interrupt (A9).  A signal its reset took is written back on every exit but its own
-MTL_EAGAIN.  An arm (J5-J8, under the lock) loads seq, checks, and CASes H, or pushes an object
already ready.  A detach marks a node in flight DEAD, and the pop drops it.  A queue interrupt
may be cleared again (OFF), after which its consumer calls again (q11).

Posts (mtl_queue_post, cases p1-p5).  A poster publishes an item of its own source (SRC), then
posts with one CAS on the queue word: the post field set, ARMED claimed (then one write), the
generation and CLOSED checked in the same CAS (S2).  A consumer takes the post field with the list
(A4) and reports it first (A5); serving that report consumes every published item.  The arm (A8)
compares the post field too.  Close clears it (C3).

Checks: LOST (a terminal state with a sleeper whose condition holds), LEAK (a WT count that does
not match the threads counted), OVER (an attachment in two places), SPIN (a cycle in the state
graph: a run that never ends), XQ (a push lands on a queue reused since the object was armed),
RPT (a report popped after its detach returned), PAC (a post accepted by a closed or reused
queue).
Usage: python3 final_model.py [CASE ...] | --trace CASE [MUTANT ...]; exit 1 unless every case
gives its expected result and every mutant is reported by its cases.

NOT_MODELLED below lists the step labels of core.md §6.1 and §6.6 that no case runs, with the
reason; check_labels.py (task M0) subtracts them."""
import sys
import time

NOT_MODELLED = {
    "N1": "the mode and target checks: pure argument checks",
    "N0'": "the fork check: WH15",
    "N2": "errno and the entry load: no interleaving",
    "N3": "the state load: no interleaving",
    "T1": "the deadline: no interleaving",
    "T5": "the deadline check: a timeout is a W option (tmo)",
    "C3": "queue close after its calls: r4 models C1-C2",
    "C4": "queue close after its calls: r4 models C1-C2",
    "C5": "queue close after its calls: r4 models C1-C2",
    "X3": "a closing object's slots: QW8",
    "Y1": "object entry reuse: argued in core.md §6.1 (Lifetime, the generation in intr); WH12, WC3",
    "Y2": "queue entry reuse beyond r3, r4",
    "J1": "arm validation before J7: lock serialised",
    "J2": "arm validation before J7: lock serialised",
    "J3": "arm validation before J7: lock serialised",
    "J4": "arm validation before J7: lock serialised",
    "J5": "arm validation before J7: lock serialised",
    "J6": "arm validation before J7: lock serialised",
    "DV1": "the delivery count: QW17",
    "DV2": "the delivery count: QW17",
    "DV3": "the delivery count: QW17",
}

# ----------------------------------------------------------------- object path ------------
# g = (seq, c0, c1, r0, r1, intr, closing, fired, fq, rows, want)
SEQ, C0, C1, R0, R1, INTR, CLOSING, FIRED, FQ, ROWS, WANT = range(11)


def setg(g, **kw):
    g = list(g)
    for k, v in kw.items():
        g[globals()[k]] = v
    return tuple(g)


def lanes(m):
    return [l for l in (0, 1) if m >> l & 1]


def fwake(g, ths, bits, kinds, par):
    """FUTEX_WAKE_BITSET(bits, INT_MAX): every queued waiter of a lane in bits returns."""
    if not bits:
        return g, ths
    woke = [t for t in g[FQ] if bits >> par[t]["lane"] & 1]
    if not woke:
        return g, ths
    ths = list(ths)
    for t in woke:
        ths[t] = ("T9",) + ths[t][1:]
    return setg(g, FQ=g[FQ] - frozenset(woke)), tuple(ths)


def o_succ(s, t, kinds, par, mut):
    g, ths = s
    k, p, loc = kinds[t], par[t], ths[t]
    out = []

    def put(g2, loc2, ths2=None):
        ths2 = list(ths if ths2 is None else ths2)
        ths2[t] = loc2
        out.append((g2, tuple(ths2)))

    cnt = lambda gg, l: gg[C0 + l]
    if k in ("P", "A"):  # tasklet producer (marks, then the loop flushes) or a direct waker
        pc, i, w = loc
        units = p["units"]
        nxt = ("pub", i + 1, 0) if i + 1 < len(units) else ("done", 0, 0)
        if pc == "done":
            return out
        F = units[i]
        if pc == "pub":  # E1: publish
            g2 = g
            for l in lanes(F):
                g2 = setg(g2, **{"R%d" % l: g2[R0 + l] + 1})
            put(g2, ("rmw", i, 0))
        elif pc == "rmw":  # E2: one RMW on ec returns the counts
            w = sum(1 << l for l in lanes(F) if cnt(g, l) > 0)
            if "producer_skip" in mut and not w:
                put(g, nxt)  # a load that sees no waiter skips the RMW
            elif "bits" in mut:
                put(setg(g, SEQ=g[SEQ] + 1), ("clr", i, w) if w else nxt)
            else:
                put(setg(g, SEQ=g[SEQ] + 1), (("mark" if k == "P" else "fw"), i, w) if w else nxt)
        elif pc == "clr":  # bits: the second RMW clears the bits it saw
            g2 = g
            for l in lanes(w):
                g2 = setg(g2, **{"C%d" % l: 0})
            put(g2, (("mark" if k == "P" else "fw"), i, w))
        elif pc == "mark":  # E3 -> M1: fired |= lanes (the loop's bitmap)
            put(setg(g, FIRED=g[FIRED] | w), ("fx", i, 0))
        elif pc == "fx":  # F6: the flush takes fired after the handlers
            F2 = g[FIRED]
            put(setg(g, FIRED=0), ("fw", i, F2) if F2 else nxt)
        elif pc == "fw":  # K1: FUTEX_WAKE_BITSET
            g2, ths2 = fwake(g, ths, w, kinds, par)
            put(g2, nxt, ths2)
        return out
    if k == "B":  # RW1: the RX binding publishes one more row per step
        pc, i, w = loc
        if pc == "done":
            return out
        nxt = ("b1", i + 1, 0) if i + 1 < p["steps"] else ("done", 0, 0)
        if pc == "b1":  # old = fetch_add(progress, 1)
            rows = g[ROWS] + 1
            fire = g[WANT] and rows >= g[WANT]
            put(setg(g, ROWS=rows), ("b2", i, 0) if fire else nxt)
        elif pc == "b2":  # fetch_and(progress, ~want): this step fires
            put(setg(g, WANT=0), ("b3", i, 0))
        elif pc == "b3":  # E2 on ec for lane 1
            w = 2 if cnt(g, 1) > 0 else 0
            put(setg(g, SEQ=g[SEQ] + 1), ("b4", i, w) if w else nxt)
        elif pc == "b4":  # K1
            g2, ths2 = fwake(g, ths, w, kinds, par)
            put(g2, nxt, ths2)
        return out
    if k in ("I", "C"):  # N5 then E2, K1 (interrupt, lanes m); CLOSE: CLOSING then E2, K1
        pc, w = loc
        m = p.get("m", 3)
        if pc == "set":
            put(setg(g, INTR=g[INTR] | m) if k == "I" else setg(g, CLOSING=1), ("rmw", 0))
        elif pc == "rmw":
            w = sum(1 << l for l in lanes(m) if cnt(g, l) > 0)
            g2 = g if ("intr_no_bump" in mut and k == "I") else setg(g, SEQ=g[SEQ] + 1)
            put(g2, ("fw", w) if w else ("done", 0))
        elif pc == "fw":
            g2, ths2 = fwake(g, ths, w, kinds, par)
            put(g2, ("off" if p.get("off") else "done", 0), ths2)
        elif pc == "off":  # N5 with MTL_INTR_OFF: clears the targets, no E2
            put(setg(g, INTR=g[INTR] & ~m), ("done", 0))
        return out
    # W: a call with a timeout on one lane, k units; may time out once (tmo) or return
    # spuriously once (spur); the final attempt after a timeout is T4 with fin = 1.
    # R: mtl_rx_wait_rows on lane 1 for min rows: the progress word is part of its arm (RW2).
    pc, e, got, used, fin = loc
    l = p["lane"]
    b = 1 << l
    if pc == "X":  # returned -MTL_ECANCELED; calls again once the interrupt is cleared
        if all(ths[j][0] == "done" for j in range(len(ths)) if kinds[j] == "I") and not g[INTR] & b:
            put(g, ("T2", None, got, used, fin))
        return out
    if pc in ("done", "S"):
        if pc == "S":
            if p.get("tmo") and not used:
                put(setg(g, FQ=g[FQ] - {t}), ("T9", e, got, 1, 1))
            if p.get("spur") and not used:
                put(setg(g, FQ=g[FQ] - {t}), ("T9", e, got, 1, 0))
        return out
    word = (g[SEQ], g[C0], g[C1])
    prog = (g[ROWS], g[WANT])
    if pc == "T2":  # T2: load ec before the attempt (R: and the progress word)
        if "load_after_attempt" in mut:
            put(g, ("T3", None, got, used, fin))
        else:
            put(g, ("T3", (word, prog), got, used, fin))
    elif pc == "T3":  # T3: codes
        if g[INTR] & b and not g[CLOSING] and p.get("again"):
            put(g, ("X", e, got, used, fin))
        elif g[CLOSING] or g[INTR] & b:
            put(g, ("done", e, got, used, fin))
        else:
            put(g, ("T4", e, got, used, fin))
    elif pc == "T4":  # T4: the attempt
        if k == "R":
            if g[ROWS] >= p["min"]:
                put(g, ("done", e, got, used, fin))
            else:
                put(g, ("RW2", e, got, used, fin))
        elif g[R0 + l] > 0:
            g2 = setg(g, **{"R%d" % l: g[R0 + l] - 1})
            put(g2, ("done" if got + 1 == p["k"] else "T2", None, got + 1, used, 0))
        elif fin:
            put(g, ("done", e, got, used, fin))
        elif e is None:  # mutant: the load after the attempt, a step of its own
            put(g, ("T6", None, got, used, fin))
        else:
            put(g, ("T7", e, got, used, fin))
    elif pc == "RW2":  # RW2: CAS(progress, p, p with want = min(want, min_rows))
        m = p["min"]
        if "want_store" in mut:
            put(setg(g, WANT=m), ("T7", e, got, used, fin))
        elif prog != e[1]:
            put(g, ("T2", None, got, used, fin))
        else:
            nw = m if not g[WANT] else min(g[WANT], m)
            put(setg(g, WANT=nw), ("T7", e, got, used, fin))
    elif pc == "T6":  # mutant only: the load of ec after the attempt
        put(g, ("T7", (word, prog), got, used, fin))
    elif pc == "T7":  # T7: CAS(ec, e, e + ONE(l))
        if "no_arm" in mut:  # the mutant sleeps without arming
            put(g, ("T8", e, got, used, fin))
        elif word != e[0]:
            put(g, ("T2", None, got, used, fin))
        elif "bits" in mut:
            put(setg(g, **{"C%d" % l: 1}), ("T8", e, got, used, fin))
        else:
            put(setg(g, **{"C%d" % l: g[C0 + l] + 1}), ("T8", e, got, used, fin))
    elif pc == "T8":  # T8: futex_wait on seq only (the kernel compares 32 bits)
        if g[SEQ] == e[0][0]:
            put(setg(g, FQ=g[FQ] | {t}), ("S", e, got, used, fin))
        else:
            put(g, ("T9", e, got, used, fin))
    elif pc == "T9":  # T9: the waiter removes its own count
        if "bits" in mut or "no_dec" in mut or "no_arm" in mut:
            put(g, ("T2", None, got, used, fin))
        else:
            put(setg(g, **{"C%d" % l: g[C0 + l] - 1}), ("T2", None, got, used, fin))
    return out


def o_check(s, kinds, par, mut):
    g, ths = s
    bad = []
    for t, loc in enumerate(ths):
        if kinds[t] in ("W", "R") and loc[0] == "S":
            l = par[t]["lane"]
            ready = g[ROWS] >= par[t]["min"] if kinds[t] == "R" else g[R0 + l]
            if ready or g[CLOSING] or g[INTR] >> l & 1:
                bad.append("LOST")
    if "bits" not in mut and "no_arm" not in mut:
        for l in (0, 1):
            n = sum(1 for t, loc in enumerate(ths) if kinds[t] in ("W", "R") and par[t]["lane"] == l
                    and loc[0] in ("T8", "S", "T9"))
            if g[C0 + l] != n:
                bad.append("LEAK")
    return bad


def o_init(kinds, par):
    g = (0, 0, 0, 0, 0, 0, 0, 0, frozenset(), 0, 0)
    ths = []
    for k in kinds:
        ths.append({"P": ("pub", 0, 0), "A": ("pub", 0, 0), "I": ("set", 0), "C": ("set", 0),
                    "B": ("b1", 0, 0), "W": ("T2", None, 0, 0, 0), "R": ("T2", None, 0, 0, 0)}[k])
    return (g, tuple(ths))


# ----------------------------------------------------------------- queue path -------------
# g = (r, seq, H, ast, aqg, lst, local, armed, closed, intr, qgen, fd, lock, cons, viol, det,
#      pst, src): pst the post field of the queue word, src the posters' published items
# ast: the attachment state per object, 0 FREE, 1 IDLE, 2 ARMED (armed or in flight), 3 DEAD
# aqg: the qgen stored in alink when the object was armed
(GR, GSEQ, GH, AST, AQG, LST, LOC, ARM, CLO, QINT, QGEN, FD, LOCK, CONS, VIOL,
 DET, PST, SRC) = range(18)
XQ_BIT, RPT_BIT, PAC_BIT = 1, 2, 4


def qset(g, **kw):
    g = list(g)
    for k, v in kw.items():
        g[globals()[k]] = v
    return tuple(g)


def tset(t, i, v):
    t = list(t)
    t[i] = v
    return tuple(t)


def q_succ(s, t, kinds, par, mut, total):
    g, ths = s
    k, p, loc = kinds[t], par[t], ths[t]
    out = []

    def put(g2, loc2):
        out.append((g2, tset(ths, t, loc2)))

    def stale(g, o):  # P2: CLOSED, or the node was armed on an earlier queue of this entry
        return g[CLO] or (g[AQG][o] != g[QGEN] and "no_qgen" not in mut)

    def push(g, o):
        """P2: returns (g', owes_write), or (g', None) when the claimer drops its node."""
        if stale(g, o):
            return qset(g, AST=tset(g[AST], o, 0)), None
        v = g[VIOL] | (XQ_BIT if g[AQG][o] != g[QGEN] else 0)
        g2 = qset(g, LST=g[LST] + (o,), VIOL=v)
        if g[ARM]:
            return qset(g2, ARM=0), 1
        return g2, 0

    if k in ("P", "A"):  # producer: P defers its write to the loop's flush, A writes itself
        pc, i = loc
        units = p["units"]
        if pc == "done":
            return out
        o = units[i]
        nxt = ("pub", i + 1) if i + 1 < len(units) else ("done", 0)
        if pc == "pub":  # E1
            put(qset(g, GR=tset(g[GR], o, g[GR][o] + 1)), ("rmw", i))
        elif pc == "rmw":  # E2: fetch_add(ec) returns H
            g2 = qset(g, GSEQ=tset(g[GSEQ], o, g[GSEQ][o] + 1))
            if g[GH][o]:
                put(g2, ("push", i) if "claim_by_load" in mut else ("clm", i))
            else:
                put(g2, nxt)
        elif pc == "clm":  # E4: fetch_and(ec, ~H): the winner pushes
            if g[GH][o]:
                put(qset(g, GH=tset(g[GH], o, 0)), ("push", i))
            else:
                put(g, nxt)
        elif pc == "push":  # P2
            g2, owe = push(g, o)
            put(g2, ("wr", i) if owe else nxt)
        elif pc == "wr":  # K2: the owed write (the loop's flush for P, at once for A)
            put(qset(g, FD=min(g[FD] + 1, 3)), nxt)
        return out
    if k == "QP":  # mtl_queue_post on an application thread (or a signal handler)
        pc, i = loc
        if pc == "done":
            return out
        late = "post_before_publish" in mut  # the mutant posts first, then publishes
        nxt = ("pc" if late else "pub", i + 1) if i + 1 < p["n"] else ("done", 0)
        after = ("publ", i) if late else nxt
        if pc in ("pub", "publ"):  # the application publishes an item of its source
            put(qset(g, SRC=g[SRC] + 1), ("pc", i) if pc == "pub" else nxt)
        elif pc == "pc":  # S2: CAS(qword) with the qgen and CLOSED checks
            if (g[CLO] or g[QGEN] != 0) and "post_no_check" not in mut:
                put(g, after)  # -MTL_EBADF: nothing posted
            else:
                v = g[VIOL] | (PAC_BIT if g[CLO] or g[QGEN] != 0 else 0)
                if g[ARM] and "post_no_claim" not in mut:  # the post field set, ARMED claimed
                    put(qset(g, PST=1, ARM=0, VIOL=v), ("pw", i))
                else:
                    put(qset(g, PST=1, VIOL=v), after)
        elif pc == "pw":  # S3: K2, the write, directly (never M1)
            put(qset(g, FD=min(g[FD] + 1, 3)), after)
        return out
    if k == "QI":  # mtl_queue_interrupt: N5 stores intr, U2 claims ARMED, K2 writes
        pc = loc[0]
        if pc == "i1":
            put(qset(g, QINT=1), ("i2",))
        elif pc == "i2":
            if "intr_no_claim" in mut or not g[ARM]:
                put(g, ("done",))
            else:
                put(qset(g, ARM=0), ("i3",))
        elif pc == "i3":
            put(qset(g, FD=min(g[FD] + 1, 3)), ("i4",) if p.get("off") else ("done",))
        elif pc == "i4":  # MTL_INTR_OFF
            put(qset(g, QINT=0), ("done",))
        return out
    if k == "QC":  # close: C1 (CLOSED, claim ARMED, write), C2 (free nodes, detach), C3 reuse
        pc = loc[0]
        if pc == "c1":
            if g[ARM]:
                put(qset(g, CLO=1, ARM=0), ("c1w",))
            else:
                put(qset(g, CLO=1), ("c2",))
        elif pc == "c1w":
            put(qset(g, FD=min(g[FD] + 1, 3)), ("c2",))
        elif pc == "c2":  # wait for q's calls (C1's write woke them), then, under the lock,
            # free the listed nodes and the armed slots
            inside = any(kinds[j] in ("E", "T", "O") and (ths[j][0] not in ("ep", "done", "stop")
                         or (kinds[j] == "T" and ths[j][0] == "ep")) for j in range(len(ths)))
            if g[LOCK] == -1 and (not inside or "close_no_wait" in mut):
                n = len(g[GR])
                ast = list(g[AST])
                gh = list(g[GH])
                for o in range(n):
                    if o in g[LST] or o in g[LOC]:
                        ast[o] = 0
                    if gh[o]:  # the walk's fetch_and won: the slot is freed
                        gh[o] = 0
                        ast[o] = 0
                put(qset(g, LST=(), LOC=(), AST=tuple(ast), GH=tuple(gh), PST=0),
                    ("c3",) if p.get("reuse") else ("done",))
        elif pc == "c3":  # a new queue on the entry: create resets head, qgen + 1, the fd kept
            put(qset(g, CLO=0, QGEN=g[QGEN] + 1, FD=0, ARM=0, QINT=0), ("done",))
        return out
    if k == "D":  # mtl_queue_arm(q, o, 0, 0) on another thread: J4a under the lock
        pc = loc[0]
        o = p["o"]
        if pc == "d1":
            if g[LOCK] == -1:
                put(qset(g, LOCK=t), ("d2",))
        elif pc == "d2":
            a = g[AST][o]
            if a == 2 and g[GH][o]:  # armed: the fetch_and of H wins
                g2 = qset(g, GH=tset(g[GH], o, 0), AST=tset(g[AST], o, 0))
            elif a == 2:  # in flight (claimed, listed or local): DEAD, dropped by its pop
                g2 = qset(g, AST=tset(g[AST], o, 0 if "no_dead" in mut else 3))
            elif a == 1:
                g2 = qset(g, AST=tset(g[AST], o, 0))
            else:
                g2 = g
            put(qset(g2, LOCK=-1, DET=1), ("done",))
        return out
    # E: an event loop that keeps the rule; T: the same, sleeping inside a timed wait (A11);
    # O: a one-off poller (one call, then leaves)
    pc, o, got, eto = loc
    me = t
    loops = ("E", "T")
    if pc == "stop" and p.get("again") and o is None and not g[QINT] and all(
            ths[j][0] == "done" for j in range(len(ths)) if kinds[j] == "QI"):
        put(g, ("w0", None, 0, eto))  # calls again once the interrupt is cleared
        return out
    if pc in ("done", "stop"):
        return out

    def L(pc2, o2=o, got2=got, eto2=eto):
        return (pc2, o2, got2, eto2)

    def leave_code(g):  # an exit with a code: write back the signal A7 took (RV-1)
        if got and "no_exit_resignal" not in mut:
            g = qset(g, FD=min(g[FD] + 1, 3))
        return put(qset(g, LOCK=-1), L("stop", None, 0))

    if pc == "w0":  # A1-A2: enter, lock
        if k in loops and g[CONS] == total and not p.get("forever"):
            put(g, L("done"))
        elif g[LOCK] == -1:
            put(qset(g, LOCK=me), L("w1"))
    elif pc == "w1":  # A3-A5, under the lock
        if g[QGEN] != 0:  # A3: the handle's generation is not the entry's: -MTL_EBADF
            leave_code(g)
        elif g[CLO] or g[QINT]:  # A3: -MTL_ESHUTDOWN, -MTL_ECANCELED
            leave_code(g)
        elif g[LOC]:  # A5: one report from the local chain
            o2 = g[LOC][0]
            g2 = qset(g, LOC=g[LOC][1:])
            if g[AST][o2] == 3:  # a DEAD node: freed, never reported
                put(qset(g2, AST=tset(g[AST], o2, 0)), L("w1"))
            else:
                v = g[VIOL] | (RPT_BIT if g[DET] else 0)  # the detacher (D) detaches object 0
                put(qset(g2, AST=tset(g[AST], o2, 1), LOCK=-1, VIOL=v), L("sv", o2, 0))
        elif g[PST]:  # A4: take the list and the post field; A5: the post report first
            put(qset(g, LOC=g[LOC] + g[LST], LST=(), PST=0, LOCK=-1), L("svp", None, 0))
        elif g[LST]:  # A4: take the whole list
            put(qset(g, LOC=g[LOC] + g[LST], LST=()), L("w1"))
        else:  # A6: nothing: unlock before the syscall
            put(qset(g, LOCK=-1), L("arm0" if "reset_after_arm" in mut else "rd", None, 0))
    elif pc == "rd":  # A7: the reset, outside the lock; got = it took a signal
        if "no_reset" in mut:
            put(g, L("rl", None, 0))
        else:
            put(qset(g, FD=0), L("rl", None, 1 if g[FD] else 0))
    elif pc in ("rl", "arm0"):  # relock
        if g[LOCK] == -1:
            put(qset(g, LOCK=me), L("arm" if pc == "rl" else "armx"))
    elif pc in ("arm", "armx"):  # A8: CAS(qword: list empty -> ARMED), local empty too
        if g[CLO] or g[QGEN] != 0:
            put(g, L("w1"))  # w1 leaves with the code and writes back got
        elif "no_arm" in mut and pc == "arm":  # the mutant sleeps without arming
            put(g, L("chk"))
        elif (not g[LST] and not g[LOC] and (not g[PST] or "arm_ignores_post" in mut)) \
                or "arm_store" in mut:
            g2 = qset(g, ARM=1)
            put(g2, L("rd2" if pc == "armx" else "chk"))
        else:  # work came: write back the signal the reset took
            if got and "no_resignal" not in mut:
                put(qset(g, FD=min(g[FD] + 1, 3)), L("w1", None, 0))
            else:
                put(g, L("w1", None, 0))
    elif pc == "rd2":  # mutant: the reset after the arm, a step of its own
        put(qset(g, FD=0), L("chk", None, 0))
    elif pc == "chk":  # A9: the interrupt pair, a seq_cst load of intr after the arm
        if g[QINT] and "no_intr_recheck" not in mut:
            put(g, L("w1"))  # w1 leaves with -MTL_ECANCELED and writes back got
        else:  # -MTL_EAGAIN: the only exit that keeps the signal (it armed)
            put(qset(g, LOCK=-1), L("ep" if k in loops else "done", None, 0))
    elif pc == "ep":  # A10, then epoll_wait (E) or A11 ppoll (T), level; may time out once
        if g[FD] > 0:
            put(g, L("w0", None, 0))
        if p.get("eto") and not eto:
            put(g, L("w0", None, 0, eto2=1))
    elif pc == "svp":  # the application serves a report of posts: every published item
        put(qset(g, CONS=g[CONS] + g[SRC], SRC=0), L("w0" if k in loops else "done"))
    elif pc == "sv":  # the application serves one unit of the reported object
        g2 = g
        if g[GR][o] > 0:
            g2 = qset(g, GR=tset(g[GR], o, g[GR][o] - 1), CONS=g[CONS] + 1)
        put(g2, L("ra0"))
    elif pc == "ra0":  # mtl_queue_arm: J1 lock, J2 CLOSED or stale, J3 the slot
        if g[LOCK] == -1:
            if g[CLO] or g[QGEN] != 0 or g[AST][o] != 1:  # closed, or detached meanwhile
                put(g, L("w0" if k in loops else "done", None, 0))
            else:  # J6 the slot ARMED (alink's qgen), J7 e = load(ec)
                put(qset(g, LOCK=me, AST=tset(g[AST], o, 2), AQG=tset(g[AQG], o, g[QGEN])),
                    L("ra1", o, g[GSEQ][o]))
    elif pc == "ra1":  # J7: ready now? then J8 pushes it at once
        if g[GR][o] > 0:
            g2, owe = push(g, o)
            put(qset(g2, LOCK=-1), L("spw" if owe else ("w0" if k in loops else "done"), None, 0))
        else:
            put(g, L("ra2", o, got))
    elif pc == "ra2":  # J8: CAS(ec, e, e | H); J10 unlock
        if g[GSEQ][o] == got or "arm_no_validate" in mut:
            put(qset(g, GH=tset(g[GH], o, 1), LOCK=-1), L("w0" if k in loops else "done", None, 0))
        else:
            put(g, L("ra1", o, g[GSEQ][o]))
    elif pc == "spw":  # the owed write of an at-once push, after J10's unlock
        put(qset(g, FD=min(g[FD] + 1, 3)), L("w0" if k in loops else "done", None, 0))
    return out


def q_check(s, kinds, par, mut, nobj, terminal):
    g, ths = s
    bad = []
    if g[VIOL] & XQ_BIT:
        bad.append("XQ")
    if g[VIOL] & RPT_BIT:
        bad.append("RPT")
    if g[VIOL] & PAC_BIT:
        bad.append("PAC")
    for o in range(nobj):  # each attachment in exactly one place
        n = g[LST].count(o) + g[LOC].count(o) + g[GH][o]
        for j, loc in enumerate(ths):
            if kinds[j] in ("P", "A") and loc[0] == "push" and par[j]["units"][loc[1]] == o:
                n += 1
            if kinds[j] in ("E", "T", "O") and loc[1] == o and loc[0] in ("sv", "ra0", "ra1", "ra2"):
                n += 1
        if n > 1:
            bad.append("OVER")
    if terminal:
        for j, loc in enumerate(ths):
            if kinds[j] in ("E", "T") and loc[0] == "ep" and not g[FD]:
                owed = any(r and g[AST][o] == 2 for o, r in enumerate(g[GR]))
                if g[LST] or g[LOC] or owed or g[CLO] or g[QINT] or g[QGEN] or g[PST] or g[SRC]:
                    bad.append("LOST")
    return bad


def q_init(kinds, par, nobj, mut=()):
    g = ((0,) * nobj, (0,) * nobj, (1,) * nobj, (2,) * nobj, (0,) * nobj, (), (), 0, 0, 0, 0, 0,
         -1, 0, 0, 0, 0, 0)
    ths = []
    for k in kinds:
        ths.append({"P": ("pub", 0), "A": ("pub", 0), "QP": ("pc", 0) if "post_before_publish" in mut
                    else ("pub", 0), "QI": ("i1",), "QC": ("c1",), "D": ("d1",),
                    "E": ("w0", None, 0, 0), "T": ("w0", None, 0, 0), "O": ("w0", None, 0, 0)}[k])
    return (g, tuple(ths))


# ----------------------------------------------------------------- explorer ----------------
def explore(case, mut, limit=4_000_000, trace=False):
    path, spec = case[0], case[1]
    kinds = [x[0] for x in spec]
    par = [x[1] for x in spec]
    if path == "obj":
        s0 = o_init(kinds, par)
        succ = lambda s, t: o_succ(s, t, kinds, par, mut)
        check = lambda s, term: o_check(s, kinds, par, mut) if term else []
    else:
        nobj = case[2]
        total = sum(len(p["units"]) for k, p in zip(kinds, par) if k in ("P", "A"))
        total += sum(p["n"] for k, p in zip(kinds, par) if k == "QP")
        s0 = q_init(kinds, par, nobj, mut)
        succ = lambda s, t: q_succ(s, t, kinds, par, mut, total)
        check = lambda s, term: q_check(s, kinds, par, mut, nobj, term)
    # depth-first, with the colours of a cycle check: a cycle is a run that never ends (SPIN)
    parent = {s0: None}
    colour = {}
    stack = [(s0, None)]
    bad = set()
    first = None
    while stack:
        s, it = stack[-1]
        if it is None:
            nxt = [s2 for t in range(len(kinds)) for s2 in succ(s, t)]
            b = check(s, not nxt)
            if b and first is None:
                first = s
            bad.update(b)
            colour[s] = 1
            it = iter(nxt)
            stack[-1] = (s, it)
        for s2 in it:
            c = colour.get(s2)
            if c is None:
                parent[s2] = s
                stack.append((s2, None))
                break
            if c == 1:
                bad.add("SPIN")
                if first is None:
                    first = s
        else:
            colour[s] = 2
            stack.pop()
        if len(parent) > limit:
            raise RuntimeError("state limit")
    if trace and first is not None:
        path = []
        while first is not None:
            path.append(first)
            first = parent[first]
        for g, ths in reversed(path):
            print("   ", g, ths)
    return len(parent), bad


W = lambda lane, k=1, **kw: ("W", dict(lane=lane, k=k, **kw))
R = lambda mn: ("R", dict(lane=1, min=mn))
B = lambda steps: ("B", dict(steps=steps))
P = lambda *u: ("P", dict(units=list(u)))
A = lambda *u: ("A", dict(units=list(u)))
E = lambda **kw: ("E", dict(kw))
T = lambda **kw: ("T", dict(kw))
O = ("O", {})
QP = lambda n=1: ("QP", dict(n=n))
CASES = {  # name: (case, expected bad set)
    "o1_two_waiters_timeout": (("obj", [P(1, 1), W(0, 1, tmo=1), W(0, 2)]), set()),
    "o2_lanes_spurious":      (("obj", [P(1, 2), W(0, 1, spur=1), W(1, 1)]), set()),
    "o3_app_producer":        (("obj", [P(1), A(1), W(0, 2, spur=1)]), set()),
    "o4_interrupt_one_lane":  (("obj", [P(2), ("I", dict(m=1)), W(0), W(1)]), set()),
    "o5_close":               (("obj", [P(1), ("C", {}), W(0, 2), W(1)]), set()),
    "o6_rows_two_readers":    (("obj", [B(2), R(1), R(3)]), set()),
    "o7_interrupt_off":       (("obj", [P(1), ("I", dict(m=1, off=1)), W(0, 1, again=1)]), set()),
    "q1_loop_three_units":    (("q", [P(0, 0, 0), E(eto=1)], 1), set()),
    "q2_two_objects":         (("q", [P(0, 1, 0), E()], 2), set()),
    "q3_poller_steals":       (("q", [P(0, 0), E(), O], 1), set()),
    "q4_app_and_tasklet":     (("q", [P(0, 1), A(0), E()], 2), set()),
    "q5_two_loops":           (("q", [P(0, 0), E(), E(eto=1)], 1), set()),
    "q6_queue_interrupt":     (("q", [P(0, 0), E(forever=1), ("QI", {})], 1), set()),
    "q7_queue_close":         (("q", [P(0, 0), E(forever=1), ("QC", {})], 1), set()),
    "q8_mixed":               (("q", [P(0, 1, 1), A(0), E(eto=1), O], 2), set()),
    "q9_two_loops_poller":    (("q", [P(0, 0, 0), E(), E(eto=1), O], 1), set()),
    "q10_detach_race":        (("q", [P(0, 0), E(forever=1), ("D", dict(o=0))], 1), set()),
    "q11_interrupt_off":      (("q", [P(0), E(forever=1, again=1), ("QI", dict(off=1))], 1), set()),
    "r1_intr_after_steal":    (("q", [P(0), E(forever=1), E(eto=1), O, ("QI", {})], 1), set()),
    "r2_close_after_steal":   (("q", [P(0), T(forever=1), E(eto=1), O, ("QC", {})], 1), set()),
    "r3_stale_claim_reuse":   (("q", [A(0), T(forever=1), ("QC", {"reuse": 1})], 1), set()),
    "r4_close_reuse_sleeper": (("q", [P(0), T(forever=1), ("QC", {"reuse": 1})], 1), set()),
    "p1_post_and_object":     (("q", [P(0), QP(1), E()], 1), set()),
    "p2_posts_coalesce":      (("q", [QP(2), QP(1), E(eto=1)], 1), set()),
    "p3_post_steal":          (("q", [QP(2), E(), E(eto=1), O], 1), set()),
    "p4_post_interrupt_off":  (("q", [QP(1), E(forever=1, again=1), ("QI", dict(off=1))], 1), set()),
    "p5_post_close_reuse":    (("q", [QP(1), T(forever=1), ("QC", {"reuse": 1})], 1), set()),
}
MUTANTS = {  # name: (mutation, cases that must report it, the kind expected)
    "bits (the counters design: a second RMW clears the bit)": ("bits", ["o1_two_waiters_timeout"], "LOST"),
    "no_arm (T7 or A8 skipped: sleep without arming)":     ("no_arm", ["o1_two_waiters_timeout", "q1_loop_three_units"], "LOST"),
    "load_after_attempt (T2 after T4)":                    ("load_after_attempt", ["o3_app_producer"], "LOST"),
    "producer_skip (no RMW when no count is seen)":        ("producer_skip", ["o3_app_producer"], "LOST"),
    "intr_no_bump (an interrupt that only loads ec)":      ("intr_no_bump", ["o4_interrupt_one_lane"], "LOST"),
    "no_dec (a leaving waiter keeps its count)":           ("no_dec", ["o2_lanes_spurious"], "LEAK"),
    "want_store (RW2 stores want, no CAS, no min)":        ("want_store", ["o6_rows_two_readers"], "LOST"),
    "no_resignal (A8 keeps a signal A7 took)":             ("no_resignal", ["q3_poller_steals"], "LOST"),
    "no_exit_resignal (a code exit keeps it)":             ("no_exit_resignal", ["r1_intr_after_steal", "r2_close_after_steal"], "LOST"),
    "reset_after_arm (A7 after A8)":                       ("reset_after_arm", ["q1_loop_three_units"], "LOST"),
    "no_reset (no A7)":                                    ("no_reset", ["q1_loop_three_units"], "SPIN"),
    "arm_store (ARMED without the empty-list compare)":    ("arm_store", ["q1_loop_three_units"], "LOST"),
    "arm_no_validate (J8 sets H without the CAS)":         ("arm_no_validate", ["q1_loop_three_units"], "LOST"),
    "claim_by_load (push without the fetch_and claim)":    ("claim_by_load", ["q1_loop_three_units"], "OVER"),
    "intr_no_claim (INTR without claiming ARMED)":         ("intr_no_claim", ["q6_queue_interrupt"], "LOST"),
    "no_intr_recheck (no A9 after the arm)":               ("no_intr_recheck", ["q6_queue_interrupt"], "LOST"),
    "no_qgen (P2 ignores the queue generation)":           ("no_qgen", ["r3_stale_claim_reuse"], "XQ"),
    "no_dead (detach frees a node in flight)":             ("no_dead", ["q10_detach_race"], "RPT"),
    "close_no_wait (C2 does not wait for q's calls)":      ("close_no_wait", ["r4_close_reuse_sleeper"], "LOST"),
    "post_no_claim (a post that never claims ARMED)":      ("post_no_claim", ["p1_post_and_object", "p2_posts_coalesce"], "LOST"),
    "arm_ignores_post (A8 arms over a pending post)":      ("arm_ignores_post", ["p2_posts_coalesce"], "LOST"),
    "post_no_check (S2 ignores CLOSED and the qgen)":      ("post_no_check", ["p5_post_close_reuse"], "PAC"),
    "post_before_publish (the rule: publish, then post)":  ("post_before_publish", ["p1_post_and_object"], "LOST"),
}


def main(argv):
    if argv[:1] == ["--trace"]:
        explore(CASES[argv[1]][0], tuple(argv[2:]), trace=True)
        return 0
    t0 = time.time()
    fail = 0
    names = argv or list(CASES)
    for name in names:
        n, bad = explore(CASES[name][0], ())
        ok = bad == CASES[name][1]
        fail |= not ok
        print("%-26s %8d states  %s" % (name, n, "ok" if ok else "FAIL " + ",".join(sorted(bad))))
    if not argv:
        for mname, (m, cases, kind) in MUTANTS.items():
            hits = []
            for c in cases:
                n, bad = explore(CASES[c][0], (m,))
                if kind in bad:
                    hits.append("%s:%s" % (c, ",".join(sorted(bad))))
            fail |= not hits or len(hits) < len(cases)
            print("%-52s %s" % (mname, "killed by " + " ".join(hits) if hits else "NOT KILLED"))
    print("%.1f s, %s" % (time.time() - t0, "PASS" if not fail else "FAIL"))
    return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
