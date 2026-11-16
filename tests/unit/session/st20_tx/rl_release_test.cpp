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

/* RL sessions on one scheduler: a packet 0 held to its target leaves after the first
 * visit of the transmitter that passes its target, not a scheduler round later. */
namespace {

constexpr int kPortP = 0, kPortR = 1;
constexpr int kWaitTarget = 2; /* ST_TX_VIDEO_RL_STATE_WAIT_TARGET */
constexpr uint64_t kTargetTsc = 100000000;
constexpr int kCallCap = 1000;

class St20TxRlRelease : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(0, ut_trs_init());
    ASSERT_NO_FATAL_FAILURE(Plan(3840, 2160, ST_FPS_P59_94));
  }

  void Plan(uint32_t width, uint32_t height, enum st_fps fps) {
    const float trained[1] = {164};
    ut_txv_ctx* txv = ut_txv_create();
    ASSERT_NE(nullptr, txv);
    ut_txv_set_rl_format(txv, width, height, false, fps);
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

  /* Session 0's packet 0 is held to kTargetTsc. Session 1's first pad burst stalls
   * stall_ns, past kTargetTsc, in the call at straddle_at. The other tasklets of a round
   * take others_ns. Returns packet 0's launch past its target. */
  long double Launch(uint64_t target1, uint64_t straddle_at, uint64_t stall_ns,
                     uint64_t others_ns) {
    now_ = kTargetTsc - window_ - others_ns;
    passed_ = 0;
    std::vector<ut_trs_ctx*> ctx = {Session(kTargetTsc, absorb_, 750),
                                    Session(target1, absorb_, 750)};
    ut_trs_set_burst_stall(ctx[1], 1, stall_ns, false);
    ut_trs_sch* sch = ut_trs_sch_create(ctx.data(), (int)ctx.size(), &now_);
    ut_trs_sch_set_visit(sch, nullptr, After, this);
    ut_trs_sch_call(sch);
    ut_trs_set_rl_state_port(ctx[0], kPortP, kWaitTarget);
    now_ = kTargetTsc - window_;
    ut_trs_sch_call(sch);
    EXPECT_EQ(kWaitTarget, ut_trs_rl_state(ctx[0]));
    now_ = straddle_at;
    ut_trs_sch_call(sch);
    for (int call = 0; call < kCallCap && !ut_trs_real_send_count(ctx[0]); call++) {
      now_ += others_ns;
      ut_trs_sch_call(sch);
    }
    EXPECT_NE(0u, passed_);
    const long double launch = ut_trs_modeled_launch_tsc(ctx[0], kPortP) - kTargetTsc;
    EXPECT_GE(launch, 0);
    end_ = (long double)passed_ - kTargetTsc;
    ut_trs_sch_destroy(sch);
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
  const long double fast = Launch(target1, at, 20000, 12000);
  const long double end = end_;
  const long double slow = Launch(target1, at, 20000, 60000);
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
  std::vector<ut_trs_ctx*> ctx = {Session(kTargetTsc, absorb_, 750),
                                  Session(kTargetTsc + 50000, absorb_, 750),
                                  Session(kTargetTsc + 900000, absorb_, 750)};
  ut_trs_set_burst_stall(ctx[1], 1, 20000, false);
  Seen seen = {&now_, ctx[0]};
  ut_trs_sch* sch = ut_trs_sch_create(ctx.data(), (int)ctx.size(), &now_);
  ut_trs_sch_set_visit(sch, before, nullptr, &seen);
  ut_trs_sch_call(sch);
  ut_trs_set_rl_state_port(ctx[0], kPortP, kWaitTarget);
  now_ = kTargetTsc - window_;
  ut_trs_sch_call(sch);
  ASSERT_EQ(kWaitTarget, ut_trs_rl_state(ctx[0])) << "held";
  now_ = kTargetTsc - 3000;
  ut_trs_sch_call(sch);
  EXPECT_EQ(1, seen.sent);
  ut_trs_sch_destroy(sch);
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
  ut_trs_sch_destroy(sch);
  for (ut_trs_ctx* c : ctx) ut_trs_destroy(c);
}

INSTANTIATE_TEST_SUITE_P(Refused, St20TxRlReleaseScan, ::testing::Bool());

/* B's packet 0 is held to a target past the release after the visits, whose scan finds
 * B locked by the stats thread: the next call's scans still know it, and B leaves before
 * session A's visit there. */
TEST(St20TxRlReleaseLocked, HeldPacket0LeavesBeforeTheNextCallsFirstVisit) {
  ASSERT_EQ(0, ut_trs_init());
  constexpr int kA = 0, kB = 1, kC = 2;
  uint64_t now = kTargetTsc - 500000;
  std::vector<ut_trs_ctx*> ctx;
  for (uint64_t target : {kTargetTsc + 900000, kTargetTsc + 5000, kTargetTsc}) {
    ut_trs_ctx* c = ut_trs_create();
    ASSERT_NE(nullptr, c);
    ut_trs_set_trs(c, 1000);
    ut_trs_set_mock_tsc_script(c, &now, 1);
    ut_trs_enqueue_first_pkt(c, target);
    ut_trs_call_rl_tasklet(c);
    ASSERT_EQ(1u, ut_trs_inflight_num(c));
    ut_trs_set_rl_state_port(c, kPortP, kWaitTarget);
    ctx.push_back(c);
  }
  now = kTargetTsc - 500;
  ut_trs_sch* sch = ut_trs_sch_create(ctx.data(), (int)ctx.size(), &now);
  ASSERT_NE(nullptr, sch);
  struct Seen {
    ut_trs_sch* sch;
    uint64_t* now;
    ut_trs_ctx* b;
    bool locked = false;
    int64_t sent = -1; /* B's sends at A's visit of the next call */
  } seen = {sch, &now, ctx[kB]};
  /* C's visit ends past C's target, and the stats thread then takes B's lock */
  auto after = [](int i, void* arg) {
    Seen* seen = static_cast<Seen*>(arg);
    if (i == kC && !seen->locked) {
      *seen->now = kTargetTsc + 100;
      ut_trs_sch_lock(seen->sch, kB, true);
      seen->locked = true;
    }
    return true;
  };
  ut_trs_sch_set_visit(sch, nullptr, after, &seen);
  ut_trs_sch_call(sch);
  ASSERT_EQ(1u, ut_trs_real_send_count(ctx[kC]));
  ASSERT_EQ(0u, ut_trs_real_send_count(ctx[kB]));
  ut_trs_sch_lock(sch, kB, false);

  auto before = [](int i, void* arg) {
    Seen* seen = static_cast<Seen*>(arg);
    if (i == kA) seen->sent = ut_trs_real_send_count(seen->b);
    return true;
  };
  now = kTargetTsc + 6000;
  ut_trs_sch_set_visit(sch, before, nullptr, &seen);
  ut_trs_sch_call(sch);
  EXPECT_EQ(1, seen.sent);
  ut_trs_sch_destroy(sch);
  for (ut_trs_ctx* c : ctx) ut_trs_destroy(c);
}

/* B's packet 0 comes due while the stats thread holds B: each call scans for it once,
 * not before every visit, and B leaves before the first visit of the call after B is
 * unlocked, not before its target. */
TEST(St20TxRlReleaseLocked, ALockedSessionCostsOneScanACall) {
  ASSERT_EQ(0, ut_trs_init());
  constexpr int kSessions = 32, kB = 5;
  uint64_t now = kTargetTsc - 500000;
  std::vector<ut_trs_ctx*> ctx;
  for (int i = 0; i < kSessions; i++) {
    ut_trs_ctx* c = ut_trs_create();
    ASSERT_NE(nullptr, c);
    ut_trs_set_trs(c, 1000);
    ut_trs_set_mock_tsc_script(c, &now, 1);
    ut_trs_enqueue_first_pkt(c, i == kB ? kTargetTsc : kTargetTsc + 900000);
    ut_trs_call_rl_tasklet(c);
    ut_trs_set_rl_state_port(c, kPortP, kWaitTarget);
    ctx.push_back(c);
  }
  now = kTargetTsc - 500;
  ut_trs_sch* sch = ut_trs_sch_create(ctx.data(), kSessions, &now);
  ASSERT_NE(nullptr, sch);
  ut_trs_sch_call(sch);
  ut_trs_sch_lock(sch, kB, true);
  for (int call = 0; call < 3; call++) {
    now = kTargetTsc + 1000 + (uint64_t)call * 10000;
    ut_trs_sch_call(sch);
    EXPECT_LE(ut_trs_sch_dp_gets(sch), 2u * kSessions + 1) << "call " << call;
  }
  ASSERT_EQ(0u, ut_trs_real_send_count(ctx[kB]));
  ut_trs_sch_lock(sch, kB, false);
  struct Seen {
    ut_trs_ctx* b;
    int64_t sent = -1; /* B's sends at session 0's visit */
  } seen = {ctx[kB]};
  auto before = [](int i, void* arg) {
    Seen* seen = static_cast<Seen*>(arg);
    if (!i && seen->sent < 0) seen->sent = ut_trs_real_send_count(seen->b);
    return true;
  };
  ut_trs_sch_set_visit(sch, before, nullptr, &seen);
  now = kTargetTsc + 40000;
  ut_trs_sch_call(sch);
  EXPECT_EQ(1, seen.sent);
  EXPECT_GE(ut_trs_last_real_send_tsc(ctx[kB]), kTargetTsc);
  ut_trs_sch_destroy(sch);
  for (ut_trs_ctx* c : ctx) ut_trs_destroy(c);
}

/* the release after the visits sends C; an empty slot before it keeps no target for the
 * next call's scans, which then walk no session */
TEST(St20TxRlReleaseLocked, AnEmptySlotKeepsNoTarget) {
  ASSERT_EQ(0, ut_trs_init());
  uint64_t now = kTargetTsc - 500000;
  std::vector<ut_trs_ctx*> ctx;
  for (uint64_t target : {kTargetTsc + 900000, kTargetTsc + 5000, kTargetTsc}) {
    ut_trs_ctx* c = ut_trs_create();
    ASSERT_NE(nullptr, c);
    ut_trs_set_trs(c, 1000);
    ut_trs_set_mock_tsc_script(c, &now, 1);
    ut_trs_enqueue_first_pkt(c, target);
    ut_trs_call_rl_tasklet(c);
    ut_trs_set_rl_state_port(c, kPortP, kWaitTarget);
    ctx.push_back(c);
  }
  now = kTargetTsc - 500;
  ut_trs_sch* sch = ut_trs_sch_create(ctx.data(), (int)ctx.size(), &now);
  ASSERT_NE(nullptr, sch);
  ut_trs_sch_empty_slot(sch, 1);
  auto after = [](int i, void* arg) {
    if (i == 2) *static_cast<uint64_t*>(arg) = kTargetTsc + 100;
    return true;
  };
  ut_trs_sch_set_visit(sch, nullptr, after, &now);
  ut_trs_sch_call(sch);
  ASSERT_EQ(1u, ut_trs_real_send_count(ctx[2]));

  now = kTargetTsc + 6000;
  ut_trs_sch_set_visit(sch, nullptr, nullptr, nullptr);
  ut_trs_sch_call(sch);
  EXPECT_EQ(ctx.size(), ut_trs_sch_dp_gets(sch)) << "the visits' only";
  ut_trs_sch_destroy(sch);
  for (ut_trs_ctx* c : ctx) ut_trs_destroy(c);
}

/* An st22 packet 0 held to its target keeps main's hold: whatever passes its target,
 * its own visit sends it, not a release scan. */
TEST(St20TxRlReleaseSt22, HeldPacket0LeavesAtItsOwnVisit) {
  ASSERT_EQ(0, ut_trs_init());
  uint64_t now = kTargetTsc - 500000;
  std::vector<ut_trs_ctx*> ctx;
  for (uint64_t target : {kTargetTsc, kTargetTsc + 900000}) {
    ut_trs_ctx* c = ut_trs_create();
    ASSERT_NE(nullptr, c);
    ut_trs_set_trs(c, 1000);
    ut_trs_set_mock_tsc_script(c, &now, 1);
    ut_trs_enqueue_first_pkt(c, target);
    ut_trs_call_rl_tasklet(c);
    ASSERT_EQ(1u, ut_trs_inflight_num(c));
    ut_trs_set_rl_state_port(c, kPortP, kWaitTarget);
    ctx.push_back(c);
  }
  ut_trs_set_st22(ctx[0]);
  now = kTargetTsc - 500;
  ut_trs_sch* sch = ut_trs_sch_create(ctx.data(), (int)ctx.size(), &now);
  ASSERT_NE(nullptr, sch);
  auto after = [](int i, void* arg) {
    if (i == 1) *static_cast<uint64_t*>(arg) = kTargetTsc + 100;
    return true;
  };
  ut_trs_sch_set_visit(sch, nullptr, after, &now);

  ut_trs_sch_call(sch);
  EXPECT_EQ(0u, ut_trs_real_send_count(ctx[0])) << "sent in the release";
  ut_trs_sch_set_visit(sch, nullptr, nullptr, nullptr);
  ut_trs_sch_call(sch);
  EXPECT_EQ(1u, ut_trs_real_send_count(ctx[0]));
  EXPECT_GE(ut_trs_last_real_send_tsc(ctx[0]), kTargetTsc);
  ut_trs_sch_destroy(sch);
  for (ut_trs_ctx* c : ctx) ut_trs_destroy(c);
}

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
  ut_trs_sch_destroy(sch);
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
  EXPECT_LE(ut_trs_sch_dp_gets(sch), 2u * kSessions);
  ut_trs_sch_destroy(sch);
  for (ut_trs_ctx* c : ctx) ut_trs_destroy(c);
}

