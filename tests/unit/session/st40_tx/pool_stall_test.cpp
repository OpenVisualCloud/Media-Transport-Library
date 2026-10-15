/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * A stalled TX queue must show as a refused burst before the ancillary session pools run
 * dry. The production session tasklet and, on the shared queue, the ancillary transmitter
 * run into a hung ring, from the start or after a lazily freeing ring kept a mbuf in
 * every slot. A header pool holds the pkt the burst refused, what the ring keeps
 * (ceil(nb_tx_desc / descriptors a pkt)) and the cache slack another lcore keeps; the
 * redundant chain pool that of the leg that keeps more. One mbuf less and the alloc
 * fails first.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='*St40TxPool*'
 */

#include <gtest/gtest.h>

#include "common/ut_tx_ring.h"
#include "session/st40_tx_harness.h"

namespace {

constexpr unsigned int kCacheSlack = 192; /* 1.5 x MT_MBUF_CACHE_SIZE */
constexpr uint64_t kStepNs = 20000;
constexpr uint64_t kLazyNs =
    40ull * 1000000000; /* 2400 pkts, every ring slot keeps one */
constexpr uint64_t kHungNs = 40ull * 1000000000;

struct StallCase {
  const char* name;
  bool shared;
  int num_port;
  bool copy;
  uint16_t nb_tx_desc;
  int hung_leg;
  bool lazy; /* drain on a ring that frees on reuse, then hang */
};

unsigned int Pool(uint16_t nb_tx_desc, bool copy) {
  unsigned int desc_per_pkt = copy ? 1 : 2;
  return 1 + (nb_tx_desc + desc_per_pkt - 1) / desc_per_pkt + kCacheSlack;
}

class St40TxPoolStallTest : public ::testing::TestWithParam<StallCase> {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_txa_init(), 0);
    ctx_ = ut_txa_create();
    ASSERT_NE(ctx_, nullptr);
  }
  void TearDown() override {
    EXPECT_EQ(ut_txa_pipe_teardown(ctx_), 0);
    ut_txa_destroy(ctx_);
  }
  /* exact pools unless production, shrink_pool one mbuf short: a leg's header pool or
   * UT_TXA_PIPE_CHAIN */
  ut_txa_pipe_case Case(bool production, int shrink_pool) {
    const StallCase& sc = GetParam();
    ut_txa_pipe_case c = {};
    c.shared = sc.shared;
    c.num_port = sc.num_port;
    c.copy = sc.copy;
    c.ring_mode = sc.lazy ? UT_TXR_FREE_ON_REUSE : UT_TXR_FREE_ON_CLEANUP;
    c.free_thresh = 32;
    for (int i = 0; i < sc.num_port; i++) {
      c.nb_tx_desc[i] = sc.nb_tx_desc;
      c.hung[i] = !sc.lazy && i == sc.hung_leg;
    }
    if (production) return c;
    c.hold = kCacheSlack;
    for (int i = 0; i < sc.num_port; i++) c.hdr_pool[i] = Pool(sc.nb_tx_desc, sc.copy);
    c.chain_pool = Pool(sc.nb_tx_desc, false);
    if (shrink_pool == UT_TXA_PIPE_CHAIN)
      c.chain_pool--;
    else if (shrink_pool >= 0)
      c.hdr_pool[shrink_pool]--;
    return c;
  }
  void Run(const ut_txa_pipe_case& c) {
    ASSERT_EQ(ut_txa_pipe_setup(ctx_, &c), 0);
    if (GetParam().lazy) {
      ut_txa_pipe_run(ctx_, kLazyNs, kStepNs);
      ut_txa_pipe_hang(ctx_, GetParam().hung_leg);
    }
    ut_txa_pipe_run(ctx_, kHungNs, kStepNs);
  }
  /* the run stops at the first refused burst or failed alloc */
  void ExpectBurstFailFirst() {
    ASSERT_NE(ut_txa_pipe_first_burst_fail(ctx_), 0u);
    EXPECT_EQ(ut_txa_pipe_first_alloc_fail(ctx_), 0u)
        << "burst fail at step " << ut_txa_pipe_first_burst_fail(ctx_);
  }
  void ExpectAllocFailFirst() {
    uint64_t burst_fail = ut_txa_pipe_first_burst_fail(ctx_);
    uint64_t alloc_fail = ut_txa_pipe_first_alloc_fail(ctx_);
    ASSERT_NE(alloc_fail, 0u);
    EXPECT_TRUE(!burst_fail || alloc_fail < burst_fail)
        << "alloc fail at step " << alloc_fail << ", burst fail at " << burst_fail;
  }
  ut_txa_ctx* ctx_ = nullptr;
};

