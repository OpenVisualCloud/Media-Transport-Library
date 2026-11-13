/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include "session/st20_tx_harness.h"
#include "session/st_video_transmitter_harness.h"
#include "st2110/st_err.h"

/* RL sessions on one scheduler: a packet 0 held to its target, or deferred behind its
 * pads, leaves after the first visit of the transmitter that passes its target, not a
 * scheduler round later. */
namespace {

constexpr int kPortP = 0, kPortR = 1;
constexpr int kWaitTarget = 2; /* ST_TX_VIDEO_RL_STATE_WAIT_TARGET */
constexpr uint64_t kTargetTsc = 100000000;
constexpr int kCallCap = 1000;
constexpr uint64_t kColdNs = 3000; /* a first pad burst past ST_TX_VIDEO_RL_STALL_NS */

class St20TxRlRelease : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(0, ut_trs_init());
    const float trained[1] = {164};
    ut_txv_ctx* txv = ut_txv_create();
    ASSERT_NE(nullptr, txv);
    ut_txv_set_rl_format(txv, 3840, 2160, false, ST_FPS_P59_94);
    ASSERT_EQ(0, ut_txv_init_rl_pacing(txv, 1, 0, false, trained, false));
    ASSERT_TRUE(ut_txv_rl_armed(txv, kPortP));
    p_ = ut_txv_rl_drain(txv, kPortP);
    credit_ = ut_txv_rl_credit(txv, kPortP);
    absorb_ = ut_txv_rl_absorb(txv, kPortP);
    trs_ = ut_txv_pacing_trs(txv);
    warm_pkts_ = ut_txv_pacing_warm_pkts(txv);
    ut_txv_destroy(txv);
    window_ = (uint64_t)(warm_pkts_ * trs_);
  }

  ut_trs_ctx* Session(uint64_t target_tsc, long double absorb, uint64_t first_ns) {
    ut_trs_ctx* ctx = ut_trs_create();
    ut_trs_set_trs(ctx, trs_);
    ut_trs_set_warm_pkts_cap(ctx, warm_pkts_);
    ut_trs_set_drain(ctx, kPortP, p_);
    ut_trs_set_credit(ctx, kPortP, credit_);
    ut_trs_set_absorb(ctx, kPortP, absorb);
    ut_trs_set_mock_tsc_script(ctx, &now_, 1);
    const uint64_t d[2] = {first_ns, 200}, g[2] = {300, 250};
    ut_trs_set_burst_cost(ctx, d, g, 2);
    ut_trs_enqueue_first_pkt(ctx, target_tsc);
    return ctx;
  }

  static bool After(int i, void* arg) {
    St20TxRlRelease* t = static_cast<St20TxRlRelease*>(arg);
    if (i == 1 && !t->passed_ && t->now_ > kTargetTsc) t->passed_ = t->now_;
    return true;
  }

  /* Session 0's packet 0 waits for kTargetTsc, held or deferred. Session 1's first pad
   * burst stalls stall_ns, past kTargetTsc, in the call at straddle_at (0: in the pre-arm
   * call). The other tasklets of a round take others_ns. Returns packet 0's launch past
   * its target. */
  long double Launch(bool held, uint64_t target1, uint64_t straddle_at, uint64_t stall_ns,
                     uint64_t others_ns) {
    now_ = kTargetTsc - window_ - others_ns;
    passed_ = 0;
    std::vector<ut_trs_ctx*> ctx = {Session(kTargetTsc, held ? 0 : absorb_, kColdNs),
                                    Session(target1, absorb_, 750)};
    ut_trs_set_burst_stall(ctx[1], 1, stall_ns, false);
    ut_trs_sch* sch = ut_trs_sch_create(ctx.data(), (int)ctx.size(), &now_);
    ut_trs_sch_set_visit(sch, nullptr, After, this);
    ut_trs_sch_call(sch);
    now_ = kTargetTsc - window_;
    ut_trs_sch_call(sch);
    /* held: ST_TX_VIDEO_RL_STATE_WAIT_TARGET; deferred: IDLE */
    EXPECT_EQ(held ? 2 : 0, ut_trs_rl_state(ctx[0]));
    EXPECT_EQ(1u, ut_trs_stat_recalculate_warmup(ctx[0]));
    if (straddle_at) {
      now_ = straddle_at;
      ut_trs_sch_call(sch);
    }
    for (int call = 0; call < kCallCap && !ut_trs_real_send_count(ctx[0]); call++) {
      now_ += others_ns;
      ut_trs_sch_call(sch);
    }
    EXPECT_NE(0u, passed_);
    const long double launch = ut_trs_modeled_launch_tsc(ctx[0], kPortP) - kTargetTsc;
    EXPECT_GE(launch, 0);
    end_ = (long double)passed_ - kTargetTsc;
    EXPECT_EQ(0u, ut_trs_sch_destroy(sch)) << "bursts out of their slot";
    for (ut_trs_ctx* c : ctx) ut_trs_destroy(c);
    return launch;
  }

  uint64_t now_ = 0, passed_ = 0, window_ = 0;
  long double p_ = 0, credit_ = 0, absorb_ = 0, trs_ = 0, end_ = 0;
  uint32_t warm_pkts_ = 0;
};