/* Session 0's NIC ring stays full behind its refused packet 0, and the last session's
 * packet 0 is due behind it: of the call's scans, only the first and the one after
 * session 0's retry walk the sessions. */
TEST_F(St20TxRlReleaseRefused, APortBlockedBehindARefusedOneIsRescannedAfterItsRetry) {
  ASSERT_EQ(0, ut_trs_init());
  uint64_t now = kTargetTsc - 500;
  std::vector<ut_trs_ctx*> ctx;
  for (int i = 0; i < kSessions; i++) {
    ut_trs_ctx* c = ut_trs_create();
    ASSERT_NE(nullptr, c);
    ut_trs_set_trs(c, 1000);
    ut_trs_set_mock_tsc_script(c, &now, 1);
    ut_trs_set_hang_detect_thresh_ns(c, UINT64_MAX);
    ctx.push_back(c);
  }
  ut_trs_set_burst_force_fail(ctx[0], true);
  ut_trs_enqueue_first_pkt(ctx[0], now);
  ut_trs_call_rl_tasklet(ctx[0]);
  ASSERT_EQ(1u, ut_trs_inflight_num(ctx[0]));
  ASSERT_EQ(-STI_RLTRS_BURST_INFLIGHT_FAIL, ut_trs_get_stat_trs_ret_code(ctx[0]));
  ut_trs_ctx* b = ctx[kSessions - 1];
  ut_trs_enqueue_first_pkt(b, kTargetTsc);
  ut_trs_call_rl_tasklet(b);
  ut_trs_set_rl_state_port(b, kPortP, kWaitTarget);
  ut_trs_sch* sch = ut_trs_sch_create(ctx.data(), kSessions, &now);
  ASSERT_NE(nullptr, sch);
  ut_trs_sch_call(sch);
  ASSERT_EQ(0u, ut_trs_real_send_count(b));

  now = kTargetTsc + 1000;
  ut_trs_sch_call(sch);
  EXPECT_EQ(1u, ut_trs_real_send_count(b));
  EXPECT_EQ(1u, ut_trs_inflight_num(ctx[0]));
  EXPECT_LE(ut_trs_sch_dp_gets(sch), 3u * kSessions);
  ut_trs_sch_destroy(sch);
  for (ut_trs_ctx* c : ctx) ut_trs_destroy(c);
}

