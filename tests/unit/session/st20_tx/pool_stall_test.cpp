/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * A stalled TX queue must show as a refused burst before the session pools run dry:
 * with every mbuf in the ring and the pool empty nothing bursts, so nothing refuses and
 * the hang goes unseen. The production builder and TSC transmitter run into a hung ring,
 * from the start or after a lazily freeing ring kept a mbuf in every slot. A header pool
 * holds the builder ring, the builder's and transmitter's bulks, what the ring keeps
 * (ceil(nb_tx_desc / descriptors a pkt)) and the cache slack another lcore keeps; the
 * redundant chain pool that of the leg that keeps more. One mbuf less and the alloc
 * fails first.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='*St20TxPool*'
 */

#include <gtest/gtest.h>

#include <algorithm>

#include "common/ut_tx_ring.h"
#include "session/st20_tx_harness.h"

namespace {

constexpr unsigned int kRingCount = 512; /* ST_TX_VIDEO_SESSIONS_RING_SIZE */
constexpr unsigned int kBulk = 4;
constexpr unsigned int kCacheSlack = 192; /* 1.5 x MT_MBUF_CACHE_SIZE */
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
};

unsigned int HdrPool(uint16_t nb_tx_desc, bool copy) {
  unsigned int desc_per_pkt = copy ? 1 : 2;
  return kRingCount + kBulk + (nb_tx_desc + desc_per_pkt - 1) / desc_per_pkt +
         kCacheSlack;
}

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
  /* exact pools, shrink_pool one mbuf short: a leg's header pool or UT_TXV_PIPE_CHAIN */
  ut_txv_pipe_case Case(int shrink_pool) {
    const StallCase& sc = GetParam();
    ut_txv_pipe_case c = {};
    c.width = 1920;
    c.height = 1080;
    c.frames_cnt = 3;
    c.num_port = sc.num_port;
    c.copy = sc.copy;
    c.ring_mode = sc.lazy ? UT_TXR_FREE_ON_REUSE : UT_TXR_FREE_ON_CLEANUP;
    c.free_thresh = 32;
    c.hold = kCacheSlack;
    for (int i = 0; i < sc.num_port; i++) {
      c.nb_tx_desc[i] = sc.nb_tx_desc[i];
      c.hung[i] = !sc.lazy && i == sc.hung_leg;
      c.hdr_pool[i] = HdrPool(sc.nb_tx_desc[i], sc.copy);
      c.chain_pool = std::max(c.chain_pool, HdrPool(sc.nb_tx_desc[i], false));
    }
    if (shrink_pool == UT_TXV_PIPE_CHAIN)
      c.chain_pool--;
    else if (shrink_pool >= 0)
      c.hdr_pool[shrink_pool]--;
    return c;
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
  Run(Case(-1));
  ExpectBurstFailFirst();
}

TEST_P(St20TxPoolStallTest, TheProductionPoolsSeeTheRefusedBurstFirst) {
  ut_txv_pipe_case c = Case(-1);
  c.hdr_pool[MTL_SESSION_PORT_P] = 0;
  Run(c);
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
                    StallCase{"RedundantDeepPLazy", 2, false, {2048, 512}, 1, true}),
    [](const testing::TestParamInfo<StallCase>& info) { return info.param.name; });

/* a lazily freeing ring keeps a mbuf in every slot, the most it can; a hung one keeps a
 * descriptor less */
using St20TxPoolBoundTest = St20TxPoolStallTest;

TEST_P(St20TxPoolBoundTest, OneHeaderMbufLessFailsTheAllocFirst) {
  Run(Case(MTL_SESSION_PORT_P));
  ExpectAllocFailFirst();
}

TEST_P(St20TxPoolBoundTest, OneRedundantHeaderMbufLessFailsTheAllocFirst) {
  if (GetParam().num_port < 2) GTEST_SKIP() << "one leg";
  Run(Case(MTL_SESSION_PORT_R));
  ExpectAllocFailFirst();
}

TEST_P(St20TxPoolBoundTest, OneChainMbufLessFailsTheAllocFirst) {
  if (GetParam().copy) GTEST_SKIP() << "no chain pool";
  Run(Case(UT_TXV_PIPE_CHAIN));
  ExpectAllocFailFirst();
}

INSTANTIATE_TEST_SUITE_P(
    LazyRings, St20TxPoolBoundTest,
    testing::Values(StallCase{"Lazy512", 1, false, {512, 0}, 0, true},
                    StallCase{"Lazy2048", 1, false, {2048, 0}, 0, true},
                    StallCase{"CopyLazy2048", 1, true, {2048, 0}, 0, true},
                    StallCase{"RedundantRLazy2048", 2, false, {2048, 2048}, 1, true},
                    StallCase{"RedundantDeepRLazy", 2, false, {512, 2048}, 1, true},
                    StallCase{"RedundantDeepPLazy", 2, false, {2048, 512}, 1, true}),
    [](const testing::TestParamInfo<StallCase>& info) { return info.param.name; });

}  // namespace
