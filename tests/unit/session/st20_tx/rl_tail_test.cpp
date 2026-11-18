/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>

#include "mtl_sch_api.h"
#include "session/st20_tx/rl_vrx_model.h"
#include "session/st20_tx_harness.h"
#include "session/st_video_transmitter_harness.h"
#include "st2110/st_tx_video_rl.h"

/* An absorbed stall whose plan needs a burst more than the hold's sends its last pads
 * with packet 0 in one call: pads first, then packet 0's pkts. */
namespace {

constexpr int kPortP = 0;
constexpr uint64_t kTargetTsc = 100000000;
constexpr long double kBurstBytes = 2048;
constexpr uint64_t kOthersNs = 12000;
constexpr int kRoundCap = 100000;
constexpr uint16_t kPktLen = 1262; /* packet 0's bytes, those of a NORMAL pkt */
constexpr uint16_t kRsThresh = kUtRlRsThresh;
constexpr uint64_t kTscReadNs = 1; /* the mock clock's step per read past its script */

class St20TxRlTail : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(0, ut_trs_init());
    ASSERT_NO_FATAL_FAILURE(Plan(3840, 2160));
  }
  void Plan(uint32_t width, uint32_t height) {
    const float trained[1] = {164};
    ut_txv_ctx* txv = ut_txv_create();
    ASSERT_NE(nullptr, txv);
    ut_txv_set_rl_format(txv, width, height, false, ST_FPS_P59_94);
    ASSERT_EQ(0, ut_txv_init_rl_pacing(txv, 1, 0, false, trained, false));
    bps_ = ut_txv_rl_queue_bps(txv, kPortP);
    p_ = ut_txv_rl_drain(txv, kPortP);
    credit_ = ut_txv_rl_credit(txv, kPortP);
    absorb_ = ut_txv_rl_absorb(txv, kPortP);
    trs_ = ut_txv_pacing_trs(txv);
    warm_pkts_ = ut_txv_pacing_warm_pkts(txv);
    ut_txv_destroy(txv);
  }
  void TearDown() override {
    if (ctx_) ut_trs_destroy(ctx_);
  }

  /* one frame, the first pad burst costing first_ns before its doorbell */
  void Frame(uint64_t first_ns, uint16_t mixed_accept, bool rtcp,
             uint64_t tail_stall_ns = 0, uint32_t pad_batches = 0,
             uint64_t deferred_round_ns = kOthersNs) {
    if (ctx_) ut_trs_destroy(ctx_);
    uint64_t now = kTargetTsc - (uint64_t)(warm_pkts_ * trs_) + entry_ns_;
    ctx_ = ut_trs_create();
    ASSERT_NE(nullptr, ctx_);
    ut_trs_set_trs(ctx_, trs_);
    ut_trs_set_warm_pkts_cap(ctx_, warm_pkts_);
    ut_trs_set_drain(ctx_, kPortP, p_);
    ut_trs_set_credit(ctx_, kPortP, credit_);
    ut_trs_set_absorb(ctx_, kPortP, absorb_held_ ? 0 : absorb_);
    ut_trs_set_shaper_bucket(ctx_, kPortP, kBurstBytes * 1e9L / bps_);
    ut_trs_set_mock_tsc_script(ctx_, &now, 1);
    const uint64_t d[2] = {first_ns, 200}, g[2] = {300, 250};
    ut_trs_set_burst_cost(ctx_, d, g, 2);
    ut_trs_set_mixed_accept(ctx_, mixed_accept);
    ut_trs_set_mixed_stall(ctx_, tail_stall_ns);
    ut_trs_set_rtcp(ctx_, rtcp);
    ut_trs_set_first_pkt_frame(ctx_, &frame_, kPktLen);
    if (drain_model_) {
      ut_trs_set_pmd_done_batches(ctx_, kRsThresh, 0, 0);
      ASSERT_TRUE(ut_trs_set_drain_model(ctx_, true));
    }
    ut_trs_enqueue_first_pkt(ctx_, kTargetTsc);
    bool pre_armed = false;
    max_visit_calls_ = 0;
    max_visit_ns_ = 0;
    for (int round = 0; round < kRoundCap && !ut_trs_real_send_count(ctx_); round++) {
      const uint32_t calls = ut_trs_burst_call_count(ctx_);
      const uint32_t mixed = ut_trs_mixed_calls(ctx_);
      const uint32_t frees = ut_trs_pmd_frees(ctx_);
      const uint64_t in = round ? ut_trs_last_tsc(ctx_) : now;
      const int ret = ut_trs_call_rl_tasklet(ctx_);
      max_visit_calls_ =
          std::max(max_visit_calls_, ut_trs_burst_call_count(ctx_) - calls);
      max_visit_ns_ = std::max(max_visit_ns_, ut_trs_last_tsc(ctx_) - in);
      if (ut_trs_real_send_count(ctx_))
        pkt0_visit_frees_ = ut_trs_pmd_frees(ctx_) - frees;
      if (ut_trs_mixed_calls(ctx_) > mixed) {
        mixed_ret_ = ret;
        mixed_status_ = ut_trs_get_stat_trs_ret_code(ctx_);
      }
      uint64_t round_ns = kOthersNs;
      if (!pre_armed && ut_trs_burst_call_count(ctx_) > calls) {
        pre_armed = true;
        pre_arm_end_tsc_ = ut_trs_last_tsc(ctx_);
        round_ns = deferred_round_ns;
        /* the deferred visit finds the batches the PMD kept of the frame before, and
         * pad_batches of the pre-arm's pads */
        if (pad_batches) {
          ut_trs_set_pmd_done_batches(ctx_, kRsThresh, ut_trs_rl_kept_batches(),
                                      kUtRlColdBatchFreeNs);
          ut_trs_add_pmd_done_batches(ctx_, pad_batches, kUtRlHotBatchFreeNs);
        }
      }
      ut_trs_advance_mock_tsc(ctx_, round_ns);
    }
  }

  /* the first burst cost, stalled and absorbed, whose plan needs a burst more than the
   * hold's, at a window entry within a burst of the warm-up open: packet 0 then leaves
   * in a call with pads */
  uint64_t TailFirstNs() {
    for (entry_ns_ = 0; entry_ns_ < ST_TX_VIDEO_RL_MAX_PADS_PER_BURST * p_;
         entry_ns_ += (uint64_t)(p_ / 2))
      for (uint64_t first = ut_trs_rl_stall_ns() + 100; first < absorb_; first += 100) {
        Frame(first, 0, false);
        if (ut_trs_mixed_calls(ctx_)) return first;
      }
    return 0;
  }

  /* the deferred round of the last Frame() that brings it back after its queued pads
   * drained, half the trim pads before the target */
  uint64_t BackBeforeTheTargetNs() const {
    return (uint64_t)(kTargetTsc - trim_pads_ * p_ / 2 - pre_arm_end_tsc_);
  }

  void ExpectPacket0Once() {
    void* frame = nullptr;
    EXPECT_EQ(1u, ut_trs_real_send_count(ctx_));
    EXPECT_EQ(1u, ut_trs_frame_starts(ctx_, &frame));
    EXPECT_EQ(&frame_, frame);
    EXPECT_GE(ut_trs_modeled_launch_tsc(ctx_, kPortP), kTargetTsc);
    EXPECT_EQ(0u, ut_trs_pad_inflight_num(ctx_));
    EXPECT_EQ(1u, ut_trs_pad_refcnt(ctx_));
    EXPECT_EQ(1u, ut_trs_trim_pad_refcnt(ctx_, 0));
    EXPECT_EQ(1u, ut_trs_trim_pad_refcnt(ctx_, 1));
    EXPECT_EQ(1u, ut_trs_port_packets(ctx_));
    EXPECT_EQ(kPktLen, ut_trs_port_bytes(ctx_));
  }

  ut_trs_ctx* ctx_ = nullptr;
  bool absorb_held_ = false;     /* absorb_ns 0: the hold on every counted stall */
  bool drain_model_ = false;     /* the drain model on, on a deep queue */
  uint64_t entry_ns_ = 0;        /* Frame()'s window entry past the warm-up open */
  uint64_t pre_arm_end_tsc_ = 0; /* the end of Frame()'s pre-arm visit */
  int mixed_ret_ = -1, mixed_status_ = 0;
  uint32_t pkt0_visit_frees_ = 0;
  uint32_t max_visit_calls_ = 0; /* the most burst calls and time of a visit of Frame() */
  uint64_t max_visit_ns_ = 0;
  /* done batches freed in the visit that sent packet 0 */ /* the visit of the call
                                                              with pads and pkts */
  int frame_ = 0; /* stands in for packet 0's st_frame_trans */
  uint64_t bps_ = 0;
  long double p_ = 0, credit_ = 0, absorb_ = 0, trs_ = 0;
  uint32_t warm_pkts_ = 0;
  const int64_t trim_pads_ = st_tx_video_rl_trim_pads(kPktLen);
};

