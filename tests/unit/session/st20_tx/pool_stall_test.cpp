/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * A stalled TX queue must show as a refused burst before the session pools run dry:
 * with every mbuf in the ring and the pool empty nothing bursts, so nothing refuses and
 * the hang goes unseen. The production builder and TSC transmitter run into a hung ring,
 * from the start or after a lazily freeing ring kept a mbuf in every slot, with pools of
 * exactly what tv_mempool_n() asks for. On a deep port (2048 here): the builder ring, the
 * builder's and transmitter's bulks, what the ring keeps (ceil(nb_tx_desc / descriptors
 * a pkt)) and the cache slack another lcore keeps; the redundant chain pool that of the
 * leg that keeps more. One mbuf less and the alloc fails first. The transmitter is TSC;
 * RL holds a bulk more, which the helper adds. A port of 512 is not deep and gets main's
 * pools, nb_tx_desc and the builder ring, the chain pool the primary leg's.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='*St20TxPool*'
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

#include "common/ut_known_bug.h"
#include "common/ut_tx_ring.h"
#include "session/st20_tx_harness.h"

namespace {

constexpr unsigned int kCacheSlack = 192; /* 1.5 x MT_MBUF_CACHE_SIZE */
constexpr uint16_t kDeep = 2048;          /* the harnesses' deep ring */
constexpr uint64_t kStepNs = 1000;
constexpr uint64_t kLazyNs = 40000000; /* 2 frames, every ring slot keeps a mbuf */
constexpr uint64_t kHungNs = 40000000;

struct StallCase {
  const char* name;
  int num_port;
  bool copy;
  uint16_t nb_tx_desc[2];
  int hung_leg;
  bool lazy; /* drain on a ring that frees on reuse, then hang */
  uint32_t linesize_pad;
};

class St20TxPoolStallTest : public ::testing::TestWithParam<StallCase> {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_txv_init(), 0);
    ctx_ = ut_txv_create();
    ASSERT_NE(ctx_, nullptr);
  }
  void TearDown() override {
    EXPECT_EQ(ut_txv_pipe_teardown(ctx_), 0);
    ut_txv_destroy(ctx_);
  }
  ut_txv_pipe_case Base() {
    const StallCase& sc = GetParam();
    ut_txv_pipe_case c = {};
    c.width = 1920;
    c.height = 1080;
    c.frames_cnt = 3;
    c.num_port = sc.num_port;
    c.copy = sc.copy;
    c.linesize_pad = sc.linesize_pad;
    c.ring_mode = sc.lazy ? UT_TXR_FREE_ON_REUSE : UT_TXR_FREE_ON_CLEANUP;
    c.free_thresh = 32;
    for (int i = 0; i < sc.num_port; i++) {
      c.nb_tx_desc[i] = sc.nb_tx_desc[i];
      c.hung[i] = !sc.lazy && i == sc.hung_leg;
    }
    return c;
  }
  /* the helper's pools, shrink_pool one mbuf short: a leg's header pool or
   * UT_TXV_PIPE_CHAIN */
  void RunExact(int shrink_pool) {
    ut_txv_pipe_case c = Base();
    ASSERT_EQ(ut_txv_pipe_setup(ctx_, &c), 0);
    for (int i = 0; i < c.num_port; i++) {
      c.hdr_pool[i] = ut_txv_pipe_mempool_n(ctx_, i);
      c.chain_pool = std::max(c.chain_pool, c.hdr_pool[i]);
    }
    if (c.linesize_pad) c.copy_pool = ut_txv_pipe_copy_mempool_n(ctx_, c.chain_pool);
    ASSERT_EQ(ut_txv_pipe_teardown(ctx_), 0);
    ut_txv_destroy(ctx_);
    ctx_ = ut_txv_create();
    ASSERT_NE(ctx_, nullptr);

    c.hold = kCacheSlack;
    if (shrink_pool == UT_TXV_PIPE_CHAIN)
      c.chain_pool--;
    else if (shrink_pool >= 0)
      c.hdr_pool[shrink_pool]--;
    Run(c);
  }
  void Run(const ut_txv_pipe_case& c) {
    ASSERT_EQ(ut_txv_pipe_setup(ctx_, &c), 0);
    if (GetParam().lazy) {
      ut_txv_pipe_run(ctx_, kLazyNs, kStepNs);
      ut_txv_pipe_hang(ctx_, GetParam().hung_leg);
    }
    ut_txv_pipe_run(ctx_, kHungNs, kStepNs);
  }
  /* the run stops at the first refused burst or failed alloc */
  void ExpectBurstFailFirst() {
    ASSERT_NE(ut_txv_pipe_first_burst_fail(ctx_), 0u);
    EXPECT_EQ(ut_txv_pipe_first_alloc_fail(ctx_), 0u)
        << "burst fail at step " << ut_txv_pipe_first_burst_fail(ctx_);
  }
  void ExpectAllocFailFirst() {
    uint64_t burst_fail = ut_txv_pipe_first_burst_fail(ctx_);
    uint64_t alloc_fail = ut_txv_pipe_first_alloc_fail(ctx_);
    ASSERT_NE(alloc_fail, 0u);
    EXPECT_TRUE(!burst_fail || alloc_fail < burst_fail)
        << "alloc fail at step " << alloc_fail << ", burst fail at " << burst_fail;
  }
  ut_txv_ctx* ctx_ = nullptr;
};

