/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * A stalled TX queue must show as a refused burst before the audio session pools run
 * dry. The production session tasklet and, on the shared queue, the audio transmitter
 * run into a hung ring, from the start or after a lazily freeing ring kept a mbuf in
 * every slot, with pools of exactly what tx_audio_session_mempool_n() asks for. On a
 * deep port (2048 here): the transmit ring up to its threshold, the pkt the transmit
 * stage waits on, what each of its queues keeps (ceil(nb_tx_desc / descriptors a pkt),
 * RL adding 3 pads a pkt on 2 queues) and the cache slack another lcore keeps; the
 * redundant chain pool that of the leg that keeps more. One mbuf less and the alloc
 * fails first, but on RL, where the warm-up pads keep descriptors too. A port of 512 is
 * not deep and gets main's pools, nb_tx_desc and the 1024 of the manager ring.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='*St30TxPool*'
 */

#include <gtest/gtest.h>

#include <algorithm>

#include "common/ut_tx_ring.h"
#include "session/st30_tx_harness.h"

namespace {

constexpr unsigned int kCacheSlack = 192; /* 1.5 x MT_MBUF_CACHE_SIZE */
constexpr uint64_t kStepNs = 1000;
constexpr uint64_t kLazyNs =
    2500000000; /* 2500 pkts at 1 ms, every ring slot keeps one */
constexpr uint64_t kHungNs = 2500000000;

struct StallCase {
  const char* name;
  enum st30_ptime ptime;
  ut_txa30_pipe_pacing pacing;
  int num_port;
  bool copy;
  uint16_t nb_tx_desc;
  int hung_leg;
  bool lazy; /* drain on a ring that frees on reuse, then hang */
};

class St30TxPoolStallTest : public ::testing::TestWithParam<StallCase> {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_txa30_init(), 0);
    ctx_ = ut_txa30_create();
    ASSERT_NE(ctx_, nullptr);
  }
  void TearDown() override {
    EXPECT_EQ(ut_txa30_pipe_teardown(ctx_), 0);
    ut_txa30_destroy(ctx_);
  }
  ut_txa30_pipe_case Base() {
    const StallCase& sc = GetParam();
    ut_txa30_pipe_case c = {};
    c.ptime = sc.ptime;
    c.pacing = sc.pacing;
    c.num_port = sc.num_port;
    c.copy = sc.copy;
    c.ring_mode = sc.lazy ? UT_TXR_FREE_ON_REUSE : UT_TXR_FREE_ON_CLEANUP;
    c.free_thresh = 32;
    for (int i = 0; i < sc.num_port; i++) {
      c.nb_tx_desc[i] = sc.nb_tx_desc;
      c.hung[i] = !sc.lazy && i == sc.hung_leg;
    }
    return c;
  }
  /* the helper's pools, shrink_pool one mbuf short: a leg's header pool or
   * UT_TXA30_PIPE_CHAIN */
  void RunExact(int shrink_pool) {
    ut_txa30_pipe_case c = Base();
    ASSERT_EQ(ut_txa30_pipe_setup(ctx_, &c), 0);
    for (int i = 0; i < c.num_port; i++) {
      c.hdr_pool[i] = ut_txa30_pipe_mempool_n(ctx_, i);
      c.chain_pool = std::max(c.chain_pool, c.hdr_pool[i]);
    }
    ASSERT_EQ(ut_txa30_pipe_teardown(ctx_), 0);
    ut_txa30_destroy(ctx_);
    ctx_ = ut_txa30_create();
    ASSERT_NE(ctx_, nullptr);

    c.hold = kCacheSlack;
    if (shrink_pool == UT_TXA30_PIPE_CHAIN)
      c.chain_pool--;
    else if (shrink_pool >= 0)
      c.hdr_pool[shrink_pool]--;
    Run(c);
  }
  void Run(const ut_txa30_pipe_case& c) {
    ASSERT_EQ(ut_txa30_pipe_setup(ctx_, &c), 0);
    if (GetParam().lazy) {
      ut_txa30_pipe_run(ctx_, kLazyNs, kStepNs);
      ut_txa30_pipe_hang(ctx_, GetParam().hung_leg);
    }
    ut_txa30_pipe_run(ctx_, kHungNs, kStepNs);
  }
  /* the run stops at the first refused burst or failed alloc */
  void ExpectBurstFailFirst() {
    ASSERT_NE(ut_txa30_pipe_first_burst_fail(ctx_), 0u);
    EXPECT_EQ(ut_txa30_pipe_first_alloc_fail(ctx_), 0u)
        << "burst fail at step " << ut_txa30_pipe_first_burst_fail(ctx_);
  }
  void ExpectAllocFailFirst() {
    uint64_t burst_fail = ut_txa30_pipe_first_burst_fail(ctx_);
    uint64_t alloc_fail = ut_txa30_pipe_first_alloc_fail(ctx_);
    ASSERT_NE(alloc_fail, 0u);
    EXPECT_TRUE(!burst_fail || alloc_fail < burst_fail)
        << "alloc fail at step " << alloc_fail << ", burst fail at " << burst_fail;
  }
  ut_txa30_ctx* ctx_ = nullptr;
};