/* A refused st22 port, whose NIC ring stays full, keeps main's RL path: the st20
 * packet 0 due after it leaves in the first scan, ahead of session 1's visit. */
TEST_F(St20TxRlReleaseRefused, ARefusedSt22PortBlocksNoSt20Port) {
  ASSERT_EQ(0, ut_trs_init());
  uint64_t now = kTargetTsc - 500;
  std::vector<ut_trs_ctx*> ctx;
  for (int i = 0; i < 3; i++) {
    ut_trs_ctx* c = ut_trs_create();
    ASSERT_NE(nullptr, c);
    ut_trs_set_trs(c, 1000);
    ut_trs_set_mock_tsc_script(c, &now, 1);
    ut_trs_set_hang_detect_thresh_ns(c, UINT64_MAX);
    ctx.push_back(c);
  }
  ut_trs_set_st22(ctx[0]);
  ut_trs_set_burst_force_fail(ctx[0], true);
  ut_trs_enqueue_first_pkt(ctx[0], now);
  ut_trs_call_rl_tasklet(ctx[0]);
  ASSERT_EQ(-STI_RLTRS_BURST_INFLIGHT_FAIL, ut_trs_get_stat_trs_ret_code(ctx[0]));
  ut_trs_ctx* b = ctx[2];
  ut_trs_enqueue_first_pkt(b, kTargetTsc);
  ut_trs_call_rl_tasklet(b);
  ut_trs_set_rl_state_port(b, kPortP, kWaitTarget);
  ut_trs_sch* sch = ut_trs_sch_create(ctx.data(), (int)ctx.size(), &now);
  ASSERT_NE(nullptr, sch);
  ut_trs_sch_call(sch);
  ASSERT_EQ(0u, ut_trs_real_send_count(b));

  struct Seen {
    ut_trs_ctx* b;
    int64_t sent = -1; /* B's sends at session 1's visit */
  } seen = {b};
  auto before = [](int i, void* arg) {
    Seen* seen = static_cast<Seen*>(arg);
    if (i == 1 && seen->sent < 0) seen->sent = ut_trs_real_send_count(seen->b);
    return true;
  };
  ut_trs_sch_set_visit(sch, before, nullptr, &seen);
  now = kTargetTsc + 1000;
  ut_trs_sch_call(sch);
  EXPECT_EQ(1, seen.sent);
  EXPECT_EQ(0u, ut_trs_sch_unguarded(sch)) << "the scan walked st22 too";
  ut_trs_sch_destroy(sch);
  for (ut_trs_ctx* c : ctx) ut_trs_destroy(c);
}

