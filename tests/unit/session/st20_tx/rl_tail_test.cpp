/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <gtest/gtest.h>

#include <cstdint>

#include "mtl_sch_api.h"
#include "session/st20_tx_harness.h"
#include "session/st_video_transmitter_harness.h"
#include "st2110/st_tx_video_rl.h"

/* A late entry whose plan takes a pad within absorb_ns sends the pads that fit with
 * packet 0 in one call: pads first, then packet 0's pkts. */
namespace {

constexpr int kPortP = 0;
constexpr uint64_t kTargetTsc = 100000000;
constexpr long double kBurstBytes = 2048;
constexpr uint64_t kOthersNs = 12000;
constexpr int kRoundCap = 100000;
constexpr uint16_t kPktLen = 1262; /* packet 0's bytes, those of a NORMAL pkt */

class St20TxRlTail : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(0, ut_trs_init());
    const float trained[1] = {164};
    ut_txv_ctx* txv = ut_txv_create();
    ASSERT_NE(nullptr, txv);
    ut_txv_set_rl_format(txv, 3840, 2160, false, ST_FPS_P59_94);
    ASSERT_EQ(0, ut_txv_init_rl_pacing(txv, 1, 0, false, trained, false));
    bps_ = ut_txv_rl_queue_bps(txv, kPortP);
    p_ = ut_txv_rl_drain(txv, kPortP);
    credit_ = ut_txv_rl_credit(txv, kPortP);
    absorb_ = ut_txv_rl_absorb(txv, kPortP);
    trs_ = ut_txv_pacing_trs(txv);
    warm_pkts_ = ut_txv_pacing_warm_pkts(txv);
    pad_size_ = ut_txv_rl_pad_size(txv, kPortP);
    ut_txv_destroy(txv);
    trim_pads_ = st_tx_video_rl_trim_pads(pad_size_);
    ASSERT_GT(absorb_, p_);
  }
  void TearDown() override {
    if (ctx_) ut_trs_destroy(ctx_);
  }

  /* one frame whose first visit in the window finds half the trim pads to the target */
  void Frame(uint16_t mixed_accept, bool rtcp, uint64_t tail_stall_ns = 0) {
    if (ctx_) ut_trs_destroy(ctx_);
    uint64_t now =
        kTargetTsc - (uint64_t)(trim_pads_ * p_ / 2 - credit_) - kOthersNs - 1000;
    ctx_ = ut_trs_create();
    ASSERT_NE(nullptr, ctx_);
    ut_trs_set_trs(ctx_, trs_);
    ut_trs_set_warm_pkts_cap(ctx_, warm_pkts_);
    ut_trs_set_drain(ctx_, kPortP, p_);
    ut_trs_set_credit(ctx_, kPortP, credit_);
    ASSERT_EQ(0, ut_trs_set_pad_sizes(ctx_, kPktLen, pad_size_));
    ut_trs_set_absorb(ctx_, kPortP, absorb_);
    ut_trs_set_shaper_bucket(ctx_, kPortP, kBurstBytes * 1e9L / bps_);
    ut_trs_set_mock_tsc_script(ctx_, &now, 1);
    const uint64_t d[2] = {750, 200}, g[2] = {300, 250};
    ut_trs_set_burst_cost(ctx_, d, g, 2);
    ut_trs_set_mixed_accept(ctx_, mixed_accept);
    ut_trs_set_mixed_stall(ctx_, tail_stall_ns);
    ut_trs_set_rtcp(ctx_, rtcp);
    ut_trs_set_first_pkt_frame(ctx_, &frame_, kPktLen);
    ut_trs_enqueue_first_pkt(ctx_, kTargetTsc);
    for (int round = 0; round < kRoundCap && !ut_trs_real_send_count(ctx_); round++) {
      const uint32_t mixed = ut_trs_mixed_calls(ctx_);
      const int ret = ut_trs_call_rl_tasklet(ctx_);
      if (ut_trs_mixed_calls(ctx_) > mixed) {
        mixed_ret_ = ret;
        mixed_status_ = ut_trs_get_stat_trs_ret_code(ctx_);
      }
      ut_trs_advance_mock_tsc(ctx_, round ? 1000 : kOthersNs);
    }
    ASSERT_EQ(1u, ut_trs_mixed_calls(ctx_));
  }

  void ExpectPacket0Once() {
    void* frame = nullptr;
    EXPECT_EQ(1u, ut_trs_real_send_count(ctx_));
    EXPECT_EQ(1u, ut_trs_frame_starts(ctx_, &frame));
    EXPECT_EQ(&frame_, frame);
    EXPECT_GE(ut_trs_modeled_launch_tsc(ctx_, kPortP), kTargetTsc);
    EXPECT_EQ(0u, ut_trs_pad_inflight_num(ctx_));
    EXPECT_EQ(1u, ut_trs_pad_refcnt(ctx_));
    EXPECT_EQ(1u, ut_trs_pre_arm_pad_refcnt(ctx_));
    EXPECT_EQ(1u, ut_trs_trim_pad_refcnt(ctx_, 0));
    EXPECT_EQ(1u, ut_trs_trim_pad_refcnt(ctx_, 1));
    EXPECT_EQ(1u, ut_trs_port_packets(ctx_));
    EXPECT_EQ(kPktLen, ut_trs_port_bytes(ctx_));
  }

  ut_trs_ctx* ctx_ = nullptr;
  int mixed_ret_ = -1, mixed_status_ = 0; /* the visit of the call with pads and pkts */
  int frame_ = 0;                         /* stands in for packet 0's st_frame_trans */
  uint64_t bps_ = 0;
  long double p_ = 0, credit_ = 0, absorb_ = 0, trs_ = 0;
  uint32_t warm_pkts_ = 0;
  uint16_t pad_size_ = 0;
  int64_t trim_pads_ = 0;
};

