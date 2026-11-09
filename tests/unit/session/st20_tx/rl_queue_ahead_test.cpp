/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Right after packet 0 the NIC TX queue of an RL session holds only what the
 * transmitter pushed since; the shaper drains a pkt every trs. If the scheduler is away
 * longer than the queue holds, the wire pauses mid-frame and the receiver's VRX
 * underflows. These cases run the transmitter tasklet a round at a time from an empty
 * queue, as just after packet 0, against a NIC TX queue of MT_DEV_TX_DESC_DEEP
 * descriptors and a full builder ring.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='*St20TxRlQueueAhead*'
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <string>

#include "session/st20_tx_harness.h"
#include "session/st_video_transmitter_harness.h"

namespace {

constexpr uint16_t kChainedSegs = 2;
/* the round of the fault matrix: the other tasklets of a scheduler */
constexpr uint64_t kRoundNs = 12000;
/* mt_sch_schedule_ns(), as mtl_init() sets it */
constexpr uint32_t kSchScheduleNs = 200000;
constexpr long double kRoundingNs = 1e-6L;
constexpr int kWaitWarmUp = 1; /* ST_TX_VIDEO_RL_STATE_WAIT_WARMUP */
constexpr uint32_t kBulk = 4;  /* ST_SESSION_MAX_BULK, the stream's bulk */

struct Fmt {
  const char* name;
  uint32_t width, height;
};

class St20TxRlQueueAhead : public ::testing::TestWithParam<Fmt> {
 protected:
  void SetUp() override {
    ASSERT_EQ(0, ut_trs_init());
    const Fmt& f = GetParam();
    const float trained[1] = {164};
    ut_txv_ctx* txv = ut_txv_create();
    ASSERT_NE(nullptr, txv);
    ut_txv_set_rl_format(txv, f.width, f.height, false, ST_FPS_P59_94);
    ASSERT_EQ(0, ut_txv_init_rl_pacing(txv, 1, 0, false, trained, false));
    trs_ = ut_txv_pacing_trs(txv);
    warm_pkts_ = ut_txv_pacing_warm_pkts(txv);
    ut_txv_destroy(txv);
    for (ut_trs_ctx*& c : ctx_) {
      c = ut_trs_create();
      ASSERT_NE(nullptr, c);
      ut_trs_set_sch_schedule_ns(c, kSchScheduleNs);
    }
    ASSERT_EQ(0,
              ut_trs_setup_stream(ctx_[0], trs_, ut_trs_deep_nb_tx_desc(), kChainedSegs));
    ut_trs_set_warm_pkts_cap(ctx_[0], warm_pkts_);
  }
  void TearDown() override {
    for (ut_trs_ctx* c : ctx_) ut_trs_destroy(c);
  }

  /* the second session holds its next packet 0, due in target_in_ns */
  void HoldPacket0(uint64_t target_in_ns) {
    ut_trs_set_trs(ctx_[1], trs_);
    ut_trs_set_warm_pkts_cap(ctx_[1], warm_pkts_);
    ut_trs_set_target_tsc_port(ctx_[1], 0, ut_trs_last_tsc(ctx_[0]) + target_in_ns);
    ut_trs_set_rl_state_port(ctx_[1], 0, kWaitWarmUp);
    ut_trs_stream_fill(ctx_[0]);
  }

  long double WindowNs() const {
    return warm_pkts_ * trs_;
  }

