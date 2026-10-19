/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * The sys TX pool of a port: the sys queue's ring and 1024 more, and with
 * MTL_FLAG_TX_MONO_POOL two rings of mbufs per TX queue, which the sessions draw on. A
 * deep port also frees the mbufs of each queue into its scheduler's cache, so it adds
 * one cache slack per scheduler.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='MtDevTxPoolTest*'
 */

#include <gtest/gtest.h>

#include "dev/mt_dev_harness.h"

namespace {

constexpr uint16_t kQueues = 17;
constexpr unsigned int kCacheSlack = 192; /* 1.5 x MT_MBUF_CACHE_SIZE */
constexpr unsigned int kSchedulers = 18;  /* MT_MAX_SCH_NUM */

class MtDevTxPoolTest : public testing::Test {
 protected:
  void SetUp() override {
    ctx_ = ut_dev_create_ctx();
    ASSERT_NE(ctx_, nullptr);
  }
  void TearDown() override {
    ut_dev_destroy_ctx(ctx_);
  }
  ut_dev_ctx* ctx_ = nullptr;
};

TEST_F(MtDevTxPoolTest, TheSysPoolTakesTheRingAnd1024) {
  EXPECT_EQ(ut_dev_tx_sys_pool_elements(ctx_, kQueues, 2048, false), 2048u + 1024);
  EXPECT_EQ(ut_dev_tx_sys_pool_elements(ctx_, kQueues, 512, false), 512u + 1024);
}

/* main's 512 + 1024 + 17 x 2 x 512 */
TEST_F(MtDevTxPoolTest, AMonoPoolOf17QueuesAt512DescriptorsGets32767) {
  unsigned int elements = ut_dev_tx_sys_pool_elements(ctx_, kQueues, 512, true);
  EXPECT_EQ(elements, 18944u);
  EXPECT_EQ(ut_dev_mempool_size(ctx_, elements), 32767u);
}

/* 2048 + 1024 + 17 x 2 x 2048 + 18 x 192 */
TEST_F(MtDevTxPoolTest, ADeepMonoPoolOf17QueuesGets131071) {
  unsigned int elements = ut_dev_tx_sys_pool_elements(ctx_, kQueues, 2048, true);
  EXPECT_EQ(elements, 76160u);
  EXPECT_EQ(ut_dev_mempool_size(ctx_, elements), 131071u);
}

/* each queue's frame video session keeps a descriptor's worth of header and chain mbufs,
 * and two of its builder ring and 3 RL bulks; every scheduler's cache keeps 192 more */
TEST_F(MtDevTxPoolTest, ADeepMonoPoolHoldsEachQueuesSessionAndEverySchedulersCache) {
  for (uint16_t q : {1, 4, 17}) {
    SCOPED_TRACE(q);
    unsigned int held =
        2048 + 1024 + q * (2048 + 2 * (512 + 3 * 4)) + kSchedulers * kCacheSlack;
    EXPECT_GE(ut_dev_tx_sys_pool_elements(ctx_, q, 2048, true), held);
  }
}

}  // namespace