/* checked 3 us before its target, then session 1's visit passes it */
TEST_F(St20TxRlRelease, HeldPacket0LeavesAfterTheVisitPastItsTarget) {
  const uint64_t target1 = kTargetTsc + 50000, at = kTargetTsc - 3000;
  const long double fast = Launch(true, target1, at, 20000, 12000);
  const long double end = end_;
  const long double slow = Launch(true, target1, at, 20000, 60000);
  EXPECT_EQ(fast, slow) << "packet 0 waited for the round";
  EXPECT_LT(fast, end + 12000) << "end of the visit past the target " << end;
}

/* deferred in the pre-arm visit, whose round session 1's visit carries past the target */
TEST_F(St20TxRlRelease, DeferredPacket0LeavesAfterTheVisitPastItsTarget) {
  const uint64_t target1 = kTargetTsc + 5000;
  const long double fast = Launch(false, target1, 0, 150000, 12000);
  const long double end = end_;
  const long double slow = Launch(false, target1, 0, 150000, 60000);
  EXPECT_EQ(fast, slow) << "packet 0 waited for the round";
  EXPECT_LT(fast, end + 12000) << "end of the visit past the target " << end;
}

/* held, its target passed in session 1's visit: it leaves before session 2's visit, not
 * after the whole loop */
TEST_F(St20TxRlRelease, HeldPacket0LeavesBeforeTheNextSessionsVisit) {
  struct Seen {
    const uint64_t* now;
    ut_trs_ctx* held;
    int64_t sent = -1; /* packet 0's sends at session 2's first visit past the target */
  };
  auto before = [](int i, void* arg) {
    Seen* seen = static_cast<Seen*>(arg);
    if (i == 2 && seen->sent < 0 && *seen->now > kTargetTsc)
      seen->sent = ut_trs_real_send_count(seen->held);
    return true;
  };
  now_ = kTargetTsc - window_ - 12000;
  std::vector<ut_trs_ctx*> ctx = {Session(kTargetTsc, 0, kColdNs),
                                  Session(kTargetTsc + 50000, absorb_, 750),
                                  Session(kTargetTsc + 900000, absorb_, 750)};
  ut_trs_set_burst_stall(ctx[1], 1, 20000, false);
  Seen seen = {&now_, ctx[0]};
  ut_trs_sch* sch = ut_trs_sch_create(ctx.data(), (int)ctx.size(), &now_);
  ut_trs_sch_set_visit(sch, before, nullptr, &seen);
  ut_trs_sch_call(sch);
  now_ = kTargetTsc - window_;
  ut_trs_sch_call(sch);
  ASSERT_EQ(2, ut_trs_rl_state(ctx[0])) << "held";
  now_ = kTargetTsc - 3000;
  ut_trs_sch_call(sch);
  EXPECT_EQ(1, seen.sent);
  EXPECT_EQ(0u, ut_trs_sch_destroy(sch)) << "bursts out of their slot";
  for (ut_trs_ctx* c : ctx) ut_trs_destroy(c);
}

/* Two packets 0 held to their targets, the second a us after the first: the release
 * after the visits, which the first one's call carries past the second target, sends
 * both, not the second a scheduler round later. */
class St20TxRlReleaseScan : public ::testing::TestWithParam<bool> {
 protected:
  static constexpr uint64_t kSecondNs = 1000;

  /* the last visit ends past the first target */
  static bool After(int i, void* arg) {
    if (i == 1) *static_cast<uint64_t*>(arg) = kTargetTsc + 100;
    return true;
  }
};

