#!/usr/bin/env python3
"""trace_d207.py: the shortest run in which D-207's own rule (a full reap before each acquire), in
ex21's shape (one lease at a time), meets a full results ring (admission pool_count + 1) once a
unit completes out of order and frees its slot at once (no hold). P = 2, N = 3.
Usage: python3 -I trace_d207.py; prints the run; exit 1 if there is none."""
import sys
from collections import deque

P, N = 2, 3
start = (0, (), 0)
par = {start: None}
todo = deque([start])
end = None
while todo and end is None:
    L, ents, k = s = todo.popleft()
    free = P - L - sum(e == "Q" for e in ents)
    moves = []
    if free > 0 and k == 0 and L == 0:  # ex21: reap, acquire, submit; one lease at a time
        if L + len(ents) >= N:
            end = (s, "acquire blocks on RESULTS: entries %d = N" % (L + len(ents)))
            break
        moves.append(("acquire", (L + 1, ents, 1)))
    if L > 0:
        moves.append(("submit", (L - 1, ents + ("Q",), k)))
    qs = [i for i, e in enumerate(ents) if e == "Q"]
    for j, i in enumerate(qs):
        what = "complete seq+%d in order" % i if j == 0 else "drop seq+%d at pick-up (older in flight)" % i
        moves.append((what, (L, ents[:i] + ("D",) + ents[i + 1:], k)))
    r = 0
    while r < len(ents) and ents[r] != "Q":
        r += 1
    moves.append(("full reap (%d ready)" % r, (L, ents[r:], 0)))
    for m, s2 in moves:
        if s2 not in par:
            par[s2] = (s, m)
            todo.append(s2)
if end is None:
    print("no run meets a full ring")
    sys.exit(1)
s, last = end
path = []
while par[s]:
    s, m = par[s]
    path.append(m)
for i, m in enumerate(reversed(path)):
    print("%2d %s" % (i + 1, m))
print("   " + last)