  ut_trs_ctx* ctx_[2] = {};
  long double trs_ = 0;
  uint32_t warm_pkts_ = 0;
};

/* the scheduler may be away mt_sch_schedule_ns() once it yields; from the round that
 * sent packet 0 on, the queue covers that */
TEST_P(St20TxRlQueueAhead, AbsenceOfAScheduleAfterPacket0LeavesNoWireGap) {
  ut_trs_stream_rounds(ctx_[0], kRoundNs, kRoundNs);
  ut_trs_stall_stream(ctx_[0], kSchScheduleNs - kRoundNs);
  ut_trs_stream_rounds(ctx_[0], 2 * (uint64_t)kSchScheduleNs, kRoundNs);

  EXPECT_LT(ut_trs_stream_max_late_ns(ctx_[0]), kRoundingNs)
      << GetParam().name << " trs " << (double)trs_;
}

/* an absence past what the queue holds leaves the wire waiting the rest */
TEST_P(St20TxRlQueueAhead, AbsencePastTheQueueLeavesAWireGap) {
  ut_trs_stream_rounds(ctx_[0], kRoundNs, kRoundNs);
  const long double ahead = ut_trs_stream_ahead_ns(ctx_[0]);
  constexpr uint64_t kPastNs = 100000;
  ut_trs_stall_stream(ctx_[0], (uint64_t)ahead + kPastNs);
  ut_trs_stream_rounds(ctx_[0], kRoundNs, kRoundNs);

  EXPECT_NEAR(ut_trs_stream_max_late_ns(ctx_[0]), (long double)kPastNs, trs_);
}

TEST_P(St20TxRlQueueAhead, CoveredAbsenceLeavesNoWireGap) {
  ut_trs_stream_rounds(ctx_[0], kRoundNs, kRoundNs);
  ut_trs_stall_stream(ctx_[0], (uint64_t)ut_trs_stream_ahead_ns(ctx_[0]) - kRoundNs);
  ut_trs_stream_rounds(ctx_[0], 2 * (uint64_t)kSchScheduleNs, kRoundNs);

  EXPECT_LT(ut_trs_stream_max_late_ns(ctx_[0]), kRoundingNs);
}

/* a packet 0 whose window opens within a window from now keeps the round as short as
 * without the catch-up */
TEST_P(St20TxRlQueueAhead, NearPacket0OfAnotherSessionDefersTheCatchUp) {
  HoldPacket0((uint64_t)(1.5L * WindowNs()));
  ut_trs_call_transmitter(ctx_, 2);

  EXPECT_EQ(2u, ut_trs_burst_call_count(ctx_[0]));
}

/* a near packet 0 on a port the visits skip, without a NIC TX queue, defers nothing */
TEST_P(St20TxRlQueueAhead, NearPacket0OfAPortWithoutAQueueLetsItCatchUp) {
  HoldPacket0((uint64_t)(1.5L * WindowNs()));
  ut_trs_set_queue_port(ctx_[1], 0, false);
  ut_trs_call_transmitter(ctx_, 2);

  EXPECT_GE(ut_trs_stream_ahead_ns(ctx_[0]), kSchScheduleNs);
}

TEST_P(St20TxRlQueueAhead, FarPacket0OfAnotherSessionLetsItCatchUp) {
  HoldPacket0((uint64_t)(3 * WindowNs()));
  ut_trs_call_transmitter(ctx_, 2);

  EXPECT_GE(ut_trs_stream_ahead_ns(ctx_[0]), kSchScheduleNs);
}

/* an st22 session keeps main's RL path: a short queue gets only its visit's bursts */
TEST_P(St20TxRlQueueAhead, St22IsNotCaughtUp) {
  ut_trs_set_st22(ctx_[0]);
  ut_trs_stream_fill(ctx_[0]);
  ut_trs_call_transmitter(ctx_, 1);

  EXPECT_EQ(2u, ut_trs_burst_call_count(ctx_[0]));
}

/* An st22 session keeps main's RL path: the packet 0 it holds defers no catch-up, even
 * once its target passes during the later visits of the round. */
TEST_P(St20TxRlQueueAhead, St22Packet0DueInTheRoundLetsItCatchUp) {
  uint64_t now = ut_trs_last_tsc(ctx_[0]);
  ut_trs_set_st22(ctx_[1]);
  ut_trs_set_trs(ctx_[1], trs_);
  ut_trs_set_target_tsc_port(ctx_[1], 0, now + kRoundNs / 2);
  ut_trs_set_rl_state_port(ctx_[1], 0, kWaitWarmUp);
  ut_trs_stream_fill(ctx_[0]);
  ut_trs_sch* sch = ut_trs_sch_create(ctx_, 2, &now);
  ASSERT_NE(nullptr, sch);
  ut_trs_sch_set_schedule_ns(sch, kSchScheduleNs);
  /* the round's other visits take kRoundNs */
  ut_trs_sch_set_visit(
      sch, nullptr,
      [](int i, void* arg) {
        if (i == 1) *(uint64_t*)arg += kRoundNs;
        return true;
      },
      &now);
  ut_trs_sch_call(sch);
  ut_trs_sch_destroy(sch);

  ASSERT_EQ(kWaitWarmUp, ut_trs_rl_state_port(ctx_[1], 0)) << "the st22 packet 0 left";
  EXPECT_GE(ut_trs_stream_ahead_ns(ctx_[0]), kSchScheduleNs);
}

/* the catch-up skips a port the visit skips: one without a NIC TX queue */
TEST_P(St20TxRlQueueAhead, CatchUpSkipsAPortWithoutAQueue) {
  ut_trs_set_trs(ctx_[1], trs_);
  ut_trs_set_stat_trs_ret_code(ctx_[1], 1);
  ut_trs_set_queue_port(ctx_[1], 0, false);
  ut_trs_stream_fill(ctx_[0]);
  ut_trs_call_transmitter(ctx_, 2);

  ASSERT_GE(ut_trs_stream_ahead_ns(ctx_[0]), kSchScheduleNs) << "no catch-up ran";
  EXPECT_EQ(1, ut_trs_get_stat_trs_ret_code(ctx_[1]));
}

/* With the NIC queue full, a visit tries no more bursts than without the catch-up. The
 * round is the longest the builder keeps up with, a bulk a visit. */
TEST_P(St20TxRlQueueAhead, FullQueueAddsNoBurstCalls) {
  const uint64_t round = std::min(kRoundNs, (uint64_t)(kBulk * trs_));
  ut_trs_stream_rounds(ctx_[0], (uint64_t)(ut_trs_deep_nb_tx_desc() * trs_), round);
  ASSERT_GT(ut_trs_stream_nic_desc_used(ctx_[0]),
            ut_trs_deep_nb_tx_desc() - kChainedSegs - 1u);
  const uint32_t calls = ut_trs_burst_call_count(ctx_[0]);
  constexpr uint32_t kRounds = 100;
  ut_trs_stream_rounds(ctx_[0], kRounds * round, round);

  EXPECT_LE(ut_trs_burst_call_count(ctx_[0]) - calls, 2 * kRounds);
}

INSTANTIATE_TEST_SUITE_P(Formats, St20TxRlQueueAhead,
                         ::testing::Values(Fmt{"720p", 1280, 720},
                                           Fmt{"1080p", 1920, 1080},
                                           Fmt{"2160p", 3840, 2160}),
                         [](const ::testing::TestParamInfo<Fmt>& info) {
                           return std::string(info.param.name) + "59";
                         });

}  // namespace