TEST_P(St20TxPoolStallTest, StallShowsAsARefusedBurstBeforeAnAllocFail) {
  if (GetParam().copy && GetParam().nb_tx_desc[0] != kDeep)
    UT_KNOWN_BUG(
        "main's nb_tx_desc and builder ring is short of what a copy session "
        "holds, which only the 2^q - 1 rounding covers");
  RunExact(-1);
  ExpectBurstFailFirst();
}

TEST_P(St20TxPoolStallTest, TheProductionPoolsSeeTheRefusedBurstFirst) {
  Run(Base());
  ExpectBurstFailFirst();
}

INSTANTIATE_TEST_SUITE_P(
    Rings, St20TxPoolStallTest,
    testing::Values(StallCase{"Hung512", 1, false, {512, 0}, 0, false},
                    StallCase{"Hung2048", 1, false, {2048, 0}, 0, false},
                    StallCase{"Lazy512", 1, false, {512, 0}, 0, true},
                    StallCase{"Lazy2048", 1, false, {2048, 0}, 0, true},
                    StallCase{"CopyHung2048", 1, true, {2048, 0}, 0, false},
                    StallCase{"CopyLazy512", 1, true, {512, 0}, 0, true},
                    StallCase{"CopyLazy2048", 1, true, {2048, 0}, 0, true},
                    StallCase{"RedundantPHung2048", 2, false, {2048, 2048}, 0, false},
                    StallCase{"RedundantRHung2048", 2, false, {2048, 2048}, 1, false},
                    StallCase{"RedundantPLazy2048", 2, false, {2048, 2048}, 0, true},
                    StallCase{"RedundantRLazy2048", 2, false, {2048, 2048}, 1, true},
                    StallCase{"RedundantDeepRLazy", 2, false, {512, 2048}, 1, true},
                    StallCase{"RedundantDeepPLazy", 2, false, {2048, 512}, 1, true},
                    StallCase{"PaddedHung2048", 1, false, {2048, 0}, 0, false, 64},
                    StallCase{"PaddedLazy512", 1, false, {512, 0}, 0, true, 64},
                    StallCase{"PaddedLazy2048", 1, false, {2048, 0}, 0, true, 64}),
    [](const testing::TestParamInfo<StallCase>& info) { return info.param.name; });

/* a lazily freeing ring keeps a mbuf in every slot, the most it can; a hung one keeps a
 * descriptor less. A padded frame's copy bound, a pkt a line plus one a frame, and the
 * chains its copies return keep the bound loose there. Only a deep port's pools are
 * that tight: main's keep slack. */
using St20TxPoolBoundTest = St20TxPoolStallTest;

TEST_P(St20TxPoolBoundTest, OneHeaderMbufLessFailsTheAllocFirst) {
  if (GetParam().nb_tx_desc[MTL_SESSION_PORT_P] != kDeep) GTEST_SKIP() << "not deep";
  RunExact(MTL_SESSION_PORT_P);
  ExpectAllocFailFirst();
}

TEST_P(St20TxPoolBoundTest, OneRedundantHeaderMbufLessFailsTheAllocFirst) {
  if (GetParam().num_port < 2) GTEST_SKIP() << "one leg";
  if (GetParam().nb_tx_desc[MTL_SESSION_PORT_R] != kDeep) GTEST_SKIP() << "not deep";
  RunExact(MTL_SESSION_PORT_R);
  ExpectAllocFailFirst();
}