/* Session 0 holds packet 0 to kTargetTsc, the others past it, and session 1's visit ends
 * past kTargetTsc: the release scan before session 2's visit sends session 0, or with two
 * sessions the one after the visits. A writer, that is a free, update_destination or
 * stats call, takes the session's lock, then waits only while the transmitter is in its
 * slot once the pass outlasts its spin: each scan must publish the slot it sends in. */
class St20TxRlReleaseSlot : public ::testing::Test {
 public:
  void SetUp() override {
    ASSERT_EQ(0, ut_trs_init());
  }
  void TearDown() override {
    ut_trs_sch_destroy(sch_);
    for (ut_trs_ctx* c : ctx_) ut_trs_destroy(c);
  }

  void Create(int n) {
    now_ = kTargetTsc - 500000;
    for (int i = 0; i < n; i++) {
      ut_trs_ctx* c = ut_trs_create();
      ASSERT_NE(nullptr, c);
      ut_trs_set_trs(c, 1000);
      ut_trs_set_mock_tsc_script(c, &now_, 1);
      ut_trs_enqueue_first_pkt(c, i ? kTargetTsc + 900000 : kTargetTsc);
      ut_trs_call_rl_tasklet(c);
      ut_trs_set_rl_state_port(c, kPortP, kWaitTarget);
      ctx_.push_back(c);
    }
    now_ = kTargetTsc - 500;
    sch_ = ut_trs_sch_create(ctx_.data(), n, &now_);
    ASSERT_NE(nullptr, sch_);
    ut_trs_sch_set_visit(sch_, nullptr, After, this);
    ut_trs_sch_set_burst(sch_, Burst, this);
  }

