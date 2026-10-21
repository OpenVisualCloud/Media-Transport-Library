/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Pins the direct chain of a zero-copy ST 2110-20 frame session: every payload mbuf but
 * the frame's last points straight into the frame, and the last one alone holds the
 * frame's extbuf, so the frame is done once that pkt is freed. It goes last only where
 * the PMD frees a queue's mbufs in ring order once the NIC is done (ice, iavf, igc); any
 * other session keeps a frame ref a pkt. Teardown frees the pkts software still holds
 * before the queues flushed, so it holds the frames in flight until no payload is out.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='*St20TxDirectChain*'
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cerrno>
#include <string>

#include "common/ut_tx_ring.h"
#include "session/st20_tx_harness.h"

namespace {

constexpr int kP = 0;
constexpr int kR = 1;

class St20TxDirectChainEligibilityTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_txv_init(), 0);
    ctx_ = ut_txv_create();
    ASSERT_NE(ctx_, nullptr);
  }
  void TearDown() override {
    ut_txv_destroy(ctx_);
  }
  ut_txv_ctx* ctx_ = nullptr;
};

TEST_F(St20TxDirectChainEligibilityTest, AnIavfFrameSessionIsDirect) {
  ut_txv_direct_chain_case c = {};
  EXPECT_TRUE(ut_txv_tx_direct_chain(ctx_, &c));
}

TEST_F(St20TxDirectChainEligibilityTest, AnIceSessionIsDirect) {
  ut_txv_direct_chain_case c = {};
  c.drv[kP] = c.drv[kR] = UT_TXV_DRV_ICE;
  EXPECT_TRUE(ut_txv_tx_direct_chain(ctx_, &c));
}

TEST_F(St20TxDirectChainEligibilityTest, AnIgcSessionIsDirect) {
  ut_txv_direct_chain_case c = {};
  c.drv[kP] = c.drv[kR] = UT_TXV_DRV_IGC;
  EXPECT_TRUE(ut_txv_tx_direct_chain(ctx_, &c));
}

TEST_F(St20TxDirectChainEligibilityTest, ASliceLevelSessionIsDirect) {
  ut_txv_direct_chain_case c = {};
  c.slice_level = true;
  EXPECT_TRUE(ut_txv_tx_direct_chain(ctx_, &c));
}

struct IneligibleCase {
  const char* name;
  ut_txv_direct_chain_case c;
};

class St20TxDirectChainIneligibleTest
    : public St20TxDirectChainEligibilityTest,
      public ::testing::WithParamInterface<IneligibleCase> {};

TEST_P(St20TxDirectChainIneligibleTest, KeepsAFrameRefAPkt) {
  EXPECT_FALSE(ut_txv_tx_direct_chain(ctx_, &GetParam().c));
}

ut_txv_direct_chain_case Only(bool ut_txv_direct_chain_case::* field) {
  ut_txv_direct_chain_case c = {};
  c.*field = true;
  return c;
}

ut_txv_direct_chain_case OnDrv(int port, enum ut_txv_drv drv) {
  ut_txv_direct_chain_case c = {};
  c.drv[port] = drv;
  return c;
}

INSTANTIATE_TEST_SUITE_P(
    Conditions, St20TxDirectChainIneligibleTest,
    ::testing::Values(
        IneligibleCase{"NoChain", Only(&ut_txv_direct_chain_case::no_chain)},
        IneligibleCase{"RtpLevelAppFillsTheChain",
                       Only(&ut_txv_direct_chain_case::rtp_level)},
        IneligibleCase{"RtcpResendsOldPkts", Only(&ut_txv_direct_chain_case::rtcp)},
        IneligibleCase{"MonoPoolHeadersShareTheChainPool",
                       Only(&ut_txv_direct_chain_case::mono_pool)},
        IneligibleCase{"IovaPaCopiesIntoTheChain",
                       Only(&ut_txv_direct_chain_case::iova_pa)},
        IneligibleCase{"SharedTxQueue", Only(&ut_txv_direct_chain_case::shared_txq)},
        IneligibleCase{"Mlx5LegRFreesInlinedPktsAtBurst", OnDrv(kR, UT_TXV_DRV_MLX5)},
        IneligibleCase{"UnknownDriverLegP", OnDrv(kP, UT_TXV_DRV_DEFAULT)},
        IneligibleCase{"I40eNotVerified", OnDrv(kP, UT_TXV_DRV_I40E)}),
    [](const ::testing::TestParamInfo<IneligibleCase>& info) {
      return std::string(info.param.name);
    });

class St20TxDirectChainAttachTest : public St20TxDirectChainEligibilityTest {};

/* the attach stops at its TX queue request, after the chain decision */
TEST_F(St20TxDirectChainAttachTest, AttachMakesAnIavfFrameSessionDirect) {
  bool direct = false;
  EXPECT_LT(ut_txv_attach_direct_chain(ctx_, false, &direct), 0);
  EXPECT_TRUE(direct);
}