TEST_P(St20TxPoolBoundTest, OneChainMbufLessFailsTheAllocFirst) {
  if (GetParam().copy) GTEST_SKIP() << "no chain pool";
  RunExact(UT_TXV_PIPE_CHAIN);
  ExpectAllocFailFirst();
}

INSTANTIATE_TEST_SUITE_P(
    LazyRings, St20TxPoolBoundTest,
    testing::Values(StallCase{"Lazy2048", 1, false, {2048, 0}, 0, true},
                    StallCase{"CopyLazy2048", 1, true, {2048, 0}, 0, true},
                    StallCase{"RedundantRLazy2048", 2, false, {2048, 2048}, 1, true},
                    StallCase{"RedundantDeepRLazy", 2, false, {512, 2048}, 1, true},
                    StallCase{"RedundantDeepPLazy", 2, false, {2048, 512}, 1, true}),
    [](const testing::TestParamInfo<StallCase>& info) { return info.param.name; });

/* the redundant pkt shares the chain mbuf, so the chain pool needs the leg keeping more
 */
class St20TxPoolSizeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_txv_init(), 0);
    ctx_ = ut_txv_create();
    ASSERT_NE(ctx_, nullptr);
  }
  void TearDown() override {
    EXPECT_EQ(ut_txv_pipe_teardown(ctx_), 0);
    ut_txv_destroy(ctx_);
  }
  void Setup(uint16_t nb_p, uint16_t nb_r, uint32_t linesize_pad = 0) {
    ut_txv_pipe_case c = {};
    c.width = 1920;
    c.height = 1080;
    c.frames_cnt = 3;
    c.linesize_pad = linesize_pad;
    c.num_port = nb_r ? 2 : 1;
    c.nb_tx_desc[MTL_SESSION_PORT_P] = nb_p;
    c.nb_tx_desc[MTL_SESSION_PORT_R] = nb_r;
    c.free_thresh = 32;
    ASSERT_EQ(ut_txv_pipe_setup(ctx_, &c), 0);
  }
  ut_txv_ctx* ctx_ = nullptr;
};

TEST_F(St20TxPoolSizeTest, RedundantChainPoolCoversTheDeeperLeg) {
  for (auto nb : {std::make_pair(512, 2048), std::make_pair(2048, 512)}) {
    SCOPED_TRACE(nb.first);
    if (nb.first != 512) {
      EXPECT_EQ(ut_txv_pipe_teardown(ctx_), 0);
      ut_txv_destroy(ctx_);
      ctx_ = ut_txv_create();
    }
    Setup(nb.first, nb.second);
    EXPECT_EQ(ut_txv_pipe_pool_size(ctx_, UT_TXV_PIPE_CHAIN),
              std::max(ut_txv_pipe_pool_size(ctx_, MTL_SESSION_PORT_P),
                       ut_txv_pipe_pool_size(ctx_, MTL_SESSION_PORT_R)));
  }
}

/* the builder ring and bulk, the transmitter's bulk, 1024 chained pkts and the cache
 * slack: 1732 */
TEST_F(St20TxPoolSizeTest, A1080pSessionAt2048DescriptorsGets2047) {
  Setup(2048, 0);
  EXPECT_EQ(ut_txv_pipe_pool_size(ctx_, MTL_SESSION_PORT_P), 2047u);
  EXPECT_EQ(ut_txv_pipe_pool_size(ctx_, UT_TXV_PIPE_CHAIN), 2047u);
}

/* not deep, main's: nb_tx_desc and the builder ring, 1024 */
TEST_F(St20TxPoolSizeTest, A1080pSessionAt512DescriptorsGets2047) {
  Setup(512, 0);
  EXPECT_EQ(ut_txv_pipe_mempool_n(ctx_, MTL_SESSION_PORT_P), 1024u);
  EXPECT_EQ(ut_txv_pipe_pool_size(ctx_, MTL_SESSION_PORT_P), 2047u);
  EXPECT_EQ(ut_txv_pipe_pool_size(ctx_, UT_TXV_PIPE_CHAIN), 2047u);
}