/* the frame starts once, for packet 0's frame, though a pad heads the call */
TEST_F(St20TxRlTail, Packet0BehindTailPadsStartsItsFrameOnce) {
  const uint64_t first = TailFirstNs();
  ASSERT_NE(0u, first);
  ExpectPacket0Once();
}

/* The drain bound after the call with pads and packet 0 ends no later than packet 0 on
 * the modeled wire: it counts the cut pads by their bytes, as the plan does. */
TEST_F(St20TxRlTail, DrainBoundEndsNoLaterThanPacket0) {
  drain_model_ = true;
  ASSERT_NE(0u, TailFirstNs());
  EXPECT_LE(ut_trs_drain_tsc(ctx_, kPortP),
            (uint64_t)(ut_trs_modeled_launch_tsc(ctx_, kPortP) + p_));
}

/* the NIC takes one pad of the call: the refused pads go ahead of packet 0, never early
 */
TEST_F(St20TxRlTail, PartlyRefusedTailPadsRetryAheadOfPacket0) {
  const uint64_t first = TailFirstNs();
  ASSERT_NE(0u, first);
  const uint64_t full_launch = (uint64_t)ut_trs_modeled_launch_tsc(ctx_, kPortP);
  Frame(first, 1, false);
  ASSERT_EQ(1u, ut_trs_mixed_calls(ctx_));
  ExpectPacket0Once();
  EXPECT_LE(ut_trs_last_pad_send_tsc(ctx_), ut_trs_last_real_send_tsc(ctx_));
  EXPECT_GE(ut_trs_modeled_launch_tsc(ctx_, kPortP), full_launch);
}