TEST_P(St40TxPoolStallTest, StallShowsAsARefusedBurstBeforeAnAllocFail) {
  Run(Case(false, -1));
  ExpectBurstFailFirst();
}

TEST_P(St40TxPoolStallTest, TheProductionPoolsSeeTheRefusedBurstFirst) {
  Run(Case(true, -1));
  ExpectBurstFailFirst();
}

INSTANTIATE_TEST_SUITE_P(
    Rings, St40TxPoolStallTest,
    testing::Values(StallCase{"Hung512", true, 1, false, 512, 0, false},
                    StallCase{"Hung2048", true, 1, false, 2048, 0, false},
                    StallCase{"Lazy512", true, 1, false, 512, 0, true},
                    StallCase{"Lazy2048", true, 1, false, 2048, 0, true},
                    StallCase{"DedicatedHung2048", false, 1, false, 2048, 0, false},
                    StallCase{"DedicatedLazy2048", false, 1, false, 2048, 0, true},
                    StallCase{"CopyLazy2048", true, 1, true, 2048, 0, true},
                    StallCase{"RedundantPHung2048", true, 2, false, 2048, 0, false},
                    StallCase{"RedundantRHung2048", true, 2, false, 2048, 1, false},
                    StallCase{"RedundantRLazy2048", true, 2, false, 2048, 1, true},
                    StallCase{"RedundantCopyRLazy2048", true, 2, true, 2048, 1, true}),
    [](const testing::TestParamInfo<StallCase>& info) { return info.param.name; });

/* a lazily freeing ring keeps a mbuf in every slot, the most it can; a hung one keeps a
 * descriptor less */
using St40TxPoolBoundTest = St40TxPoolStallTest;

TEST_P(St40TxPoolBoundTest, OneHeaderMbufLessFailsTheAllocFirst) {
  Run(Case(false, MTL_SESSION_PORT_P));
  ExpectAllocFailFirst();
}

TEST_P(St40TxPoolBoundTest, OneRedundantHeaderMbufLessFailsTheAllocFirst) {
  if (GetParam().num_port < 2) GTEST_SKIP() << "one leg";
  Run(Case(false, MTL_SESSION_PORT_R));
  ExpectAllocFailFirst();
}

TEST_P(St40TxPoolBoundTest, OneChainMbufLessFailsTheAllocFirst) {
  if (GetParam().copy) GTEST_SKIP() << "no chain pool";
  Run(Case(false, UT_TXA_PIPE_CHAIN));
  ExpectAllocFailFirst();
}

INSTANTIATE_TEST_SUITE_P(
    LazyRings, St40TxPoolBoundTest,
    testing::Values(StallCase{"Lazy512", true, 1, false, 512, 0, true},
                    StallCase{"Lazy2048", true, 1, false, 2048, 0, true},
                    StallCase{"DedicatedLazy2048", false, 1, false, 2048, 0, true},
                    StallCase{"CopyLazy2048", true, 1, true, 2048, 0, true},
                    StallCase{"RedundantRLazy2048", true, 2, false, 2048, 1, true},
                    StallCase{"RedundantCopyRLazy2048", true, 2, true, 2048, 1, true}),
    [](const testing::TestParamInfo<StallCase>& info) { return info.param.name; });

/* the payload is already chained to the primary pkt when the redundant header alloc
 * fails */
TEST(St40TxRedundantAllocTest, AFailedRedundantHeaderFreesThePayloadOnce) {
  ASSERT_EQ(ut_txa_init(), 0);
  ut_txa_ctx* ctx = ut_txa_create();
  ASSERT_NE(ctx, nullptr);
  ut_txa_pipe_case c = {};
  c.num_port = 2;
  c.nb_tx_desc[MTL_SESSION_PORT_P] = 512;
  c.nb_tx_desc[MTL_SESSION_PORT_R] = 512;
  c.ring_mode = UT_TXR_FREE_ON_CLEANUP;
  c.free_thresh = 32;
  c.hdr_pool[MTL_SESSION_PORT_P] = 64;
  c.hdr_pool[MTL_SESSION_PORT_R] = 1;
  c.chain_pool = 64;
  ASSERT_EQ(ut_txa_pipe_setup(ctx, &c), 0);

  ut_txa_pipe_run(ctx, 1000000000, kStepNs);

  EXPECT_NE(ut_txa_pipe_first_alloc_fail(ctx), 0u);
  EXPECT_EQ(ut_txa_pipe_teardown(ctx), 0) << "a pool lost or doubled mbufs";
  ut_txa_destroy(ctx);
}

}  // namespace
