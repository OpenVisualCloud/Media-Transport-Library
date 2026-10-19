/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Pins tv_pkts_capable_chain(): zero copy keeps the app a free frame buffer only while
 * the pkts still holding a frame cannot span every other buffer. Those are the pkts the
 * builder ring holds unsent, ST_TX_VIDEO_SESSIONS_RING_SIZE, and the sent ones the TX
 * queue holds until it frees them: a zero-copy pkt chains a header and a payload
 * segment, 2 descriptors, so nb_tx_desc / 2. The RTCP retransmit ring holds the last
 * tv_rtcp_buffer_size() pkts sent until newer ones come, so a ring spanning every
 * buffer never frees one: a deadlock. That is on a deep port (2048 here); every other
 * port keeps main's bound.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='St20TxZeroCopyTest.*'
 */

#include <gtest/gtest.h>

#include "common/ut_known_bug.h"
#include "session/st20_tx_harness.h"

namespace {

constexpr uint16_t kNbTxDesc = 2048;
constexpr int kBuilderRing = 512; /* ST_TX_VIDEO_SESSIONS_RING_SIZE */
constexpr int kHeld = kNbTxDesc / 2 + kBuilderRing;

class St20TxZeroCopyTest : public ::testing::Test {
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

TEST_F(St20TxZeroCopyTest, OneSpareBufferCoveringTheHeldPktsKeepsZeroCopy) {
  /* 720p 4:2:2 10-bit BPM, 2 buffers: 1829 pkts, more than the 1536 held */
  EXPECT_TRUE(ut_txv_pkts_capable_chain(ctx_, kNbTxDesc, 1829, 2, false));
  EXPECT_TRUE(ut_txv_pkts_capable_chain(ctx_, kNbTxDesc, kHeld, 2, false));
}

TEST_F(St20TxZeroCopyTest, SpareBuffersShortOfTheHeldPktsCopy) {
  /* 576i fields of 432 pkts: 3 buffers hold 864, short of 1536 */
  EXPECT_FALSE(ut_txv_pkts_capable_chain(ctx_, kNbTxDesc, 432, 3, false));
  EXPECT_FALSE(ut_txv_pkts_capable_chain(ctx_, kNbTxDesc, kHeld - 1, 2, false));
}

TEST_F(St20TxZeroCopyTest, RtcpRingSpanningEverySpareBufferCopies) {
  /* the default ring keeps the last nb_tx_desc pkts, 2048, in place of the queue's */
  constexpr int kRtcpHeld = kNbTxDesc + kBuilderRing;
  EXPECT_FALSE(ut_txv_pkts_capable_chain(ctx_, kNbTxDesc, kRtcpHeld - 1, 2, true));
  EXPECT_TRUE(ut_txv_pkts_capable_chain(ctx_, kNbTxDesc, kRtcpHeld, 2, true));
  /* 2 buffers: 720p (BPM, 1829 pkts) and 1080i fields (GPM_SL, 2160) copy, 1080p not */
  EXPECT_FALSE(ut_txv_pkts_capable_chain(ctx_, kNbTxDesc, 1829, 2, true));
  EXPECT_FALSE(ut_txv_pkts_capable_chain(ctx_, kNbTxDesc, 2160, 2, true));
  EXPECT_TRUE(ut_txv_pkts_capable_chain(ctx_, kNbTxDesc, 4320, 2, true));
}

/* main's bound off a deep port: the spare buffers hold nb_tx_desc pkts or more */
TEST_F(St20TxZeroCopyTest, NotDeepPortZeroCopiesFromNbTxDescSparePkts) {
  EXPECT_TRUE(ut_txv_pkts_capable_chain(ctx_, 512, 512, 2, false));
  EXPECT_FALSE(ut_txv_pkts_capable_chain(ctx_, 512, 511, 2, false));
}

/* at nb_tx_desc 512 the default 1024 ring is longer than the queue: 576p BPM */
TEST_F(St20TxZeroCopyTest, NotDeepRtcpRingSpanningEverySpareBufferCopies) {
  UT_KNOWN_BUG(
      "main's bound counts neither the RTCP ring nor the builder ring, so their "
      "frames never come back");
  EXPECT_FALSE(ut_txv_pkts_capable_chain(ctx_, 512, 823, 2, true));
}

/* the builder reclaims the session's own queues: not shared ones, other schedulers burst
 * on them, and not with RTCP, whose ring keeps the frames */
TEST_F(St20TxZeroCopyTest, TxReclaimNeedsReclaimingDedicatedQueuesAndNoRtcp) {
  const ut_txv_reclaim_case reclaims = {{32, 32}, 1, false, false, false, 823, 2};
  ut_txv_reclaim_case c = reclaims;
  EXPECT_TRUE(ut_txv_tx_reclaims(ctx_, &c));
  c.num_port = 2;
  EXPECT_TRUE(ut_txv_tx_reclaims(ctx_, &c));

  struct {
    const char* what;
    void (*change)(ut_txv_reclaim_case*);
  } const cases[] = {
      {"port", [](ut_txv_reclaim_case* c) { c->port_rs_thresh[0] = 0; }},
      {"r leg",
       [](ut_txv_reclaim_case* c) {
         c->num_port = 2;
         c->port_rs_thresh[1] = 0;
       }},
      {"shared", [](ut_txv_reclaim_case* c) { c->shared_txq = true; }},
      {"rtcp", [](ut_txv_reclaim_case* c) { c->rtcp = true; }},
      {"rtp level", [](ut_txv_reclaim_case* c) { c->rtp_level = true; }},
  };
  for (const auto& k : cases) {
    c = reclaims;
    k.change(&c);
    EXPECT_FALSE(ut_txv_tx_reclaims(ctx_, &c)) << k.what;
  }
}

/* the next frame's 2 descriptors a pkt free the batch the frame before ended in */
TEST_F(St20TxZeroCopyTest, TxReclaimNeedsFramesOfTheRsThreshOfEveryPort) {
  for (uint16_t rs : {32, 64}) {
    SCOPED_TRACE(rs);
    ut_txv_reclaim_case c = {{rs, rs}, 2, false, false, false, rs, 2};
    EXPECT_TRUE(ut_txv_tx_reclaims(ctx_, &c));
    c.total_pkts = rs - 1;
    EXPECT_FALSE(ut_txv_tx_reclaims(ctx_, &c));
  }
  ut_txv_reclaim_case r_leg = {{32, 64}, 2, false, false, false, 63, 2};
  EXPECT_FALSE(ut_txv_tx_reclaims(ctx_, &r_leg));
}

/* tv_attach() decides it before the bound, which it lifts */
TEST_F(St20TxZeroCopyTest, AttachKeepsSdZeroCopyWhereItReclaims) {
  ut_txv_reclaim_case c = {{32, 32}, 1, false, false, false, 823, 2};
  bool zero_copy, reclaim;
  ut_txv_init_tx_chain(ctx_, &c, kNbTxDesc, &zero_copy, &reclaim);
  EXPECT_TRUE(zero_copy);
  EXPECT_TRUE(reclaim);
  c.total_pkts = 432; /* 576i fields */
  ut_txv_init_tx_chain(ctx_, &c, kNbTxDesc, &zero_copy, &reclaim);
  EXPECT_TRUE(zero_copy);
  EXPECT_TRUE(reclaim);
  c.total_pkts = 823;

  c.port_rs_thresh[0] = 0;
  ut_txv_init_tx_chain(ctx_, &c, kNbTxDesc, &zero_copy, &reclaim);
  EXPECT_FALSE(zero_copy);
  EXPECT_FALSE(reclaim);

  /* a copy session sends from its own mbufs: nothing to wait for */
  c = {{32, 32}, 1, false, false, false, 823, 2, true};
  ut_txv_init_tx_chain(ctx_, &c, kNbTxDesc, &zero_copy, &reclaim);
  EXPECT_FALSE(zero_copy);
  EXPECT_FALSE(reclaim);
}

}  // namespace