/* refused: the first one's ring is full, and the second waits behind its retry */
TEST_P(St20TxRlReleaseScan, TargetPassedInTheReleaseLeavesInIt) {
  ASSERT_EQ(0, ut_trs_init());
  uint64_t now = kTargetTsc - 500000;
  std::vector<ut_trs_ctx*> ctx;
  for (uint64_t target : {kTargetTsc, kTargetTsc + kSecondNs}) {
    ut_trs_ctx* c = ut_trs_create();
    ASSERT_NE(nullptr, c);
    ut_trs_set_trs(c, 1000);
    ut_trs_set_mock_tsc_script(c, &now, 1);
    const uint64_t d[1] = {2000}, g[1] = {250};
    ut_trs_set_burst_cost(c, d, g, 1);
    ut_trs_enqueue_first_pkt(c, target);
    ut_trs_call_rl_tasklet(c);
    ASSERT_EQ(1u, ut_trs_inflight_num(c));
    ut_trs_set_rl_state_port(c, kPortP, kWaitTarget);
    ctx.push_back(c);
  }
  ut_trs_set_burst_force_fail(ctx[0], GetParam());
  now = kTargetTsc - 500;
  ut_trs_sch* sch = ut_trs_sch_create(ctx.data(), (int)ctx.size(), &now);
  ASSERT_NE(nullptr, sch);
  ut_trs_sch_set_visit(sch, nullptr, After, &now);

  ut_trs_sch_call(sch);
  EXPECT_EQ(GetParam() ? 0u : 1u, ut_trs_real_send_count(ctx[0]));
  EXPECT_EQ(GetParam() ? 0u : 1u, ut_trs_real_send_count(ctx[1]));
  if (GetParam()) {
    EXPECT_EQ(1u, ut_trs_inflight_num(ctx[1]));
    EXPECT_EQ(kWaitTarget, ut_trs_rl_state(ctx[1]));
  } else {
    EXPECT_GE(ut_trs_last_real_send_tsc(ctx[1]), kTargetTsc + kSecondNs);
  }
  EXPECT_EQ(0u, ut_trs_sch_destroy(sch)) << "bursts out of their slot";
  for (ut_trs_ctx* c : ctx) ut_trs_destroy(c);
}

INSTANTIATE_TEST_SUITE_P(Refused, St20TxRlReleaseScan, ::testing::Bool());

/* Session 1's pre-arm, all refused, waits on its retry into the next call. Session 0's
 * late entry there, before session 1's visit, holds: no call of it goes ahead of that
 * retry, as the hold's. */
TEST_F(St20TxRlRelease, LateEntryHoldsBehindARefusalOfTheCallBefore) {
  const uint64_t now2 = kTargetTsc - window_, now3 = now2 + 130000;
  now_ = now2 - 12000;
  std::vector<ut_trs_ctx*> ctx = {Session(now3 + 5000, absorb_, 750),
                                  Session(now2 + 60000, absorb_, 750)};
  ut_trs_set_burst_refuse(ctx[1], 1, 1000);
  ut_trs_sch* sch = ut_trs_sch_create(ctx.data(), (int)ctx.size(), &now_);
  ut_trs_sch_call(sch);
  now_ = now2;
  ut_trs_sch_call(sch);
  ASSERT_GT(ut_trs_pad_inflight_num(ctx[1]), 0u);
  now_ = now3;
  const uint32_t calls = ut_trs_burst_call_count(ctx[0]);
  ut_trs_sch_call(sch);
  EXPECT_EQ(calls, ut_trs_burst_call_count(ctx[0]));
  EXPECT_EQ(kWaitTarget, ut_trs_rl_state(ctx[0]));
  EXPECT_EQ(0u, ut_trs_sch_destroy(sch)) << "bursts out of their slot";
  for (ut_trs_ctx* c : ctx) ut_trs_destroy(c);
}

/* As the call before's, but the control plane holds session 1 since: a held session
 * counts as not refused, as when the late entry read its slot, so session 0 sends. */
TEST_F(St20TxRlRelease, LateEntrySendsPastAHeldSessionsRefusal) {
  const uint64_t now2 = kTargetTsc - window_, now3 = now2 + 130000;
  now_ = now2 - 12000;
  std::vector<ut_trs_ctx*> ctx = {Session(now3 + 5000, absorb_, 750),
                                  Session(now2 + 60000, absorb_, 750)};
  ut_trs_set_burst_refuse(ctx[1], 1, 1000);
  ut_trs_sch* sch = ut_trs_sch_create(ctx.data(), (int)ctx.size(), &now_);
  ut_trs_sch_call(sch);
  now_ = now2;
  ut_trs_sch_call(sch);
  ASSERT_GT(ut_trs_pad_inflight_num(ctx[1]), 0u);
  ut_trs_sch_lock_session(sch, 1, true);
  now_ = now3;
  const uint32_t calls = ut_trs_burst_call_count(ctx[0]);
  ut_trs_sch_call(sch);
  EXPECT_GT(ut_trs_burst_call_count(ctx[0]), calls);
  EXPECT_NE(kWaitTarget, ut_trs_rl_state(ctx[0]));
  ut_trs_sch_lock_session(sch, 1, false);
  EXPECT_EQ(0u, ut_trs_sch_destroy(sch)) << "bursts out of their slot";
  for (ut_trs_ctx* c : ctx) ut_trs_destroy(c);
}

