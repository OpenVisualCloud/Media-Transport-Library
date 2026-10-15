#!/usr/bin/env python3
"""pin_ring.py: the results ring, the out-of-order hold (W, D-209) and the REPEAT pin (D-212), by
exhaustive search.

Entries in seq order (= pick order, G-08), each (st, parity, age, sent):
  Q queued; F in flight (the engine completes it; also a released pin whose repeat sends drain);
  p pinned, first send in flight; P pinned, recorded (HELD); D done, slot acquirable;
  W done out of order, slot held until every older entry is done (hold=True).
Done for the reaper: D, W.  Not done: Q, F, p, P.  Free slots = pool - L - #{Q,F,p,P} - #W.
Application: acquire (any thread, any time), release, submit (a full reap since its previous
submit: D-208's rule), reap, discard (seek).  Engine: pick the oldest Q (sent whole, dropped at
pick-up, or ended short when rows=True), complete a first send (p -> P), complete any F.
Pin rules:
  skip       no REPEAT.
  sent-only  a sent whole unit takes its parity's pin; drops and short units do not touch it;
             discard keeps it (rejected).
  window     after each pick-up the pins are the whole units (sent or dropped) among the last
             defer_step pick-ups, one per parity; every drop takes the pin; a short unit
             releases its parity's pin and takes none; discard releases every pin.
  D-212      the rule as decided: window, where a drop at pick-up is either TOO_LATE or
             SNAP_COLLISION (it takes the pin) or BEHIND (it releases its parity's pin and takes
             none, as a short unit); the engine may choose either at every pick-up.
  mutants of window: nodrop (drops take no pin and release their parity's), keepdiscard,
             shortkeeps (a short unit leaves the old pin), parity (no age rule: only a
             same-parity unit releases a pin).
tsq=True: the engine completes an F only after a newer unit was picked (no idle cleanup).
Checks: RING (an admitted-looking acquire meets resv - reap_seq >= N), DEADLOCK (no lease, no
acquirable slot, and no engine step or reap leads to one), EOS (with no lease and nothing more
submitted, the engine ends with more than defer_step unreported entries).
Pools: pins + 1 to 4 (a row may name fewer: a counterexample row stops at the smallest pools that
show it). Usage: python3 -I pin_ring.py; exit 1 unless every row gives its expected result."""
import sys
from collections import deque

NOTDONE = set("QFpP")


def free_slots(pool, L, ents):
    return pool - L - sum(1 for e in ents if e[0] in "QFpPW")


def settle(ents):
    out, ok = [], True
    for e in ents:
        if e[0] == "W" and ok:
            e = ("D", e[1], 0, e[3])
        out.append(e)
        ok = ok and e[0] not in NOTDONE
    return tuple(out)


def finish(ents, i, hold):  # entry i completes with nothing held: D, or W behind one not done
    older = any(e[0] in NOTDONE for e in ents[:i])
    st = "W" if (hold and older) else "D"
    return settle(ents[:i] + ((st, ents[i][1], 0, ents[i][3]),) + ents[i + 1:])


def release(e):
    return ("F", e[1], 0, e[3]) if e[0] in "pP" else e


def pick(ents, i, outcome, rule, ds, hold):
    """the oldest Q (index i) is picked with outcome sent/drop/behind/short"""
    par = ents[i][1]
    ents = list(ents)
    if rule == "skip":
        if outcome == "drop":
            return finish(tuple(ents), i, hold)
        ents[i] = ("F", par, 0, 1)
        return settle(tuple(ents))
    whole = outcome in ("sent", "drop")
    if rule == "sent-only":
        if outcome == "sent":
            ents = [release(e) if (e[0] in "pP" and e[1] == par) else e for e in ents]
            ents[i] = ("p", par, 0, 1)
            return settle(tuple(ents))
        if outcome == "drop":
            return finish(tuple(ents), i, hold)
        ents[i] = ("F", par, 0, 1)
        return settle(tuple(ents))
    # window, D-212 and the mutants
    takes = whole and not (rule == "nodrop" and outcome == "drop")
    out = []
    for e in ents:
        if e[0] in "pP":
            age = e[2] + 1
            gone = (rule != "parity" and age >= ds)
            if e[1] == par:  # a take replaces its parity's pin; a finish without one releases it
                gone = True
            if rule == "shortkeeps" and not takes:
                gone = False
            out.append(("F", e[1], 0, e[3]) if gone else (e[0], e[1], age, e[3]))
        else:
            out.append(e)
    if takes:
        out[i] = ("p", par, 0, 1) if outcome == "sent" else ("P", par, 0, 0)
        return settle(tuple(out))
    if outcome in ("drop", "behind"):
        return finish(tuple(out), i, hold)
    out[i] = ("F", par, 0, 1)
    return settle(tuple(out))


def engine_moves(ents, rule, ds, hold, rows, tsq):
    qs = [i for i, e in enumerate(ents) if e[0] == "Q"]
    if qs:
        i = qs[0]
        for oc in ("sent", "drop") + (("behind",) if rule == "D-212" else ()) + \
                (("short",) if rows else ()):
            yield pick(ents, i, oc, rule, ds, hold)
    for i, e in enumerate(ents):
        if e[0] == "p":
            yield settle(ents[:i] + (("P", e[1], e[2], e[3]),) + ents[i + 1:])
        elif e[0] == "F":
            if tsq and not any(x[0] in "pP" for x in ents) and not any(x[3] for x in ents[i + 1:]):
                continue  # no later traffic (no repeat, no newer send): on a TSQ without cleanup it never completes
            yield finish(ents, i, hold)