TEST_F(St20TxDirectChainAttachTest, AttachDecidesAfterItTookTheMonoPool) {
  bool direct = true;
  EXPECT_LT(ut_txv_attach_direct_chain(ctx_, true, &direct), 0);
  EXPECT_FALSE(direct);
}

/* 640x8 4:2:2 10-bit BPM: 12800 bytes in 11 pkts, most crossing a line, the last short */
class St20TxDirectChainTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_txv_init(), 0);
    ctx_ = ut_txv_create();
    ASSERT_NE(ctx_, nullptr);
  }
  void TearDown() override {
    if (setup_) {
      EXPECT_EQ(ut_txv_pipe_teardown(ctx_), 0);
    }
    ut_txv_destroy(ctx_);
  }
  void Setup(int num_port, bool iavf = true, uint32_t linesize_pad = 0) {
    ut_txv_pipe_case c = {};
    c.width = 640;
    c.height = 8;
    c.frames_cnt = 2;
    c.num_port = num_port;
    c.ring_mode = UT_TXR_FREE_ON_CLEANUP;
    c.free_thresh = 32;
    for (int i = 0; i < num_port; i++) c.nb_tx_desc[i] = 512;
    c.linesize_pad = linesize_pad;
    c.iavf = iavf;
    ASSERT_EQ(ut_txv_pipe_setup(ctx_, &c), 0);
    setup_ = true;
    ut_txv_pipe_geometry(ctx_, &pkt_len_, &line_bytes_, &frame_size_, &total_pkts_);
    ASSERT_LE(total_pkts_, UT_TXV_PIPE_BUILT_MAX);
  }
  void BuildFrame() {
    ASSERT_EQ(ut_txv_pipe_build(ctx_, total_pkts_), total_pkts_);
  }
  void ExpectAtFrameOffset(int idx, const ut_txv_payload& payload) {
    uint64_t off = (uint64_t)idx * pkt_len_;
    EXPECT_EQ(payload.addr_off, off) << "pkt " << idx;
    EXPECT_EQ(payload.iova_off, off) << "pkt " << idx;
    EXPECT_EQ(payload.len, std::min<uint64_t>(pkt_len_, frame_size_ - off))
        << "pkt " << idx;
  }
  ut_txv_ctx* ctx_ = nullptr;
  bool setup_ = false;
  uint32_t pkt_len_ = 0;
  uint32_t line_bytes_ = 0;
  uint32_t frame_size_ = 0;
  int total_pkts_ = 0;
};

TEST_F(St20TxDirectChainTest, EveryPayloadButTheLastIsADirectMbufOnTheFrame) {
  Setup(1);
  BuildFrame();

  for (int i = 0; i < total_pkts_ - 1; i++) {
    ut_txv_payload payload;
    ut_txv_pipe_payload(ctx_, i, &payload);
    EXPECT_FALSE(payload.extbuf) << "pkt " << i;
    ExpectAtFrameOffset(i, payload);
  }
}

TEST_F(St20TxDirectChainTest, TheLastPayloadIsTheFramesExtbuf) {
  Setup(1);
  BuildFrame();

  ut_txv_payload payload;
  ut_txv_pipe_payload(ctx_, total_pkts_ - 1, &payload);
  EXPECT_TRUE(payload.extbuf);
  ExpectAtFrameOffset(total_pkts_ - 1, payload);
}

TEST_F(St20TxDirectChainTest, TheLastPktAloneHoldsTheFrame) {
  Setup(1);
  BuildFrame();

  ut_txv_pipe_free_last_built(ctx_, kP);
  EXPECT_EQ(ut_txv_notify_frame_done_calls(ctx_), 1);
  EXPECT_EQ(ut_txv_notify_frame_done_idx(ctx_), 0);
  ut_txv_pipe_free_built(ctx_, kP, total_pkts_);
  EXPECT_EQ(ut_txv_notify_frame_done_calls(ctx_), 1);
}

/* the queue sent and freed every pkt built while the builder was held up */
TEST_F(St20TxDirectChainTest, FrameIsNotDoneWhileItsLastPktIsUnbuilt) {
  Setup(1);
  int built = ut_txv_pipe_build(ctx_, 1);
  ASSERT_GT(built, 0);
  ASSERT_LT(built, total_pkts_);

  ut_txv_pipe_free_built(ctx_, kP, built);

  EXPECT_EQ(ut_txv_notify_frame_done_calls(ctx_), 0);
}

/* a PMD that may free out of order or at burst time, as mlx5 does */
TEST_F(St20TxDirectChainTest, SessionOnAnotherDriverAttachesEveryPayload) {
  Setup(1, false);
  BuildFrame();

  for (int i = 0; i < total_pkts_; i++) {
    ut_txv_payload payload;
    ut_txv_pipe_payload(ctx_, i, &payload);
    EXPECT_TRUE(payload.extbuf) << "pkt " << i;
    ExpectAtFrameOffset(i, payload);
  }
}