/* Session 0's late entry looks for a refusal in the other sessions, then sends: from its
 * own slot, which a writer of session 0 waits on. */
TEST_F(St20TxRlRelease, LateEntrySendsFromItsOwnSlot) {
  const uint64_t now2 = kTargetTsc - window_, now3 = now2 + 130000;
  now_ = now2 - 12000;
  std::vector<ut_trs_ctx*> ctx = {Session(now3 + 5000, absorb_, 750),
                                  Session(now2 + 60000, absorb_, 750)};
  ut_trs_sch* sch = ut_trs_sch_create(ctx.data(), (int)ctx.size(), &now_);
  ut_trs_sch_call(sch);
  now_ = now2;
  ut_trs_sch_call(sch);
  now_ = now3;
  const uint32_t calls = ut_trs_burst_call_count(ctx[0]);
  ut_trs_sch_call(sch);
  ASSERT_GT(ut_trs_burst_call_count(ctx[0]), calls);
  EXPECT_EQ(0u, ut_trs_sch_destroy(sch)) << "bursts out of their slot";
  for (ut_trs_ctx* c : ctx) ut_trs_destroy(c);
}

/* port P waits on refused pads ahead of packet 0: a late entry on port R holds too */
TEST_F(St20TxRlRelease, LateEntryHoldsBehindItsOtherPortsRefusal) {
  now_ = kTargetTsc - 5000;
  ut_trs_ctx* c = Session(kTargetTsc, absorb_, 750);
  ut_trs_set_num_port(c, 2);
  ut_trs_set_rl_port(c, kPortP);
  ut_trs_set_inflight_num(c, 1);
  ut_trs_set_stat_trs_ret_code(c, -STI_RLTRS_BURST_PAD_INFLIGHT_FAIL);
  ut_trs_set_drain(c, kPortR, p_);
  ut_trs_set_credit(c, kPortR, credit_);
  ut_trs_set_absorb(c, kPortR, absorb_);
  ut_trs_set_target_tsc_port(c, kPortR, kTargetTsc);
  EXPECT_FALSE(ut_trs_pre_arm_port(c, kPortR));
  ut_trs_destroy(c);
}

/* A full NIC ring refuses every burst: each port past its target tries once in the
 * release, not again before each session of the visit. */
class St20TxRlReleaseRefused : public ::testing::Test {
 protected:
  static constexpr int kSessions = 16;

  static bool Before(int i, void* arg) {
    static_cast<St20TxRlReleaseRefused*>(arg)->visits_[i]++;
    return true;
  }

  int visits_[kSessions] = {};
};

TEST_F(St20TxRlReleaseRefused, AVisitTriesEachPortAtMostTwice) {
  ASSERT_EQ(0, ut_trs_init());
  uint64_t now = kTargetTsc + 1000;
  std::vector<ut_trs_ctx*> ctx;
  for (int i = 0; i < kSessions; i++) {
    ut_trs_ctx* c = ut_trs_create();
    ASSERT_NE(nullptr, c);
    ut_trs_set_trs(c, 1000);
    ut_trs_set_mock_tsc_script(c, &now, 1);
    ut_trs_set_target_tsc_port(c, kPortP, kTargetTsc);
    ut_trs_set_rl_state_port(c, kPortP, kWaitTarget);
    ut_trs_set_pad_inflight_num(c, 1);
    ut_trs_set_burst_force_fail(c, true);
    ctx.push_back(c);
  }
  ut_trs_sch* sch = ut_trs_sch_create(ctx.data(), kSessions, &now);
  ASSERT_NE(nullptr, sch);
  ut_trs_sch_set_visit(sch, Before, nullptr, this);

  ut_trs_sch_call(sch);
  for (int i = 0; i < kSessions; i++) EXPECT_LE(visits_[i], 2) << "session " << i;
  EXPECT_LE(ut_trs_sch_session_gets(sch), 2u * kSessions);
  EXPECT_EQ(0u, ut_trs_sch_destroy(sch)) << "bursts out of their slot";
  for (ut_trs_ctx* c : ctx) ut_trs_destroy(c);
}

}  // namespace