TEST_P(St30TxPoolStallTest, StallShowsAsARefusedBurstBeforeAnAllocFail) {
  RunExact(-1);
  ExpectBurstFailFirst();
}

TEST_P(St30TxPoolStallTest, TheProductionPoolsSeeTheRefusedBurstFirst) {
  Run(Base());
  ExpectBurstFailFirst();
}

INSTANTIATE_TEST_SUITE_P(
    Rings, St30TxPoolStallTest,
    testing::Values(StallCase{"Hung512", ST30_PTIME_1MS, UT_TXA30_PIPE_TSC_SHARED, 1,
                              false, 512, 0, false},
                    StallCase{"Hung2048", ST30_PTIME_1MS, UT_TXA30_PIPE_TSC_SHARED, 1,
                              false, 2048, 0, false},
                    StallCase{"Lazy512", ST30_PTIME_1MS, UT_TXA30_PIPE_TSC_SHARED, 1,
                              false, 512, 0, true},
                    StallCase{"Lazy2048", ST30_PTIME_1MS, UT_TXA30_PIPE_TSC_SHARED, 1,
                              false, 2048, 0, true},
                    StallCase{"DedicatedLazy2048", ST30_PTIME_1MS,
                              UT_TXA30_PIPE_TSC_DEDICATED, 1, false, 2048, 0, true},
                    StallCase{"P125Lazy2048", ST30_PTIME_125US, UT_TXA30_PIPE_TSC_SHARED,
                              1, false, 2048, 0, true},
                    StallCase{"CopyLazy2048", ST30_PTIME_1MS, UT_TXA30_PIPE_TSC_SHARED, 1,
                              true, 2048, 0, true},
                    StallCase{"RedundantPHung2048", ST30_PTIME_1MS,
                              UT_TXA30_PIPE_TSC_SHARED, 2, false, 2048, 0, false},
                    StallCase{"RedundantRHung2048", ST30_PTIME_1MS,
                              UT_TXA30_PIPE_TSC_SHARED, 2, false, 2048, 1, false},
                    StallCase{"RedundantRLazy2048", ST30_PTIME_1MS,
                              UT_TXA30_PIPE_TSC_SHARED, 2, false, 2048, 1, true},
                    StallCase{"RedundantCopyRLazy2048", ST30_PTIME_1MS,
                              UT_TXA30_PIPE_TSC_SHARED, 2, true, 2048, 1, true},
                    StallCase{"RlP125Hung2048", ST30_PTIME_125US, UT_TXA30_PIPE_RL, 1,
                              false, 2048, 0, false},
                    StallCase{"RlP125Lazy512", ST30_PTIME_125US, UT_TXA30_PIPE_RL, 1,
                              false, 512, 0, true},
                    StallCase{"RlP125Lazy2048", ST30_PTIME_125US, UT_TXA30_PIPE_RL, 1,
                              false, 2048, 0, true},
                    StallCase{"RlLazy2048", ST30_PTIME_1MS, UT_TXA30_PIPE_RL, 1, false,
                              2048, 0, true}),
    [](const testing::TestParamInfo<StallCase>& info) { return info.param.name; });