  static bool After(int i, void* arg) {
    St20TxRlReleaseSlot* t = static_cast<St20TxRlReleaseSlot*>(arg);
    if (i != 1 || t->passed_) return true;
    t->now_ = kTargetTsc + 100;
    t->passed_ = true;
    if (t->at_pass_) t->at_pass_(t);
    return true;
  }

  /* session 0's first burst past the target, in a release scan */
  static bool Burst(int i, void* arg) {
    St20TxRlReleaseSlot* t = static_cast<St20TxRlReleaseSlot*>(arg);
    if (i || !t->passed_ || t->burst_seen_) return true;
    t->burst_seen_ = true;
    if (t->in_burst_) t->in_burst_(t);
    return true;
  }

  uint64_t now_ = 0;
  bool passed_ = false, burst_seen_ = false;
  std::vector<ut_trs_ctx*> ctx_;
  ut_trs_sch* sch_ = nullptr;
  void (*at_pass_)(St20TxRlReleaseSlot*) = nullptr;
  void (*in_burst_)(St20TxRlReleaseSlot*) = nullptr;
  bool writer_ = false, writer_in_ = false;
  int own_ = -1, peer_ = -1;
};

class St20TxRlReleaseSlotScan : public St20TxRlReleaseSlot,
                                public ::testing::WithParamInterface<int> {};