/* RTCP buffers packet 0's pkts only, never the pads that share its call */
TEST_F(St20TxRlTail, RtcpBuffersNoTailPad) {
  const uint64_t first = TailFirstNs();
  ASSERT_NE(0u, first);
  Frame(first, 0, true);
  ASSERT_EQ(1u, ut_trs_mixed_calls(ctx_));
  uint32_t pads = 0;
  EXPECT_EQ(1u, ut_trs_rtcp_buffered(ctx_, &pads));
  EXPECT_EQ(0u, pads);
  ExpectPacket0Once();
}

/* a stall before the doorbell of packet 0's call, past the pads queued ahead, counts
 * as the pre-arm's do; one the queued pads cover does not */
TEST_F(St20TxRlTail, TailCallStallIsCounted) {
  const uint64_t first = TailFirstNs();
  ASSERT_NE(0u, first);
  const uint64_t counted = ut_trs_stat_recalculate_warmup(ctx_);
  Frame(first, 0, false, 2 * ut_trs_rl_stall_ns());
  ASSERT_EQ(1u, ut_trs_mixed_calls(ctx_));
  EXPECT_EQ(counted, ut_trs_stat_recalculate_warmup(ctx_));
  Frame(first, 0, false, (uint64_t)(warm_pkts_ * trs_));
  ASSERT_EQ(1u, ut_trs_mixed_calls(ctx_));
  EXPECT_EQ(counted + 1, ut_trs_stat_recalculate_warmup(ctx_));
  ExpectPacket0Once();
}