/* the legs share each payload */
TEST_F(St20TxDirectChainTest, RedundantFrameIsHeldByTheLastPktOfBothLegs) {
  Setup(2);
  BuildFrame();

  ut_txv_pipe_free_last_built(ctx_, kP);
  EXPECT_EQ(ut_txv_notify_frame_done_calls(ctx_), 0);
  ut_txv_pipe_free_last_built(ctx_, kR);
  EXPECT_EQ(ut_txv_notify_frame_done_calls(ctx_), 1);
}

/* a crossing pkt's two runs are a padding apart, so it copies them into one */
TEST_F(St20TxDirectChainTest, PaddedFrameCopiesOnlyItsLineCrossingPkts) {
  constexpr uint32_t kPad = 64;
  Setup(1, true, kPad);
  BuildFrame();

  for (int i = 0; i < total_pkts_ - 1; i++) {
    uint64_t off = (uint64_t)i * pkt_len_;
    uint64_t line = off / line_bytes_;
    bool crosses = off + pkt_len_ > (line + 1) * line_bytes_;
    ut_txv_payload payload;
    ut_txv_pipe_payload(ctx_, i, &payload);
    EXPECT_FALSE(payload.extbuf) << "pkt " << i;
    EXPECT_EQ(payload.copy, crosses) << "pkt " << i;
    if (crosses) continue;
    uint64_t padded_off = off % line_bytes_ + line * (line_bytes_ + kPad);
    EXPECT_EQ(payload.addr_off, padded_off) << "pkt " << i;
    EXPECT_EQ(payload.iova_off, padded_off) << "pkt " << i;
  }
}

/* its line-crossing pkts copy into pkts of the copy pool, the last never crosses */
TEST_F(St20TxDirectChainTest, PaddedFrameIsHeldByItsLastPktAlone) {
  Setup(1, true, 64);
  BuildFrame();

  ut_txv_pipe_free_last_built(ctx_, kP);

  EXPECT_EQ(ut_txv_notify_frame_done_calls(ctx_), 1);
}

/* stop cleans the builder ring, holding the last pkt, before it flushes the queue */
TEST_F(St20TxDirectChainTest, StopHandsTheFrameBackOnlyOnceTheQueueFlushed) {
  Setup(1);
  BuildFrame();

  ASSERT_EQ(ut_txv_pipe_stop_last_pkt_unsent(ctx_), 0);

  EXPECT_EQ(ut_txv_done_at_flush(ctx_, kP), 0);
  EXPECT_EQ(ut_txv_notify_frame_done_calls(ctx_), 1);
}

/* R's ring, holding the payload's last ref, is cleaned after P's queue flushed */
TEST_F(St20TxDirectChainTest, RedundantStopHandsTheFrameBackOnlyOnceBothQueuesFlushed) {
  Setup(2);
  BuildFrame();

  ASSERT_EQ(ut_txv_pipe_stop_last_pkt_unsent(ctx_), 0);

  EXPECT_EQ(ut_txv_done_at_flush(ctx_, kR), 0);
  EXPECT_EQ(ut_txv_notify_frame_done_calls(ctx_), 1);
}

/* R's queue keeps its share of the payloads P's queue gave back */
TEST_F(St20TxDirectChainTest, RedundantStopKeepsAFrameRsQueueDidNotFlush) {
  Setup(2);
  BuildFrame();
  ut_txv_pipe_set_flush_ret(ctx_, kR, -EBUSY);

  ASSERT_EQ(ut_txv_pipe_stop_last_pkt_unsent(ctx_), 0);

  EXPECT_EQ(ut_txv_notify_frame_done_calls(ctx_), 0);
}

TEST_F(St20TxDirectChainTest, StopKeepsAFrameAQueueDidNotFlush) {
  Setup(1);
  BuildFrame();
  ut_txv_pipe_set_flush_ret(ctx_, kP, -EBUSY);

  ASSERT_EQ(ut_txv_pipe_stop_last_pkt_unsent(ctx_), 0);

  EXPECT_EQ(ut_txv_notify_frame_done_calls(ctx_), 0);
}

class St20TxDirectChainRecoveryTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_txv_init(), 0);
    ctx_ = ut_txv_create();
    ASSERT_NE(ctx_, nullptr);
    ASSERT_EQ(ut_txv_recovery_setup(ctx_, 2), 0);
    ASSERT_EQ(ut_txv_recovery_set_last_pkt_unsent(ctx_), 0);
  }
  void TearDown() override {
    ut_txv_destroy(ctx_);
  }
  ut_txv_ctx* ctx_ = nullptr;
};

/* P's recovery frees the pkts its transmitters hold, then flushes R's queue */
TEST_F(St20TxDirectChainRecoveryTest, FrameIsHandedBackOnlyOnceTheOtherLegFlushed) {
  ASSERT_EQ(ut_txv_recover(ctx_, kP), 0);

  EXPECT_EQ(ut_txv_done_at_flush(ctx_, kR), 0);
  EXPECT_EQ(ut_txv_notify_frame_done_calls(ctx_), 1);
  EXPECT_EQ(ut_txv_recovery_frame_refcnt(ctx_), 0);
}

}  // namespace