TEST_P(St20TxRlReleaseSlotScan, SendsEachSessionInItsSlot) {
  Create(GetParam());
  ut_trs_sch_call(sch_);
  ASSERT_EQ(1u, ut_trs_real_send_count(ctx_[0]));
  EXPECT_EQ(0u, ut_trs_sch_unguarded(sch_));
  EXPECT_EQ(-1, ut_trs_sch_slot(sch_)) << "a writer waits for the slot past the call";
}

INSTANTIATE_TEST_SUITE_P(Sessions, St20TxRlReleaseSlotScan, ::testing::Values(2, 3));

/* with membarrier(2), or a full fence after each slot store without it */
class St20TxRlReleaseSlotWriter : public St20TxRlReleaseSlot,
                                  public ::testing::WithParamInterface<bool> {};

TEST_P(St20TxRlReleaseSlotWriter, WaitsWhileTheScanSendsItsSession) {
  Create(3);
  if (GetParam()) ut_trs_sch_force_fence(sch_);
  in_burst_ = [](St20TxRlReleaseSlot* t) {
    t->writer_ = !ut_trs_sch_writer_start(t->sch_, 0);
    t->writer_in_ = t->writer_ && ut_trs_sch_writer_in(t->sch_, 20000);
  };
  ut_trs_sch_call(sch_);
  ASSERT_TRUE(writer_);
  EXPECT_FALSE(writer_in_) << "the writer went on while the scan sent its session";
  EXPECT_TRUE(ut_trs_sch_writer_in(sch_, 1000000));
  ut_trs_sch_writer_put(sch_);
  EXPECT_EQ(1u, ut_trs_real_send_count(ctx_[0]));
}

INSTANTIATE_TEST_SUITE_P(Fence, St20TxRlReleaseSlotWriter, ::testing::Bool());

/* session 0 freed after its visit counted its target: the scans skip its slot */
TEST_F(St20TxRlReleaseSlot, AFreedSessionIsNotTouchedByTheLaterScans) {
  Create(3);
  at_pass_ = [](St20TxRlReleaseSlot* t) {
    ut_trs_sch_lock(t->sch_, 0, true);
    ut_trs_sch_empty_slot(t->sch_, 0);
    ut_trs_sch_lock(t->sch_, 0, false);
  };
  ut_trs_sch_call(sch_);
  EXPECT_EQ(0u, ut_trs_real_send_count(ctx_[0]));
  EXPECT_EQ(0u, ut_trs_sch_unguarded(sch_));
}

/* Its notify_frame_done() runs in the burst, through the frame's extbuf free, and asks
 * for its own stats and another session's of the scheduler: as in a visit, both return
 * at once and leave no lock held. */
TEST_F(St20TxRlReleaseSlot, ACallbackInAScanGetsItsOwnAndAPeersStats) {
  Create(3);
  in_burst_ = [](St20TxRlReleaseSlot* t) {
    t->own_ = ut_trs_sch_get_stats(t->sch_, 0);
    t->peer_ = ut_trs_sch_get_stats(t->sch_, 2);
  };
  ut_trs_sch_call(sch_);
  ASSERT_TRUE(burst_seen_);
  EXPECT_EQ(0, own_);
  EXPECT_EQ(0, peer_);
  for (int i = 0; i < 3; i++) EXPECT_FALSE(ut_trs_sch_locked(sch_, i)) << i;
}