/* the frame starts once, for packet 0's frame, though a pad heads the call */
TEST_F(St20TxRlTail, Packet0BehindTailPadsStartsItsFrameOnce) {
  ASSERT_NO_FATAL_FAILURE(Frame(0, false));
  ExpectPacket0Once();
}

/* the NIC takes one pad of the call: the refused pads go ahead of packet 0, never early
 */
TEST_F(St20TxRlTail, PartlyRefusedTailPadsRetryAheadOfPacket0) {
  ASSERT_NO_FATAL_FAILURE(Frame(0, false));
  const uint64_t full_launch = (uint64_t)ut_trs_modeled_launch_tsc(ctx_, kPortP);
  ASSERT_NO_FATAL_FAILURE(Frame(1, false));
  ExpectPacket0Once();
  EXPECT_LE(ut_trs_last_pad_send_tsc(ctx_), ut_trs_last_real_send_tsc(ctx_));
  EXPECT_GE(ut_trs_modeled_launch_tsc(ctx_, kPortP), full_launch);
}

/* RTCP buffers packet 0's pkts only, never the pads that share its call */
TEST_F(St20TxRlTail, RtcpBuffersNoTailPad) {
  ASSERT_NO_FATAL_FAILURE(Frame(0, true));
  uint32_t pads = 0;
  EXPECT_EQ(1u, ut_trs_rtcp_buffered(ctx_, &pads));
  EXPECT_EQ(0u, pads);
  ExpectPacket0Once();
}

/* a stall before the doorbell of packet 0's call counts as the pre-arm's do */
TEST_F(St20TxRlTail, TailCallStallIsCounted) {
  ASSERT_NO_FATAL_FAILURE(Frame(0, false));
  EXPECT_EQ(0u, ut_trs_stat_recalculate_warmup(ctx_));
  ASSERT_NO_FATAL_FAILURE(Frame(0, false, 2 * ut_trs_rl_stall_ns()));
  EXPECT_EQ(1u, ut_trs_stat_recalculate_warmup(ctx_));
  ExpectPacket0Once();
}

/* the NIC takes pads only of packet 0's call: the visit had work, and packet 0 retries */
TEST_F(St20TxRlTail, PadsOnlyAcceptReportsWork) {
  ASSERT_NO_FATAL_FAILURE(Frame(1, false));
  EXPECT_NE(MTL_TASKLET_ALL_DONE, mixed_ret_);
  EXPECT_GT(mixed_status_, 0);
  ExpectPacket0Once();
}

}  // namespace