/* a user ring of the deep depth is not deep: main's 2048 + the builder ring, 2560 */
TEST_F(St20TxPoolSizeTest, A1080pSessionOnAUserRingOf2048GetsMainsPools) {
  ut_txv_pipe_case c = {};
  c.width = 1920;
  c.height = 1080;
  c.frames_cnt = 3;
  c.num_port = 1;
  c.nb_tx_desc[MTL_SESSION_PORT_P] = 2048;
  c.free_thresh = 32;
  c.user_nb_tx_desc = true;
  ASSERT_EQ(ut_txv_pipe_setup(ctx_, &c), 0);
  EXPECT_EQ(ut_txv_pipe_mempool_n(ctx_, MTL_SESSION_PORT_P), 2048u + 512);
  EXPECT_EQ(ut_txv_pipe_pool_size(ctx_, MTL_SESSION_PORT_P), 4095u);
  EXPECT_EQ(ut_txv_pipe_pool_size(ctx_, UT_TXV_PIPE_CHAIN), 4095u);
}

/* main's chain pool is the primary leg's alone */
TEST_F(St20TxPoolSizeTest, ARedundantSessionOffDeepPortsGetsThePrimaryChainPool) {
  Setup(512, 1536);
  EXPECT_EQ(ut_txv_pipe_mempool_n(ctx_, MTL_SESSION_PORT_R), 1536u + 512);
  EXPECT_EQ(ut_txv_pipe_pool_size(ctx_, MTL_SESSION_PORT_R), 4095u);
  EXPECT_EQ(ut_txv_pipe_pool_size(ctx_, UT_TXV_PIPE_CHAIN), 2047u);
}

/* an RL transmitter holds a bulk of 4 more on a deep port; main's count has no bulks */
TEST_F(St20TxPoolSizeTest, AnRlLegHoldsATransmitterBulkMoreOnADeepPort) {
  Setup(2048, 512);
  EXPECT_EQ(ut_txv_pipe_rl_mempool_n(ctx_, MTL_SESSION_PORT_P),
            ut_txv_pipe_mempool_n(ctx_, MTL_SESSION_PORT_P) + 4);
  EXPECT_EQ(ut_txv_pipe_rl_mempool_n(ctx_, MTL_SESSION_PORT_R),
            ut_txv_pipe_mempool_n(ctx_, MTL_SESSION_PORT_R));
}

/* a migrated session leaves the cache of a second lcore: main's nb_tx_desc and builder
 * ring, 2560, where that is more than the deep count and a second slack */
TEST_F(St20TxPoolSizeTest, AMigratingSessionOnADeepPortGetsMainsCount) {
  ut_txv_set_tx_video_migrate(ctx_, true);
  Setup(2048, 0);
  EXPECT_EQ(ut_txv_pipe_mempool_n(ctx_, MTL_SESSION_PORT_P), 2048u + 512);
}

TEST_F(St20TxPoolSizeTest, AMigratingCopySessionOnADeepPortGetsASecondSlack) {
  for (bool migrate : {false, true}) {
    SCOPED_TRACE(migrate);
    if (migrate) {
      EXPECT_EQ(ut_txv_pipe_teardown(ctx_), 0);
      ut_txv_destroy(ctx_);
      ctx_ = ut_txv_create();
    }
    ut_txv_set_tx_video_migrate(ctx_, migrate);
    ut_txv_pipe_case c = {};
    c.width = 1920;
    c.height = 1080;
    c.frames_cnt = 3;
    c.num_port = 1;
    c.copy = true;
    c.nb_tx_desc[MTL_SESSION_PORT_P] = 2048;
    c.free_thresh = 32;
    ASSERT_EQ(ut_txv_pipe_setup(ctx_, &c), 0);
    /* the builder ring, the transmitter's bulk, 2048 pkts and the slack */
    EXPECT_EQ(ut_txv_pipe_mempool_n(ctx_, MTL_SESSION_PORT_P),
              512u + 4 + 2048 + kCacheSlack * (migrate ? 2 : 1));
  }
}

/* main's RTCP term: the buffer_size of the ops, still 0 when the app leaves it to the
 * library, then the ST_TX_VIDEO_RTCP_RING_SIZE tv_init_rtcp() writes back for recovery */
