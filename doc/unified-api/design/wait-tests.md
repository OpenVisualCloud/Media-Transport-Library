# The wait protocol: pause-hook tests and models

This file carries the evidence of G-52, G-95 and G-142 (D-142, D-158–D-169) that the plan cites:
the deterministic pause-hook tests (WH, WC and QW) and the models in [models/](models/). It is a
design record: C1w, C2, C1q1, C1q2 and MS2 copy from it, and
[implementation-plan.md](../implementation-plan.md) §8.2 ("The wait evidence") says which job runs
what. The protocol and its step labels (E1–E4, M1, K1–K2, F1–F8, T1–T9, D1–D2, Q1–Q3, RW1–RW2,
N0', N1–N9, V1–V2, X1–X4, Y1, P1–P3, PS1–PS4, U1–U2, A1–A11, J1–J10, C1–C5, Y2, DV1–DV3) are those of
[core.md](../core.md) §6.1–§6.3 and §6.6; a step label is local to its procedure and is not a
test.

Random stress caught 1 of 4 mutants of an earlier protocol, so G-52, G-95 and G-142 rest on two
pieces of deterministic evidence: **the model, run in CI**, and **pause-hook tests that replay its
counterexamples**. Random stress (`stress.c`) is a nightly smoke test only.

## 1. The models

They need only the Python 3 standard library (`stress.c`: a C compiler and Linux). Each explores
every interleaving under sequential consistency; every atomic operation and every syscall is one
step. Weak-memory orderings are argued in core.md §6.1, not modelled.

| File | What it checks | Run |
|---|---|---|
| [final_model.py](models/final_model.py) | the object path of core.md §6 (cases o1–o7), the queue path (q1–q11, r1–r4) and posts (p1–p5); 23 mutants named by the step they break; LOST, LEAK, OVER, SPIN, XQ, RPT, PAC (below) | `python3 final_model.py [CASE ...]`; `--trace CASE MUTANT` prints a counterexample; exit 1 unless every case gives its result and every mutant dies (6 s) |
| [wait_flush.py](models/wait_flush.py) | the count-bounded flush (F1–F8) of one library loop over three objects, sleepers arming while marks are carried, the loop's sleep check; the variant that ignores the flush's return; from M0 a queue entry among the marked entries and `wake_k` 1 and 4 | `python3 wait_flush.py`; the design cases must print LOST=0, the "return ignored" variant LOST > 0 |
| [stress.c](models/stress.c) | random-yield stress on the real kernel (futex, eventfd, epoll LT or EPOLLONESHOT). Until M0 it still drives the superseded per-object handle (D-159); M0 rewrites it onto the event word and a queue consumer, with the mutants `-DBITS`, `-DLOAD_AFTER_ATTEMPT`, `-DNO_EXIT_RESIGNAL` | `cc -O2 -pthread stress.c -o stress; ./stress ROUNDS [1]`; a smoke test, not evidence |
| [provide_model.py](models/provide_model.py) | the `mtl_rx_provide` hand-back of D-152 (contract.md §9.11): every interleaving of providers, hand-backs and a taking unit; the ledger rule | `python3 provide_model.py`; MS2b copies it into `tests/unit/core/` |

`final_model.py`'s object path covers the event word with counts, calls with a timeout,
interrupts and their clearing (`MTL_INTR_OFF`, then a new call: o7), close and row waits; its
queue path the push and claim, the reset and arm, the write-back rule, queue interrupts, their
clearing (q11) and close, detach, a stale claimer and queue entry reuse, and the posts of
`mtl_queue_post` (a poster thread `QP` that publishes an item, then posts). Its checks:
LOST (a sleeper whose condition holds), LEAK (a count that does not match its sleepers), OVER (an
attachment in two places), SPIN (a cycle in the state graph), XQ (a push onto a reused queue), RPT
(a report popped after its detach returned), PAC (a post accepted by a closed or reused queue).
LOST also fires when a sleeper's queue holds a pending post or an item no report served.

Not modelled, and argued in core.md §6.1 instead: weak memory, the instance walk, state-change
reports (an EVENT with every lane), J5's return 1 (lock serialisation), the delivery count of the
detach (DV1–DV3), descriptors kept for the process's life, the flush bound (`wait_flush.py`), and
the post's CAS made even when its bits are already pending (a weak-memory rule, core.md §6.1), the retire and reuse of an object's entry (X1–X4, Y1) under a stale K1 or interrupt (argued in
core.md §6.1 "Lifetime" and by the generation in `intr`; tests WH12 and WC3). `final_model.py`'s
`NOT_MODELLED` list names the labels it does not model (N1, N0', N2, N3, T1, T5, C3–C5, X3, Y1,
Y2, J1–J6, DV1–DV3) with the reason for each.

The two paths are explored apart: no case has a sleeper in a call with a timeout on an object
that is also armed on a queue. They meet only on `ec`, whose every write is an RMW (T7, T9, E2,
E4, J8), so the single-location lemma of core.md §6.1 covers both. A T7 or J8 CAS that fails
because of the other path reloads `ec` and retries; neither path clears the other's bits. Task M0
adds a joint case to `final_model.py`: a W on lane 0 and an E with the object armed for ACQUIRE,
one P unit; both return (one report, one wake).

epoll registrations other than level-triggered are not modelled either: an `EPOLLONESHOT` or
`EPOLLEXCLUSIVE` registration shared by several threads and an `EPOLLET` one (contract.md §7.2
"Any epoll mode works") are argued from A7 and A10. Every sleep follows a reset, and every signal
a call took is written back on every exit but its own `-MTL_EAGAIN`, so each later write is a new
edge, and a one-shot registration is re-armed by the thread that consumed it. Task M0 adds
ONESHOT and ET variants of q5 and q9 to `final_model.py`.

The model has no consumer that leaves a ready target untaken (a sender without a source frame).
The earlier handle protocol's model drained every ready target and so missed that ex03 spun. Here
"arm ACQUIRE only with a frame" is a user rule (contract.md §7.2), pinned by QW10, not by the
model.

Task M0 adds `check_labels.py` (the label sets of core.md §6.1 and §6.6, of the model and of
`st_core_wait.c` once it exists must agree, minus the `NOT_MODELLED` list) and `run_models.py`, the
model job's one entry point. C1w copies the model files into `tests/unit/core/wait_model/` with
`run_models.py`; a changed protocol step changes the model in the same commit, and
`check_labels.py` compares the labels. The reviewer does not.

## 2. Mechanism

- `ST_CORE_WAIT_HOOK(point)` is compiled only with `-Denable_unit_tests=true`; the release build
  defines it empty.
- A test registers, per hook point, a pair of semaphores. The hooked thread posts "arrived" and
  waits for "go". The test thread drives the other parties step by step.
- The pause hooks wait on a raw futex word (`syscall(SYS_futex, &w, FUTEX_WAIT_PRIVATE, …)`), never
  on `sem_wait`, which is itself a cancellation point.
- No test sleeps to provoke a race, and every wait has a 1 s ceiling. A test asserts on event
  counts (wakes, `read()`s, `write()`s, epoll returns, reports, results), never on short wall-clock
  times.

## 3. The hook points

| Point | Where |
|---|---|
| `WT_BEFORE_ARM` | between T4 and T7 |
| `WT_BEFORE_SLEEP` | between T7 and T8 |
| `FLUSH_AFTER_XCHG` | after F6 |
| `WAKE_BEFORE_SYSCALL` | in K1 and K2, before the syscall |
| `INTR_AFTER_STATE` | after N3 |
| `RETIRE_AFTER_STATE` | after X1 |
| `PUSH_BEFORE_CAS` | in P2, before the CAS (MS2a) |
| `POST_BEFORE_CAS` | in PS2, before the CAS (MS2a) |
| `QW_AFTER_RESET` | after A7's `read()` (MS2a) |
| `QW_AFTER_ARM` | after A8 (MS2a) |
| `QW_AFTER_POP` | after A5, before A10 (MS2a) |
| `QA_AFTER_LOAD` | after J7 (MS2a) |
| `Q_AFTER_ENTER` | after A1 and after J1 (MS2a) |

## 4. Where they run

- Binary `UnifiedUnitTest`, suite `WaitHook` (object path) and `QueueHook` (queue path), a
  parameterised fixture with two bindings:
  - **`TestBinding`:** a counter-based core binding, from C1w;
  - **`NullBinding`:** `null:1` with the test clock, from C2. The same schedules then run on the
    production null path, including the test clock's FLUSH under ADVANCE.
- `WaitFlush` runs in `UnitTest` through `tests/unit/sch/mt_sch_harness.c`, because it needs the
  real `sch_tasklet_func`.

**Gate 2.** Each test must fail with the step it pins reverted locally (the plan's existing Gate 2
practice). The "Kills" column names that step or the model's mutant. No mutant code is added to the
library.

## 5. The tests

### 5.1 The object path (MS1, MS2)

| Test | Schedule | Asserts | Kills | Task |
|---|---|---|---|---|
| WH2 `WaitHook.load_before_attempt` | W parked at `WT_BEFORE_ARM`; complete and flush; resume | W returns without sleeping | T2 after T4 (`load_after_attempt`); arming without the compare | C1w |
| WH3 `WaitHook.lanes_do_not_steal` | A and D in `acquire(∞)`, C in `reap(∞)`, all asleep; publish a result only | C returns; A and D woken 0 times (a futex-return counter) | a broadcast or a lane-free bitset | C1w |
| WH10 `WaitHook.interrupt_vs_sleep` | W parked at `WT_BEFORE_SLEEP`; an instance interrupt from another thread, and from a signal handler sent to W itself; resume. Variant: ACQUIRE only, with a reaper asleep | `-MTL_ECANCELED`; the reaper woken 0 times | N6 without E2 (`intr_no_bump`); a wake through M1 | C1w |
| WH11 `WaitHook.close_vs_sleep` | W parked at `WT_BEFORE_SLEEP`; close, or stop FLUSH under acquire; resume | `-MTL_ESHUTDOWN` | a state change without a wake | C1w (close); C2 (`FORCE_ERROR` → `-MTL_EIO`) |
| WH12 `WaitHook.stale_wake_reused` | a K1 parked at `WAKE_BEFORE_SYSCALL` after F6 on S; S retires and S' reuses the entry; resume | S' sleepers return at most once spuriously; retire never waits for the wake | a wake that reads freed or reused state; Y1 order | C1w |
| WH14b (a `WaitHook` case) | a thread cancelled while blocked in `acquire(∞)`; then `mtl_interrupt` | it stays blocked until the interrupt, returns `-MTL_ECANCELED`, dies at its next cancellation point; the in-flight count reads 0 | a cancellation point in a WT call; a leaked in-flight count | C1w |
| WH15 `WaitHook.fork_child_interrupt` | `fork()`; the child calls `mtl_instance_interrupt`, `mtl_session_interrupt` and, from MS2a, `mtl_queue_wait` and `mtl_queue_post` | `-MTL_EBADF`, no crash, `errno` kept | a missing N0' | C1w, C1q2 |
| WH16 `WaitFlush.count_bound` (UnitTest) | 512 marked entries with a counting syscall shim; entries 0–63 re-marked every iteration; run with `wake_k` 1 and 4; from MS2a queues among them | ≤ `wake_k` entries with a syscall per iteration; every entry woken within 512 iterations; the loop never sleeps while marked | a lost return value; no cursor | C1w |
| WH17 `WaitHook.test_clock_flush` | a WT sleeper on `null:1`; the test thread calls `MTL_FAULT_CLOCK_ADVANCE` | the sleeper returns after ADVANCE returns; two runs give the same wake order (G-92) | ADVANCE calling WAKE_NOW directly instead of FLUSH | C2 |
| WH19 `WaitHook.accounting` | 10^5 timeouts, interrupts and closes; 10^5 timed-out calls, then 10^5 completions; 10^6 units with no sleeper and no armed queue | every lane count 0; 0 futex and 0 eventfd syscalls (counters) | a leaked count (`no_dec`); a wake with nobody counted | C2 |
| WH20 `WaitHook.timeouts` | a 1 ms wait; `MTL_FAULT_TIME_STEP` during a wait | the return is ≥ 1 ms and < the 1 s ceiling; the step changes nothing | a deadline on TAI | C2 |
| WH21 `WaitHook.as_paths` (non-ASan job) | the interrupt from a handler delivered to a WT sleeper, to a thread inside chunk growth, inside a DP call or in RETIRE's wait; from MS2a a queue interrupt, and a post to a thread parked at `QW_AFTER_POP` in the queue lock | completes; `errno` unchanged; a `malloc` hook that aborts is never hit | TLS or allocation on the AS path | C1w, C1q2 |
| WC1 `WaitHook.no_clear_by_producer` | the model's o1 trace with W2 parked at `WT_BEFORE_SLEEP` | W2 returns both units | a producer-cleared bit (`bits`) | C1w |
| WC2 `WaitHook.waiter_removes_its_count` | 10^4 sleepers time out; publish | 0 futex wakes, counts 0 | `no_dec` | C1w |
| WC3 `WaitHook.interrupt_generation` | an interrupt parked at `INTR_AFTER_STATE` across retire and reuse | `-MTL_EBADF`; S' untouched | N5 without the generation | C1w |
| WC4 `WaitHook.rows_want` | `rows_step` 34 over 2160 rows; readers for 540 and 1080 | 0 wakes before 540, one each after | `want_store`; RW1 firing per step | MS2 |
| WC5 `WaitHook.sleeper_beyond_count` | one sleeper more than a lane's count holds, on one target; one unit | the last returns within 2 ms; no thread spins | T6 returning at once | C1w |
| WC6 `WaitHook.slot_beside_acquire` | a sleeper in `mtl_tx_acquire_slot(s, i, ∞)` with slot i busy and a sleeper in `mtl_tx_acquire(∞)`, both on lane 0; release another slot | `acquire` returns that slot; `acquire_slot` returns only when slot i is free; no thread spins | a K1 that wakes one sleeper of the lane | MS2 (`acquire_slot`) |

### 5.2 The queue path (MS2a)

| Test | Schedule | Asserts | Kills | Task |
|---|---|---|---|---|
| WH14 `QueueHook.no_cancellation_point` | a thread with a deferred cancel pending, parked at `QW_AFTER_RESET`, then in A10's write-back and in A11's `ppoll` | the call completes and returns; the thread dies only at the test's own `pthread_testcancel()`; no signal is stranded | libc `read`, `write`, `ppoll` | C1q2 |
| QW1 `QueueHook.arm_validates` | J8 parked at `QA_AFTER_LOAD`; publish; resume | reported | `arm_no_validate` | C1q1 |
| QW2 `QueueHook.claim_once` | a completion and an application release race on one armed object | one report | `claim_by_load` | C1q1 |
| QW3 `QueueHook.reset_before_arm` | parked at `QW_AFTER_RESET`; push and write; resume | a report, not `-MTL_EAGAIN` | `reset_after_arm` | C1q1 |
| QW4 `QueueHook.resignal_after_steal` | E in epoll; a one-off poller parked at `QW_AFTER_RESET`; a push | the poller reports; the descriptor is readable | `no_resignal` | C1q1 |
| QW5 `QueueHook.stale_write_once` | K2 parked at `WAKE_BEFORE_SYSCALL`; the consumer finds the work itself | one empty return, then unreadable | `no_reset` | C1q1 |
| QW6 `QueueHook.interrupt_pair` | parked at `QW_AFTER_ARM`, and before A8 | `-MTL_ECANCELED` both ways | `intr_no_claim`, `no_intr_recheck` | C1q2 |
| QW7 `QueueHook.close_waits_calls` | a sleeper inside `mtl_queue_wait(∞)`; close, then create on the entry | the sleeper returns `-MTL_ESHUTDOWN`; the new queue starts unreadable | `close_no_wait` | C1q1 |
| QW8 `QueueHook.closing_never_reported` | close a session whose node is listed | no report; the slot DEAD, then FREE at the pop; a reused entry gets another slot | X3, A5's state check | C1q2 |
| QW9 `QueueHook.instance_interrupt_queues` | `mtl_instance_interrupt` from a handler, sleepers on 3 queues | every one `-MTL_ECANCELED`; no `malloc` | V2's queue branch | C1q2 |
| QW10 `QueueHook.one_shot_mask` | RESULTS armed, slots free, no frame | 0 reports, 0 wakes over 10^4 completions | a level report | C1q1 |
| QW11 `QueueHook.burst_one_write` | 128 armed sessions complete in one iteration | one `write()`, 128 reports | a write per object | C1q1 |
| QW12 `QueueHook.exit_writes_back` | E in epoll; a poller parked at `QW_AFTER_RESET` while an interrupt (and, as a variant, a close) writes | E wakes and returns the code; close does not hang | `no_exit_resignal` | C1q1 |
| QW13 `QueueHook.detach_vs_pop` | `mtl_queue_arm(q, o, 0, 0)` against a pop of o, both orders | no report with o's user popped after the detach returned | `no_dead` | C1q2 |
| QW14 `QueueHook.stale_claimer` | a claimer parked at `PUSH_BEFORE_CAS` across close and create | the new queue never sees the node; the slot FREE | `no_qgen` | C1q1 |
| QW15 `QueueHook.state_edge` | DRAIN stop, update, start of an armed session; ERROR, stop, start | one STATE report per change; the session stays armed and reports its next unit | a code that detaches | C1q2 |
| QW16 `QueueHook.rearm_in_flight` | J5 against an E4 claim | returns 1; the report carries the new user | J5's silent 0 | C1q2 |
| QW17 `QueueHook.detach_waits_delivery` | a consumer parked at `QW_AFTER_POP` with a report of o; another thread detaches o (variant: closes o); then 4 consumers loop on the queue while a fifth detaches and re-arms 10^4 times | the detach and the close return only after the parked consumer returned; every detach returns within 1 ms (no starvation) | no DV1–DV3; one phase only | C1q2 |
| QW18 `QueueHook.stale_caller_ebadf` | a `mtl_queue_wait` and a `mtl_queue_arm` parked at `Q_AFTER_ENTER` across the queue's close and a create on the entry | both return `-MTL_EBADF`; the new queue's list and descriptor are untouched | A3 and J2 testing CLOSED only | C1q1 |
| QW19 `QueueHook.descriptors_kept` | an AS `mtl_instance_interrupt` parked at `WAKE_BEFORE_SYSCALL` in U2's write while the last `mtl_instance_close` runs; then the test opens a socket | the write lands on the kept eventfd (the socket reads nothing); `/proc/self/fd` still lists the queues' eventfds, at most the peak of live queues | closing queue descriptors at the last instance close | C1q2 |
| QW20 `QueueHook.post_wakes` | a loop asleep in `mtl_queue_wait(∞)`, another in epoll on the descriptor; posts from a thread and from a signal handler; then 10^4 posts while the loop is awake | each sleeper wakes; one report whose `user` is the OR; 0 `write()`s while awake | `post_no_claim` | C1q1 |
| QW21 `QueueHook.post_vs_arm` | a consumer parked at `QW_AFTER_RESET`; a post; resume | a report of posts, not `-MTL_EAGAIN` | `arm_ignores_post` | C1q1 |
| QW22 `QueueHook.post_vs_close` | a post parked at `POST_BEFORE_CAS` across close and a create on the entry; and parked at `WAKE_BEFORE_SYSCALL` in PS3 | `-MTL_EBADF`, the new queue reports nothing; at most one empty return | `post_no_check` | C1q1 |

**Public forms** (C1q2): ex03 on `null:1` with two event-loop threads on one queue, a one-off
poller and a test source that posts from two threads and a signal handler; `mtl_instance_interrupt` from a real SIGTERM handler while workers block in acquire (ex16's
shape, A2a).

**Nightly smoke** (MS2a): [models/stress.c](models/stress.c) (LT and ONESHOT, 2 000 rounds each).

## 6. Placement

Each test lands with the task that makes it runnable:

| Task | Tests |
|---|---|
| C1w | WH2, WH3, WH10, WH11 (close and stop), WH12, WH14b, WH15, WH16, WH21, WC1–WC3, WC5, on `TestBinding` |
| C2 | WH11 (ERROR), WH17, WH19, WH20, and the `NullBinding` runs |
| A2a | ex16's SIGTERM form |
| MS2 | WC4 |
| C1q1 (MS2a) | QW1–QW5, QW7, QW10–QW12, QW14, QW18, QW20–QW22 |
| C1q2 (MS2a) | WH14, QW6, QW8, QW9, QW13, QW15–QW17, QW19; WH15's and WH21's queue parts; ex03's public form |

Retired with the per-object descriptor that queues replaced ([D-159](../decisions.md)): WH1,
WH4–WH9, WH13, WH13b and WH18.
Totals: 16 tests in MS1, 2 in MS2, 23 in MS2a.

## 7. What the schedules replay

- **Model cases.** o1–o7, q1–q11, r1–r4 and p1–p5 are case names in `final_model.py`; each is a
  configuration whose interleavings the model explores, and `--trace CASE MUTANT` prints the
  counterexample a test replays.
- **Mutants.** `bits`, `no_arm`, `load_after_attempt`, `producer_skip`, `intr_no_bump`, `no_dec`,
  `want_store`, `no_resignal`, `no_exit_resignal`, `reset_after_arm`, `no_reset`, `arm_store`,
  `arm_no_validate`, `claim_by_load`, `intr_no_claim`, `no_intr_recheck`, `no_qgen`, `no_dead`,
  `close_no_wait`, `post_no_claim`, `arm_ignores_post`, `post_no_check` and `post_before_publish`; the model prints the cases that kill each one.
- **Argued and tested, not modelled**: the delivery count (QW17), the generation check of A3 and J2
  (QW18; the model checks it), the descriptors kept for the process's life (QW19).