/* RL audio is left out: its warm-up pads keep descriptors too, so one mbuf less may still
 * see the refused burst first. Only a deep port's pools are that tight. */
using St30TxPoolBoundTest = St30TxPoolStallTest;

TEST_P(St30TxPoolBoundTest, OneHeaderMbufLessFailsTheAllocFirst) {
  RunExact(MTL_SESSION_PORT_P);
  ExpectAllocFailFirst();
}

TEST_P(St30TxPoolBoundTest, OneRedundantHeaderMbufLessFailsTheAllocFirst) {
  if (GetParam().num_port < 2) GTEST_SKIP() << "one leg";
  RunExact(MTL_SESSION_PORT_R);
  ExpectAllocFailFirst();
}

TEST_P(St30TxPoolBoundTest, OneChainMbufLessFailsTheAllocFirst) {
  if (GetParam().copy) GTEST_SKIP() << "no chain pool";
  RunExact(UT_TXA30_PIPE_CHAIN);
  ExpectAllocFailFirst();
}

INSTANTIATE_TEST_SUITE_P(
    LazyRings, St30TxPoolBoundTest,
    testing::Values(StallCase{"Lazy2048", ST30_PTIME_1MS, UT_TXA30_PIPE_TSC_SHARED, 1,
                              false, 2048, 0, true},
                    StallCase{"DedicatedLazy2048", ST30_PTIME_1MS,
                              UT_TXA30_PIPE_TSC_DEDICATED, 1, false, 2048, 0, true},
                    StallCase{"P125Lazy2048", ST30_PTIME_125US, UT_TXA30_PIPE_TSC_SHARED,
                              1, false, 2048, 0, true},
                    StallCase{"CopyLazy2048", ST30_PTIME_1MS, UT_TXA30_PIPE_TSC_SHARED, 1,
                              true, 2048, 0, true},
                    StallCase{"RedundantRLazy2048", ST30_PTIME_1MS,
                              UT_TXA30_PIPE_TSC_SHARED, 2, false, 2048, 1, true},
                    StallCase{"RedundantCopyRLazy2048", ST30_PTIME_1MS,
                              UT_TXA30_PIPE_TSC_SHARED, 2, true, 2048, 1, true}),
    [](const testing::TestParamInfo<StallCase>& info) { return info.param.name; });

/* the payload is already chained to the primary pkt when the redundant header alloc
 * fails */
TEST(St30TxRedundantAllocTest, AFailedRedundantHeaderFreesThePayloadOnce) {
  ASSERT_EQ(ut_txa30_init(), 0);
  ut_txa30_ctx* ctx = ut_txa30_create();
  ASSERT_NE(ctx, nullptr);
  ut_txa30_pipe_case c = {};
  c.ptime = ST30_PTIME_1MS;
  c.pacing = UT_TXA30_PIPE_TSC_DEDICATED;
  c.num_port = 2;
  c.nb_tx_desc[MTL_SESSION_PORT_P] = 512;
  c.nb_tx_desc[MTL_SESSION_PORT_R] = 512;
  c.ring_mode = UT_TXR_FREE_ON_CLEANUP;
  c.free_thresh = 32;
  c.hdr_pool[MTL_SESSION_PORT_P] = 64;
  c.hdr_pool[MTL_SESSION_PORT_R] = 1;
  c.chain_pool = 64;
  ASSERT_EQ(ut_txa30_pipe_setup(ctx, &c), 0);

  ut_txa30_pipe_run(ctx, 100000000, kStepNs);

  EXPECT_NE(ut_txa30_pipe_first_alloc_fail(ctx), 0u);
  EXPECT_EQ(ut_txa30_pipe_teardown(ctx), 0) << "a pool lost or doubled mbufs";
  ut_txa30_destroy(ctx);
}

/* the redundant pkt shares the chain mbuf, so the chain pool needs the leg keeping more
 */
