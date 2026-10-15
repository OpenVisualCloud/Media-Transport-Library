#!/usr/bin/env python3
"""w_noe2.py: the stores that must run E2 (D-209, core.md §4.2 step 4), by exhaustive search under
sequential consistency.  A waiter (MTL_FOREVER) follows wait-protocol.md T2/T4/T7/T8/T9: load the event
word, attempt, arm by CAS against the loaded sequence, sleep while the sequence is unchanged,
disarm.  E2 bumps the sequence and wakes a counted sleeper.  LOST = a terminal state in which the
waiter sleeps while its target is ready.

Scenarios, each with the variant that skips an E2 (LOST expected) and the rule (ok expected):
  reap      two entries j (older) and k (younger), each completed by its own context; the waiter
            reaps until both are reaped (RESULTS).  k's completer loads j's word and stores W iff
            j is not done.  skip: E2 only without W.  rule: every publish runs E2.
  acquire   the same two completions; the waiter acquires (ACQUIRE).  A slot is acquirable when
            PUBLISHED without W, or with W once j is done; a second application thread takes one
            acquirable slot, once, at any moment.  skip: E2 only without W.  rule: every publish.
  held      results off, REPEAT: slot a completes ENGINE -> FREE, slot b is HELD and its pin
            release stores HELD -> FREE; the waiter and the second thread acquire.  skip: the
            HELD -> FREE store runs no E2.  rule: every store to FREE or PUBLISHED runs E2.
Usage: python3 -I w_noe2.py; exit 1 unless every skip variant is LOST and every rule is ok."""
import sys

# The slot or entry states: E in flight, H held (pinned), P published, PW published with W,
# F free (results off), A acquired, R reaped.


def ready_reap(sl):
    return all(x in ("P", "PW") for x in sl)


def acquirable(sl, i, scen):
    if scen == "held":
        return sl[i] == "F"
    if sl[i] == "P":
        return True
    return sl[i] == "PW" and sl[0] in ("P", "PW", "A")  # j (slot 0) done: published or reused


def completers(scen):
    """per context, its steps: 'decide' loads j's word, 'store' publishes, 'e2' runs E2 (or not,
    by the variant)"""
    if scen in ("reap", "acquire"):
        return (("store", 0), ("e2", 0)), (("decide", 1), ("store", 1), ("e2", 1))
    return (("store", 0), ("e2", 0)), (("store", 1), ("e2", 1))


def runs_e2(scen, variant, slot, w):
    if variant == "rule":
        return True
    if scen == "held":
        return slot == 0  # the ENGINE -> FREE completion runs E2; the HELD -> FREE store does not
    return not (slot == 1 and w)  # only k can store W


def run(scen, variant):
    ctxs = completers(scen)
    init_sl = ("E", "H") if scen == "held" else ("E", "E")
    # state: slots, completer pcs, k's W decision, waiter pc, e, seq, cnt, asleep,
    # the second thread done, the waiter done
    init = (init_sl, (0, 0), False, 0, None, 0, 0, False, scen == "reap", False)
    seen, stack, lost = set(), [init], 0
    while stack:
        s = stack.pop()
        if s in seen:
            continue
        seen.add(s)
        sl, pcs, w, pw, e, seq, cnt, asleep, xdone, wdone = s
        nxt = []
        for c, steps in enumerate(ctxs):
            if pcs[c] >= len(steps):
                continue
            kind, slot = steps[pcs[c]]
            npcs = tuple(p + 1 if i == c else p for i, p in enumerate(pcs))
            if kind == "decide":
                nxt.append((sl, npcs, sl[0] in ("E", "H"), pw, e, seq, cnt, asleep, xdone, wdone))
            elif kind == "store":
                st = "F" if scen == "held" else ("PW" if (slot == 1 and w) else "P")
                nsl = tuple(st if i == slot else x for i, x in enumerate(sl))
                nxt.append((nsl, npcs, w, pw, e, seq, cnt, asleep, xdone, wdone))
            elif runs_e2(scen, variant, slot, w):
                wake = asleep and cnt > 0
                nxt.append((sl, npcs, w, pw, e, seq + 1, cnt, False if wake else asleep,
                            xdone, wdone))
            else:
                nxt.append((sl, npcs, w, pw, e, seq, cnt, asleep, xdone, wdone))
        if not xdone:  # the second thread: one acquire attempt, at any moment
            for i in range(2):
                if acquirable(sl, i, scen):
                    nsl = tuple("A" if j == i else x for j, x in enumerate(sl))
                    nxt.append((nsl, pcs, w, pw, e, seq, cnt, asleep, True, wdone))
            nxt.append((sl, pcs, w, pw, e, seq, cnt, asleep, True, wdone))
        if not wdone:
            if pw == 0:  # T2
                nxt.append((sl, pcs, w, 1, seq, seq, cnt, asleep, xdone, wdone))
            elif pw == 1:  # T4
                got = None
                if scen == "reap":
                    if ready_reap(sl):
                        got = ("R", "R")
                else:
                    for i in range(2):
                        if acquirable(sl, i, scen):
                            got = tuple("A" if j == i else x for j, x in enumerate(sl))
                            break
                if got:
                    nxt.append((got, pcs, w, 9, e, seq, cnt, asleep, xdone, True))
                else:
                    nxt.append((sl, pcs, w, 2, e, seq, cnt, asleep, xdone, wdone))
            elif pw == 2:  # T7
                if seq == e:
                    nxt.append((sl, pcs, w, 3, e, seq, cnt + 1, asleep, xdone, wdone))
                else:
                    nxt.append((sl, pcs, w, 0, e, seq, cnt, asleep, xdone, wdone))
            elif pw == 3:  # T8
                if seq == e:
                    nxt.append((sl, pcs, w, 4, e, seq, cnt, True, xdone, wdone))
                else:
                    nxt.append((sl, pcs, w, 5, e, seq, cnt, asleep, xdone, wdone))
            elif pw == 4 and not asleep:
                nxt.append((sl, pcs, w, 5, e, seq, cnt, asleep, xdone, wdone))
            elif pw == 5:  # T9
                nxt.append((sl, pcs, w, 0, e, seq, cnt - 1, asleep, xdone, wdone))
        if not nxt and asleep:
            if (ready_reap(sl) if scen == "reap" else
                    any(acquirable(sl, i, scen) for i in range(2))):
                lost += 1
        stack.extend(nxt)
    return len(seen), lost


def main():
    ok = True
    for scen in ("reap", "acquire", "held"):
        for variant, expect_lost in (("skip", True), ("rule", False)):
            n, lost = run(scen, variant)
            good = bool(lost) == expect_lost
            ok = ok and good
            print("%-8s %-5s %5d states  %-24s %s" % (
                scen, variant, n, "LOST (%d terminal states)" % lost if lost else "ok",
                "as expected" if good else "UNEXPECTED"))
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
