# The wait protocol: pause-hook tests and models

This file carries the evidence of G-52, G-95 and G-142 (D-141, D-142) that the plan cites:
the deterministic pause-hook tests WH1–WH21, WH13b and WH14b, and the models in
[models/](models/). It is a design record: C1w, C1h, C2 and MS2 copy from it, and
[implementation-plan.md](../implementation-plan.md) §8.2 ("The wait evidence") says which job runs
what. The protocol and its step labels (T1–T12, D1–D10, K1–K6, R1–R2, H1–H3, W0–W6, P1, S1, Q1–Q8,
Q5a, F1–F8, G1–G5, N0', N1–N12, X1–X4, Y1) are those of [engine.md](../engine.md) §7.1–§7.3; a
step label is local to its procedure and is not a test.

Random stress caught 1 of 4 mutants, so G-52, G-95 and G-142 rest on two pieces of deterministic
evidence: **the models, run in CI**, and **pause-hook tests that replay the models'
counterexamples**. Random stress (`stress.c`) is a nightly smoke test only.

## 1. The models

They need only the Python 3 standard library (`stress.c`: a C compiler and Linux). Each explores
every interleaving under sequential consistency; every atomic operation and every syscall is one
step. Weak-memory orderings are argued in engine.md §7.1, not modelled.

| File | What it checks | Run |
|---|---|---|
| [wait_model.py](models/wait_model.py) | the 16 cases of the first model: one-shot consumers beside a sleeping loop (N9), single sweeps (I3), shared handles (L10), WT waiters beside loops (I1, I1', I2), interrupts (I2b, F1), and the review's protocol (h) for comparison; LOST and SPIN | `python3 wait_model.py [CASE...]`; `--v1` checks the first version (no always-drain); exit 1 on a design violation |
| [wait_model2.py](models/wait_model2.py) | the extended model, 62 cases: split WAKE_NOW, application wakers, session and instance interrupts, close, spurious returns and timeouts, `mtl_wait(o, m, 0)` loops (Q), EPOLLONESHOT, killed threads, the M4 variant (V1, V2), the v2.1 cases N1–N6 and the edge-triggered (ET) cases with their no-edge-rule mutants (ETm); LOST, SPIN, LEAK | `python3 wait_model2.py [CASE...] [key=0\|1 ...]`; exit 1 if a case gives other than its expected result |
| [wait_reuse.py](models/wait_reuse.py) | retire and reuse (Y1) racing a WAKE_NOW of the old session; LOST and a write to the old descriptor (UAF); v1, always-drain with v1's guard, and v2 | `python3 wait_reuse.py`; v2 must print 0 violations for mask {1} and {0,1} |
| [wait_flush.py](models/wait_flush.py) | the count-bounded flush (F1–F8) of one library loop over three objects, WT waiters arming while marks are carried, the loop's sleep check; the variant that ignores the flush's return | `python3 wait_flush.py`; the design cases must print LOST=0, the "return ignored" variant LOST > 0 |
| [wait_mutants.py](models/wait_mutants.py) | five mutants of the handle path of `wait_model.py`: no drain without a taken bit, no re-signal after the read, no arm on a miss, no re-check after the arm, no re-post | `python3 wait_mutants.py`; exit 1 if a mutant is not killed |
| [wait_cycles.py](models/wait_cycles.py) | the exact busy-loop check: the full state graph of a `wait_model2.py` case with the loops' idle counters zeroed, every strongly connected component, and whether a weakly fair scheduler can stay in it | `python3 wait_cycles.py CASE...`; a busy loop prints `fair_livelock` > 0 |
| [stress.c](models/stress.c) | random-yield stress on the real kernel (futex, eventfd, epoll LT or EPOLLONESHOT) with the mutants `-DNO_REPOST`, `-DNO_E2`, `-DNO_HSIG`, `-DNO_RESIGNAL` | `cc -O2 -pthread stress.c -o stress; ./stress ROUNDS [1]`; a smoke test, not evidence |
| [provide_model.py](models/provide_model.py) | the `mtl_rx_provide` hand-back of D-152 (contract.md §9.11): every interleaving of providers, hand-backs and a taking unit; the ledger rule | `python3 provide_model.py`; MS2b copies it into `tests/unit/core/` |

C1w copies the six `wait_*.py` files into `tests/unit/core/wait_model/` with a runner that fails
on any result other than the expected one; a changed protocol step changes the model in the same
commit, and the reviewer compares engine.md §7.1's step numbers with the model's states.

## 2. Mechanism

- `ST_CORE_WAIT_HOOK(point)` is compiled only with `-Denable_unit_tests=true`; the release build
  defines it empty.
- A test registers, per hook point, a pair of semaphores. The hooked thread posts "arrived" and
  waits for "go". The test thread drives the other parties step by step.
- The pause hooks wait on a raw futex word (`syscall(SYS_futex, &w, FUTEX_WAIT_PRIVATE, …)`), never
  on `sem_wait`, which is itself a cancellation point.
- No test sleeps to provoke a race, and every wait has a 1 s ceiling. A test asserts on event
  counts (wakes, `read()`s, epoll returns, results), never on short wall-clock times.

## 3. The hook points

| Point | Where |
|---|---|
| `WT_BEFORE_SLEEP` | after T9 |
| `WT_AFTER_RECHECK` | between T8 and T9 |
| `DP_AFTER_ATTEMPT` | after D2 |
| `DP_AFTER_CONSUME` | after K2 |
| `DP_BEFORE_READ` | before R1 |
| `DP_AFTER_READ` | between R1 and R2 |
| `DP_AFTER_ARM` | after H3 |
| `WAKE_AFTER_TAKE` | after W2 |
| `WAKE_BEFORE_WRITE` | after step P1, before step S1 |
| `FLUSH_AFTER_XCHG` | after F6 |
| `INTR_AFTER_SET` | after N7 |
| `RETIRE_AFTER_STATE` | after X1 |

## 4. Where they run

- Binary `UnifiedUnitTest`, suite `WaitHook`, a parameterised fixture with two bindings:
  - **`TestBinding`:** a counter-based core binding, from C1w;
  - **`NullBinding`:** `null:1` with the test clock, from C2. The same schedules then run on the
    production null path, including the test clock's FLUSH under ADVANCE.
- `WaitFlush` runs in `UnitTest` through `tests/unit/sch/mt_sch_harness.c`, because it needs the
  real `sch_tasklet_func`.

**Gate 2.** Each test must fail with the step it pins reverted locally (the plan's existing Gate 2
practice). The "Kills" column names that step. No mutant code is added to the library.

## 5. The tests

The labels in parentheses in the Schedule column name what a test replays (§7).

| Test | Schedule (replays) | Asserts | Kills | Task |
|---|---|---|---|---|
| WH1 `WaitHook.wt_ignores_handle_drain` | W in `acquire(∞)` parked at `WT_BEFORE_SLEEP`; complete and flush; B runs `reap(0)` with a RESULTS handle and drains; resume W (I1) | W returns the slot; 1 wake | a WT call that consumes the handle | C1w |
| WH2 `WaitHook.wseq_before_recheck` | W parked at `WT_AFTER_RECHECK`; complete and flush (a bump); resume (F3) | W returns, no sleep | loading `v` after T8 | C1w |
| WH3 `WaitHook.lanes_do_not_steal` | A and D in `acquire(∞)`, C in `reap(∞)`, all asleep; publish a result only (I2) | C returns; A and D woken 0 times (a futex-return counter) | a broadcast or a lane-free bitset | C1w |
| WH4 `WaitHook.sweep_keeps_result` | mask ACQUIRE\|RESULTS; reap(0) misses; a completion frees a slot and makes a result, flushed; acquire(0) succeeds, then misses (I3) | `poll(fd, 0)` = POLLIN; the next sweep returns the result and leaves the fd unreadable | consuming the handle on success | C1w |
| WH5 `WaitHook.stale_write_drained` | the flush parked at `WAKE_BEFORE_WRITE`; the sweep takes the lane, drains (nothing to read) and arms; resume the flush (N1; `wait_mutants.py`'s `mut_drain_only_if_took` on `wait_model.py` case 2) | the loop wakes once; the next sweep leaves the fd unreadable; epoll returns ≤ 2 | draining only after taking a bit | C1w |
| WH6 `WaitHook.resignal_after_read` | E parked at `DP_BEFORE_READ` with nothing pending; P publishes, posts and writes; resume E: its read consumes that write (mutant `mut_no_resignal`) | after E's call the fd is readable and the unit is still ready | no re-signal after the read | C1w |
| WH7 `WaitHook.arm_then_recheck` | E parked at `DP_AFTER_CONSUME`, before ARM_H; complete and flush; resume (I4; mutants `mut_no_arm`, `mut_no_recheck`) | the call returns the unit, or the fd is readable | no arm; no re-check | C1w |
| WH8 `WaitHook.repost_after_take` | E asleep; X's `acquire(0)` parked at `DP_AFTER_READ` after taking the bit; P publishes two units (H clear, no wake); resume X (N3; mutant `repost_off` on case 0) | X takes one; the fd is readable; E wakes and takes the other | no re-post | C1w |
| WH9 `WaitHook.slot_variant_reposts` | X in `acquire_slot(busy slot)` takes lane 0's bit while another slot is free beside a sleeping `acquire` loop (M4; `wait_model2.py` V1) | the fd is readable | a re-post by the call's predicate | MS2 (`acquire_slot`) |
| WH10 `WaitHook.interrupt_vs_sleep` | W parked at `WT_BEFORE_SLEEP`; an instance interrupt from another thread, and from a signal handler sent to W itself; resume (F1). Variant: ACQUIRE only, with a reaper asleep | `-MTL_ECANCELED`; the reaper woken 0 times | the walk without a bump; W's `intr` load before its fence | C1w |
| WH11 `WaitHook.close_vs_sleep` | W parked at `WT_BEFORE_SLEEP`; close, or stop FLUSH under acquire; resume | `-MTL_ESHUTDOWN` | a state change without a wake | C1w (close); C2 (`FORCE_ERROR` → `-MTL_EIO`) |
| WH12 `WaitHook.retire_vs_wake` | a WAKE_NOW of session S parked at `WAKE_AFTER_TAKE`; another thread closes and retires S, then creates S' on the reused entry with mask {RESULTS} (M1; `wait_reuse.py`) | retire does not complete while the wake is parked; after resume, S' sweeps, a result on S' makes its fd readable | WAKE_NOW outside the in-flight counter; Y1 order | C1w |
| WH13 `WaitHook.mtl_wait_cancel_level` | Q sweeps with `mtl_wait(o, A\|R, 0)` and sleeps; interrupt ACQUIRE ON (M2) | Q's `mtl_wait` returns `-MTL_ECANCELED` and the fd stays readable; after OFF, a sweep returns `-MTL_EAGAIN` and the fd is unreadable; with a DP loop E on the same handle, E's units are not lost (V3) | Q7 while cancelled; consuming on `-MTL_ECANCELED` | C1w |
| WH13b `WaitHook.wait0_intr_after_arm` | WAIT0 parked at `DP_AFTER_CONSUME`; interrupt; resume (`wait_model2.py` N1, N1m) | `-MTL_ECANCELED`; the fd is readable | a removed Q5a | C1w (C1h if split) |
| WH14 `WaitHook.no_cancellation_point` | a thread with a deferred cancel pending, parked at `DP_BEFORE_READ` and then `DP_AFTER_READ` (M3) | the call completes and returns; the thread dies only at the test's own `pthread_testcancel()`; no lane is stranded | libc `read`/`write` | C1w |
| WH14b (a `WaitHook` case; C1w names it) | a thread cancelled while blocked in `acquire(∞)`; then `mtl_interrupt` (M3) | it stays blocked until the interrupt, returns `-MTL_ECANCELED`, dies at its next cancellation point; the in-flight count reads 0 | a cancellation point in a WT call; a leaked in-flight count | C1w |
| WH15 `WaitHook.fork_child_interrupt` | `fork()`; the child calls `mtl_instance_interrupt` and `mtl_session_interrupt` (eng M4) | `-MTL_EBADF`, no crash, `errno` kept | a missing N0' | C1w |
| WH16 `WaitFlush.count_bound` (UnitTest) | 512 marked objects with a counting syscall shim; objects 0–63 re-marked every iteration (`wait_flush.py`) | ≤ 1 object with a syscall per iteration; every object woken within 512 iterations; the loop never sleeps while marked | a lost return value; no cursor | C1w |
| WH17 `WaitHook.test_clock_flush` | a WT waiter on `null:1`; the test thread calls `MTL_FAULT_CLOCK_ADVANCE` (eng M5) | the waiter returns after ADVANCE returns; two runs give the same wake order (G-92) | ADVANCE calling WAKE_NOW directly instead of FLUSH | C2 |
| WH18 `WaitHook.mask_fixed` | `mtl_get_wait_handle` twice with the same mask, then with another (eng M3) | the same fd; then `-MTL_EBUSY` | the union | C1w |
| WH19 `WaitHook.accounting` | 10^5 timeouts, interrupts and closes; then 10^6 units with no waiter and no handle | every lane count 0; 0 futex and 0 eventfd syscalls (counters) | a leaked count; a wake with nobody armed | C2 |
| WH20 `WaitHook.timeouts` | a 1 ms wait; `MTL_FAULT_TIME_STEP` during a wait | the return is ≥ 1 ms and < the 1 s ceiling; the step changes nothing | a deadline on TAI | C2 |
| WH21 `WaitHook.as_paths` (non-ASan job) | the interrupt from a handler delivered to a WT waiter, to a thread inside chunk growth, to a thread inside a DP call, and to a thread in RETIRE's wait | completes; `errno` unchanged; a `malloc` hook that aborts is never hit | TLS or allocation on the AS path | C1w |

**Public forms** (A2a):

- ex03 on `null:1` with two event-loop threads on one handle;
- `mtl_instance_interrupt` from a real SIGTERM handler while workers block in acquire (ex11's
  shape).

**Nightly smoke** (MS2a): [models/stress.c](models/stress.c) (LT and ONESHOT, 2 000 rounds each).

## 6. Placement

Each test lands with the task that makes it runnable:

| Task | Tests |
|---|---|
| C1w | WH1–WH8, WH10, WH11 (close and stop), WH12–WH16, WH13b, WH14b, WH18, WH21, on `TestBinding` |
| C2 | WH11 (ERROR), WH17, WH19, WH20, and the `NullBinding` runs |
| A2a | the public forms |
| MS2 | WH9 |
| C1h (only if C1w splits, MS2a) | WH1's handle half, WH4–WH8, WH12, WH13, WH13b, WH18 |

## 7. What the schedules replay

- **Model cases.** I1, I1', I2, I2b, I3, F1, N9, L10 and `(h) …` are case names in
  `wait_model.py`; R*, X*, V*, N1–N6 and ET* are case names in `wait_model2.py`. Each is a
  configuration whose interleavings the model explores.
- **The first review's interleaving classes** (the cases above are named after them):
  - I1: a WT acquire in `poll()` while a DP reap drains the shared descriptor;
  - I2: two WT waiters, one draining the other's wake;
  - I3: a single thread whose drain in the middle of the sweep strands a result;
  - I4: a DP miss that arms with no re-check;
  - F1: the instance interrupt waking without changing the compared word;
  - F3: the order of the `wseq` load (after the fence, before the T8 re-check);
  - N1: a drain clearing the last pending bit before the flush's `write()` lands, so a call that
    drains only when it clears a bit never drains again (a busy loop with one thread);
  - N3: a call that takes a pending bit and returns a unit without re-arming or re-posting (a lost
    wake-up for another thread asleep on the handle).
- **The formal review's majors:**
  - M1: a WAKE_NOW outside the in-flight counter straddles retire and reuse and posts a stale lane
    into the new session (closed by W0, W6 and the Y1 order);
  - M2: `mtl_wait(o, m, 0)` under an interrupt never ends the sweep (closed by Q2 and Q5a: the
    interrupt is level for `mtl_wait`);
  - M3: libc `read`/`write` are cancellation points (closed by raw `syscall()`);
  - M4: `acquire_slot` takes lane 0's bit with a narrower predicate and does not re-post (closed by
    D8 and Q7 re-posting by the lane predicate).
- **The engineering review's items:**
  - eng M3: the union of handle masks (the mask is fixed by the first `mtl_get_wait_handle`);
  - eng M4: the AS path in a `fork()`ed child (N0' by `getpid`);
  - eng M5: the test clock skipping the production flush (ADVANCE runs FLUSH).
- **Mutants.** `mut_drain_only_if_took`, `mut_no_resignal`, `mut_no_arm`, `mut_no_recheck` and
  `repost_off` are the mutants of `wait_mutants.py`; it prints the cases that kill each one.