TEST_F(St20TxPoolSizeTest, ARtcpSessionAt512RecoversWithMainsPools) {
  unsigned int hdr = 0, recovery_hdr = 0;
  uint16_t ring = 0;
  ASSERT_EQ(0,
            ut_txv_rtcp_recovery_mempool_sizes(ctx_, 512, 0, &hdr, &ring, &recovery_hdr));
  EXPECT_EQ(hdr, 2047u);
  EXPECT_EQ(ring, 1024);
  EXPECT_EQ(recovery_hdr, 4095u);
}

/* a deep port asks mt_rtcp for the ring it keeps, at least nb_tx_desc, and counts it */
TEST_F(St20TxPoolSizeTest, ARtcpSessionOnADeepPortRecoversWithTheSamePools) {
  unsigned int hdr = 0, recovery_hdr = 0;
  uint16_t ring = 0;
  ASSERT_EQ(
      0, ut_txv_rtcp_recovery_mempool_sizes(ctx_, 2048, 0, &hdr, &ring, &recovery_hdr));
  EXPECT_EQ(hdr, 4095u);
  EXPECT_EQ(ring, 2048);
  EXPECT_EQ(recovery_hdr, 4095u);
}

/* and the 2048 pkts of the RTCP retransmit ring: 3780 */
TEST_F(St20TxPoolSizeTest, A1080pRtcpSessionAt2048DescriptorsGets4095) {
  unsigned int hdr = 0, chain = 0;
  ASSERT_EQ(0, ut_txv_rtcp_mempool_sizes(ctx_, 2048, 0, &hdr, &chain));
  EXPECT_EQ(hdr, 4095u);
  EXPECT_EQ(chain, 4095u);
}

/* a padded frame copies its line-crossing pkts from a pool of its own with its own
 * cache */
TEST_F(St20TxPoolSizeTest, APadded1080pSessionAt512DescriptorsGets511Copies) {
  Setup(512, 0, 64);
  EXPECT_EQ(ut_txv_pipe_pool_size(ctx_, UT_TXV_PIPE_COPY), 511u);
}

TEST_F(St20TxPoolSizeTest, APadded1080pSessionAt2048DescriptorsGets1023Copies) {
  Setup(2048, 0, 64);
  EXPECT_EQ(ut_txv_pipe_pool_size(ctx_, UT_TXV_PIPE_COPY), 1023u);
}

/* tv_build_st20_chain() copies a pkt that runs past a line end, but the frame's last: at
 * every chain pool size, the copy pool covers the most such pkts any run of the held pkts
 * has, across frame ends too */
TEST_F(St20TxPoolSizeTest, CopiesCoverEveryRunOfHeldPkts) {
  for (uint32_t width : {1920u, 1280u, 720u}) {
    SCOPED_TRACE(width);
    if (width != 1920u) {
      EXPECT_EQ(ut_txv_pipe_teardown(ctx_), 0);
      ut_txv_destroy(ctx_);
      ctx_ = ut_txv_create();
    }
    ut_txv_pipe_case c = {};
    c.width = width;
    c.height = width * 9 / 16;
    c.frames_cnt = 3;
    c.linesize_pad = 64;
    c.num_port = 1;
    c.nb_tx_desc[MTL_SESSION_PORT_P] = 512;
    c.free_thresh = 32;
    ASSERT_EQ(ut_txv_pipe_setup(ctx_, &c), 0);
    uint32_t pkt_len, line_bytes, frame_size;
    int total;
    ut_txv_pipe_geometry(ctx_, &pkt_len, &line_bytes, &frame_size, &total);
    const unsigned int kMaxHeld = 3000;
    std::vector<int> copies;
    while (copies.size() < kMaxHeld + (size_t)total) {
      for (int i = 0; i < total; i++) {
        uint64_t end = (uint64_t)i * pkt_len + pkt_len;
        uint64_t line_end = ((uint64_t)i * pkt_len / line_bytes + 1) * line_bytes;
        copies.push_back(end > line_end && end < frame_size);
      }
    }
    std::vector<int> sum(copies.size() + 1, 0);
    for (size_t i = 0; i < copies.size(); i++) sum[i + 1] = sum[i] + copies[i];
    for (unsigned int held = 1; held <= kMaxHeld; held++) {
      int most = 0;
      for (int start = 0; start < total; start++)
        most = std::max(most, sum[start + held] - sum[start]);
      ASSERT_GE(ut_txv_pipe_copy_mempool_n(ctx_, held + kCacheSlack), most + kCacheSlack)
          << held << " held";
    }
  }
}

}  // namespace