/* Session 0's late entry checks session 1's refused pads, nested in its own visit: it
 * holds behind them, unless a writer holds session 1 or session 1 is st22, and takes no
 * lock and leaves the visit's slot published. */
enum class Peer { kRefused, kHeld, kSt22 };

class St20TxRlReleaseNested : public St20TxRlRelease,
                              public ::testing::WithParamInterface<Peer> {};

TEST_P(St20TxRlReleaseNested, ALateEntryChecksTheOtherSessionsInItsOwnSlot) {
  const Peer peer = GetParam();
  const uint64_t now2 = kTargetTsc - window_, now3 = now2 + 130000;
  now_ = now2 - 12000;
  std::vector<ut_trs_ctx*> ctx = {Session(now3 + 5000, absorb_, 750),
                                  Session(now2 + 60000, absorb_, 750)};
  ut_trs_set_burst_refuse(ctx[1], 1, 1000);
  if (peer == Peer::kSt22) ut_trs_set_st22(ctx[1]);
  ut_trs_sch* sch = ut_trs_sch_create(ctx.data(), (int)ctx.size(), &now_);
  ut_trs_sch_call(sch);
  now_ = now2;
  ut_trs_sch_call(sch);
  ASSERT_GT(ut_trs_pad_inflight_num(ctx[1]), 0u);
  now_ = now3;
  const uint32_t calls = ut_trs_burst_call_count(ctx[0]);
  if (peer == Peer::kHeld) ut_trs_sch_lock(sch, 1, true);
  ut_trs_sch_call(sch);
  EXPECT_EQ(peer != Peer::kRefused, ut_trs_burst_call_count(ctx[0]) > calls);
  EXPECT_EQ(0u, ut_trs_sch_unguarded(sch));
  EXPECT_EQ(0u, ut_trs_sch_trylocks(sch));
  EXPECT_EQ(peer == Peer::kHeld, ut_trs_sch_locked(sch, 1));
  EXPECT_FALSE(ut_trs_sch_locked(sch, 0));
  if (peer == Peer::kHeld) ut_trs_sch_lock(sch, 1, false);
  ut_trs_sch_destroy(sch);
  for (ut_trs_ctx* c : ctx) ut_trs_destroy(c);
}

INSTANTIATE_TEST_SUITE_P(Peers, St20TxRlReleaseNested,
                         ::testing::Values(Peer::kRefused, Peer::kHeld, Peer::kSt22));

/* A session that leaves its slot, freed or migrated, takes its transmitter state along:
 * none of it lingers for the slot's next session or the new scheduler's calls. */
class St20TxRlReleaseLeave : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(0, ut_txv_init());
    ctx_ = ut_txv_create();
    ASSERT_NE(nullptr, ctx_);
  }
  void TearDown() override {
    ut_txv_destroy(ctx_);
  }
  ut_txv_ctx* ctx_ = nullptr;
};

/* the old scheduler's transmitter released it in calls the new one's count may reach */
TEST_F(St20TxRlReleaseLeave, AMigratedSessionWasReleasedInNoCallOfTheNewScheduler) {
  ut_txv_set_trs_released(ctx_, 0, 1);
  ut_txv_run_migrate(ctx_);
  EXPECT_EQ(0u, ut_txv_trs_released(ctx_, 0));
}

/* a refused port of a freed or migrated session no longer holds another's late entry */
TEST_F(St20TxRlReleaseLeave, AFreedSessionLeavesNoRefusedPort) {
  ut_txv_set_slot_refused(ctx_, 1);
  ut_txv_run_detach(ctx_);
  EXPECT_EQ(0, ut_txv_slot_refused(ctx_));
}

/* a migrated one takes its refused ports to its new slot: the other sessions there hold
 * their late entries behind its retry from the first call on */
TEST_F(St20TxRlReleaseLeave, AMigratedSessionTakesItsRefusedPortsAlong) {
  ut_txv_set_slot_refused(ctx_, 1);
  ut_txv_run_migrate(ctx_);
  EXPECT_EQ(0, ut_txv_slot_refused(ctx_));
  EXPECT_EQ(1, ut_txv_migrated_slot_refused(ctx_));
}

}  // namespace