/* A deferred visit back after the queued pads drained finds done batches past the
 * PMD's free threshold: the visit that sends packet 0 frees no more of them, in any of
 * its calls, than the hold's release visit, which holds packet 0 on that stall, and
 * packet 0 is late of the frame without them by at most those frees, never early. */
TEST_F(St20TxRlTail, TailVisitFreesNoMoreThanTheRelease) {
  const uint64_t first = TailFirstNs();
  ASSERT_NE(0u, first);
  const uint64_t round = BackBeforeTheTargetNs();
  const uint32_t pad_batches = ut_trs_rl_kept_batches();
  Frame(first, 0, false, 0, 0, round);
  const long double launch = ut_trs_modeled_launch_tsc(ctx_, kPortP);
  absorb_held_ = true;
  Frame(first, 0, false, 0, pad_batches, round);
  ASSERT_EQ(0u, ut_trs_mixed_calls(ctx_));
  const uint32_t held_frees = pkt0_visit_frees_;
  ASSERT_GT(held_frees, 0u);
  absorb_held_ = false;
  Frame(first, 0, false, 0, pad_batches, round);
  ASSERT_EQ(1u, ut_trs_mixed_calls(ctx_));
  EXPECT_LE(pkt0_visit_frees_, held_frees);
  EXPECT_LE(ut_trs_modeled_launch_tsc(ctx_, kPortP),
            launch + pkt0_visit_frees_ * kUtRlColdBatchFreeNs +
                ut_trs_fine_trim_ns(ctx_, kPortP));
  ExpectPacket0Once();
}

/* the NIC takes pads only of packet 0's call: the visit had work, and packet 0 retries */
TEST_F(St20TxRlTail, PadsOnlyAcceptReportsWork) {
  const uint64_t first = TailFirstNs();
  ASSERT_NE(0u, first);
  Frame(first, 1, false);
  ASSERT_EQ(1u, ut_trs_mixed_calls(ctx_));
  EXPECT_NE(MTL_TASKLET_ALL_DONE, mixed_ret_);
  EXPECT_GT(mixed_status_, 0);
  ExpectPacket0Once();
}

/* a deferred visit back after the pads drained sends a tail short of the trim pads: late
 * by under a pad past the timeslot aim, never early */
TEST_F(St20TxRlTail, LateTailVisitIsLateByUnderAPad) {
  const uint64_t first = TailFirstNs();
  ASSERT_NE(0u, first);
  Frame(first, 0, false, 0, 0, BackBeforeTheTargetNs());
  ASSERT_EQ(1u, ut_trs_mixed_calls(ctx_));
  const long double late = ut_trs_modeled_launch_tsc(ctx_, kPortP) - kTargetTsc;
  EXPECT_GE(late, 0);
  EXPECT_LT(late, credit_ - kBurstBytes * 1e9L / bps_ + p_);
  ExpectPacket0Once();
}

/* Absorbing a first pad burst's stall makes no visit of more burst calls than the
 * hold of packet 0 on that stall, nor longer but by a TSC read: a burst the plan needs
 * more goes with packet 0. */
TEST_F(St20TxRlTail, AbsorbedStallVisitIsNoLongerThanTheHold) {
  const uint32_t dims[][2] = {{1280, 720}, {1920, 1080}, {3840, 2160}};
  for (const auto& d : dims) {
    ASSERT_NO_FATAL_FAILURE(Plan(d[0], d[1]));
    for (uint64_t first = ut_trs_rl_stall_ns() + 100; first < absorb_; first += 100) {
      const std::string what = std::to_string(d[1]) + "p first " + std::to_string(first);
      absorb_held_ = true;
      Frame(first, 0, false);
      const uint32_t held_calls = max_visit_calls_;
      const uint64_t held_ns = max_visit_ns_;
      absorb_held_ = false;
      Frame(first, 0, false);
      EXPECT_LE(max_visit_calls_, held_calls) << what;
      EXPECT_LE(max_visit_ns_, held_ns + kTscReadNs) << what;
      if (HasFailure()) return;
    }
  }
}

}  // namespace
