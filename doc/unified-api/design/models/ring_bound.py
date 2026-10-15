#!/usr/bin/env python3
"""ring_bound.py: the smallest results-ring admission bound N (acquire admitted while
resv - reap_seq < N) under which no acquire ever finds the ring full, by exhaustive search.

State: L leased units (no seq yet), then the submitted, unreaped entries in seq order, each
  Q = queued or in flight, D = done (slot acquirable), H = done out of order (slot held until
  every older entry is done; only with hold=True).
Free slots = P - L - #Q - #H.  entries = resv - reap_seq = L + len(entries).
Actions: acquire (needs a free slot; a violation if entries >= N), release a lease, submit a lease
(appends Q), complete the oldest Q (in order), complete any other Q (out of order, ooo=True:
a drop at pick-up behind a unit still in flight, a flush, recovery), reap every ready entry.
Disciplines:
  d207   one thread: a full reap before each acquire                         (D-207's rule)
  r4     one thread: at most P acquires between two full reaps              (R4's N1 rule)
  r3     acquires at any time by any thread; a full reap before each submit (R3's rule)
  r3s2   as r3 with two submitting threads (MT_SUBMIT): <= 2 submits per full reap
Expected, P = 1...5: in order, and out of order with the hold, d207 P + 1, r4 2P, r3 2P + 1
(D-208), r3s2 2P + 2 (D-208's 2P + S); out of order without the hold, no bound up to 2P + 2 for
P >= 2 (the cap of those rows; a larger cap only costs time).
Usage: python3 -I ring_bound.py [DISC ...]; exit 1 unless every row gives its expected result.
"""
import sys
from collections import deque


def settle(ents, hold):
    # H entries whose older entries are all done become D (acquire's scan sees them in order)
    out, older_done = [], True
    for e in ents:
        if e == "H" and older_done:
            e = "D"
        out.append(e)
        older_done = older_done and e != "Q"
    return tuple(out)


def violates(P, N, disc, ooo, hold):
    # state: (L, ents, k) ; k = acquires (d207/r4) or submits (r3*) since the last full reap
    start = (0, (), 0)
    seen, todo = {start}, deque([start])
    while todo:
        L, ents, k = todo.popleft()
        nq = sum(e in "QH" for e in ents)
        free = P - L - nq
        nxt = []
        acq_ok = {"d207": k == 0, "r4": k < P}.get(disc, True)
        if free > 0 and acq_ok:
            if L + len(ents) >= N:
                return True
            nxt.append((L + 1, ents, k + 1 if disc in ("d207", "r4") else k))
        if L > 0:
            nxt.append((L - 1, ents, k))
            sub_ok = {"r3": k < 1, "r3s2": k < 2, "r3sP": k < P}.get(disc, True)
            if sub_ok:
                nxt.append((L - 1, ents + ("Q",), k + 1 if disc.startswith("r3") else k))
        qs = [i for i, e in enumerate(ents) if e == "Q"]
        for j, i in enumerate(qs):
            if j > 0 and not ooo:
                break
            older_q = any(e == "Q" for e in ents[:i])
            mark = "H" if (hold and older_q) else "D"
            e2 = settle(ents[:i] + (mark,) + ents[i + 1:], hold)
            nxt.append((L, e2, k))
        r = 0
        while r < len(ents) and ents[r] != "Q":
            r += 1
        nxt.append((L, ents[r:], 0))  # a full reap of every ready entry
        for s in nxt:
            if s not in seen:
                seen.add(s)
                todo.append(s)
    return False


def need(P, disc, ooo, hold, cap):
    for N in range(1, cap + 1):
        if not violates(P, N, disc, ooo, hold):
            return N
    return None


DISCS = ("d207", "r4", "r3", "r3s2")

BOUND = {"d207": lambda P: P + 1, "r4": lambda P: 2 * P, "r3": lambda P: 2 * P + 1,
         "r3s2": lambda P: 2 * P + 2}


def main(discs):
    ok = True
    print("%-6s %-5s %-5s " % ("rule", "ooo", "hold") + " ".join("P=%d" % P for P in range(1, 6)))
    for disc in discs:
        for ooo, hold in ((False, False), (True, False), (True, True)):
            row = []
            for P in range(1, 6):
                unbounded = ooo and not hold and P >= 2
                cap = 2 * P + 2 if unbounded else 4 * P + 4
                n = need(P, disc, ooo, hold, cap)
                ok = ok and n == (None if unbounded else BOUND[disc](P))
                row.append(">%d" % cap if n is None else str(n))
            print("%-6s %-5s %-5s " % (disc, ooo, hold) + " ".join("%4s" % x for x in row))
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(tuple(sys.argv[1:]) or DISCS))