TEST(St30TxPoolSizeTest, RedundantChainPoolCoversTheDeeperLeg) {
  ASSERT_EQ(ut_txa30_init(), 0);
  for (auto nb : {std::make_pair(512, 2048), std::make_pair(2048, 512)}) {
    SCOPED_TRACE(nb.first);
    ut_txa30_ctx* ctx = ut_txa30_create();
    ASSERT_NE(ctx, nullptr);
    ut_txa30_pipe_case c = {};
    c.ptime = ST30_PTIME_1MS;
    c.pacing = UT_TXA30_PIPE_TSC_SHARED;
    c.num_port = 2;
    c.nb_tx_desc[MTL_SESSION_PORT_P] = nb.first;
    c.nb_tx_desc[MTL_SESSION_PORT_R] = nb.second;
    c.free_thresh = 32;
    ASSERT_EQ(ut_txa30_pipe_setup(ctx, &c), 0);
    EXPECT_EQ(ut_txa30_pipe_pool_size(ctx, UT_TXA30_PIPE_CHAIN),
              std::max(ut_txa30_pipe_pool_size(ctx, MTL_SESSION_PORT_P),
                       ut_txa30_pipe_pool_size(ctx, MTL_SESSION_PORT_R)));
    EXPECT_EQ(ut_txa30_pipe_teardown(ctx), 0);
    ut_txa30_destroy(ctx);
  }
}

/* main's chain pool is the primary leg's alone: 1536 */
TEST(St30TxPoolSizeTest, ARedundantSessionOffDeepPortsGetsThePrimaryChainPool) {
  ASSERT_EQ(ut_txa30_init(), 0);
  ut_txa30_ctx* ctx = ut_txa30_create();
  ASSERT_NE(ctx, nullptr);
  ut_txa30_pipe_case c = {};
  c.ptime = ST30_PTIME_1MS;
  c.pacing = UT_TXA30_PIPE_TSC_SHARED;
  c.num_port = 2;
  c.nb_tx_desc[MTL_SESSION_PORT_P] = 512;
  c.nb_tx_desc[MTL_SESSION_PORT_R] = 1024;
  c.free_thresh = 32;
  ASSERT_EQ(ut_txa30_pipe_setup(ctx, &c), 0);
  EXPECT_EQ(ut_txa30_pipe_mempool_n(ctx, MTL_SESSION_PORT_R), 1024u + 1024);
  EXPECT_EQ(ut_txa30_pipe_pool_size(ctx, MTL_SESSION_PORT_R), 4095u);
  EXPECT_EQ(ut_txa30_pipe_pool_size(ctx, UT_TXA30_PIPE_CHAIN), 2047u);
  EXPECT_EQ(ut_txa30_pipe_teardown(ctx), 0);
  ut_txa30_destroy(ctx);
}

/* deep, the 10 pkts of the transmit ring, the waiting one, the chained pkts of the queue
 * and the cache slack: 1227 at 2048 descriptors; not deep, main's 512 + 1024 at 512 */
TEST(St30TxPoolSizeTest, A1msSessionGets2047At2048DescriptorsAnd2047At512) {
  ASSERT_EQ(ut_txa30_init(), 0);
  for (auto nb : {std::make_pair(2048, 2047u), std::make_pair(512, 2047u)}) {
    SCOPED_TRACE(nb.first);
    ut_txa30_ctx* ctx = ut_txa30_create();
    ASSERT_NE(ctx, nullptr);
    ut_txa30_pipe_case c = {};
    c.ptime = ST30_PTIME_1MS;
    c.pacing = UT_TXA30_PIPE_TSC_SHARED;
    c.num_port = 1;
    c.nb_tx_desc[MTL_SESSION_PORT_P] = nb.first;
    c.free_thresh = 32;
    ASSERT_EQ(ut_txa30_pipe_setup(ctx, &c), 0);
    EXPECT_EQ(ut_txa30_pipe_pool_size(ctx, MTL_SESSION_PORT_P), nb.second);
    EXPECT_EQ(ut_txa30_pipe_pool_size(ctx, UT_TXA30_PIPE_CHAIN), nb.second);
    EXPECT_EQ(ut_txa30_pipe_teardown(ctx), 0);
    ut_txa30_destroy(ctx);
  }
}

}  // namespace