def reap(ents):
    r = 0
    while r < len(ents) and ents[r][0] in "DW":
        r += 1
    return ents[r:]


def run(pool, N, rule, ds, hold, rows=False, tsq=False, discard=True):
    start = (0, (), 0)
    seen, todo = {start}, deque([start])
    ring = False
    while todo:
        L, ents, k = todo.popleft()
        nxt = []
        if free_slots(pool, L, ents) > 0:
            if L + len(ents) >= N:
                ring = True
            else:
                nxt.append((L + 1, ents, k))
        if L > 0:
            nxt.append((L - 1, ents, k))
            if k == 0:
                for par in range(ds):
                    nxt.append((L - 1, ents + (("Q", par, 0, 0),), 1))
        nxt.append((L, reap(ents), 0))
        if discard and any(e[0] in "QpP" for e in ents):
            d = list(ents)
            if rule not in ("skip", "sent-only", "keepdiscard"):
                d = [release(e) for e in d]
            d = tuple(d)
            for i in range(len(d)):
                if d[i][0] == "Q":
                    d = finish(d, i, hold)
            nxt.append((L, d, k))
        for e2 in engine_moves(ents, rule, ds, hold, rows, tsq):
            nxt.append((L, e2, k))
        for s in nxt:
            if s not in seen:
                seen.add(s)
                todo.append(s)
    # liveness and EOS over every reachable state without a lease: the engine and the reaper
    # alone (no application step) must reach an acquirable slot (else DEADLOCK), and must not
    # end with more than defer_step unreported entries (else EOS). One graph, two backward
    # searches.
    starts = {ents for L, ents, k in seen if L == 0}
    succ, q = {}, deque(starts)
    while q:
        e = q.popleft()
        if e in succ:
            continue
        nx = set(engine_moves(e, rule, ds, hold, rows, tsq))
        succ[e] = nx | {reap(e)}
        q.extend(x for x in succ[e] if x not in succ)
    pred = {}
    for e, nx in succ.items():
        for x in nx:
            pred.setdefault(x, []).append(e)

    def back(targets):
        got, q = set(targets), deque(targets)
        while q:
            for e in pred.get(q.popleft(), ()):
                if e not in got:
                    got.add(e)
                    q.append(e)
        return got

    good = back([e for e in succ if free_slots(pool, 0, e) > 0 and len(e) < N])
    stuck = back([e for e in succ if not list(engine_moves(e, rule, ds, hold, rows, tsq)) and
                  sum(1 for x in e if x[0] in NOTDONE) > ds])
    dead = any(e not in good for e in starts)
    eos = any(e in stuck for e in starts)
    return ring, dead, eos, len(seen)


CASES = [
    # label, kwargs, expected (ring, dead, eos)
    ("skip, hold, 2P+1", dict(rule="skip", ds=1, hold=True), (False, False, False)),
    ("sent-only, no hold, P+1 (D-207)", dict(rule="sent-only", ds=1, hold=False, n="P+1"), (True, None, None)),
    ("sent-only, hold, 2P+1", dict(rule="sent-only", ds=1, hold=True), (None, True, None)),
    ("window, hold, 2P+1, progressive", dict(rule="window", ds=1, hold=True), (False, False, False)),
    ("window, hold, 2P+1, progressive, rows", dict(rule="window", ds=1, hold=True, rows=True), (False, False, False)),
    ("window, hold, 2P+1, interlaced, rows", dict(rule="window", ds=2, hold=True, rows=True), (False, False, False)),
    ("D-212, hold, 2P+1, progressive", dict(rule="D-212", ds=1, hold=True), (False, False, False)),
    ("D-212, hold, 2P+1, progressive, rows", dict(rule="D-212", ds=1, hold=True, rows=True), (False, False, False)),
    ("D-212, hold, 2P+1, interlaced, rows", dict(rule="D-212", ds=2, hold=True, rows=True), (False, False, False)),
    ("window, no hold, 2P+1 (the hold is still needed)", dict(rule="window", ds=1, hold=False, top=3), (True, None, None)),
    ("nodrop: a drop releases, takes no pin (live, gaps)", dict(rule="nodrop", ds=1, hold=True), (False, False, False)),
    ("mutant keepdiscard", dict(rule="keepdiscard", ds=1, hold=True), (None, True, None)),
    ("mutant shortkeeps", dict(rule="shortkeeps", ds=1, hold=True, rows=True), (None, True, None)),
    ("mutant parity (interlaced, no age rule)", dict(rule="parity", ds=2, hold=True), (None, True, None)),
    ("mutant tsq: window, hold, no idle cleanup", dict(rule="window", ds=1, hold=True, tsq=True), (None, True, None)),
    ("mutant tsq: skip, hold, no idle cleanup", dict(rule="skip", ds=1, hold=True, tsq=True), (None, True, None)),
]


def main():
    ok = True
    for label, kw, exp in CASES:
        kw = dict(kw)
        n = kw.pop("n", "2P+1")
        pools = range(kw["ds"] + 1, kw.pop("top", 4) + 1)
        res = []
        for pool in pools:
            N = pool + 1 if n == "P+1" else 2 * pool + 1
            r = run(pool, N, **kw)
            res.append((pool,) + r)
        agg = tuple(any(x[i] for x in res) for i in (1, 2, 3))
        good = all(e is None or e == a for e, a in zip(exp, agg))
        ok = ok and good
        print("%-50s %s  ring %-5s dead %-5s eos %-5s  states %s" % (
            label, "ok " if good else "BAD", agg[0], agg[1], agg[2],
            "/".join(str(x[4]) for x in res)))
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
