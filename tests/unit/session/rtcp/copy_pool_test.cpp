/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * The RTCP retransmit copy pool against its TX queue: a copy is one descriptor, so the
 * queue keeps at most nb_tx_desc of them, the retransmit loop copies 32 more before it
 * bursts them, and another lcore's cache keeps 192. A NACK of the whole retransmit ring
 * gets the same retransmits from that pool as from one of a retransmit ring more.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='*RtcpCopyPool*'
 */

#include <gtest/gtest.h>

#include <vector>

#include "common/ut_tx_ring.h"
#include "session/rtcp_attack_harness.h"

namespace {

constexpr uint16_t kNbTxDesc = 2048;
constexpr unsigned int kCacheSlack = 192;    /* 1.5 x MT_MBUF_CACHE_SIZE */
constexpr unsigned int kRetransmitBulk = 32; /* MT_RTCP_RETRANSMIT_BULK */

/* deep: what the queue keeps, a retransmit bulk and the cache slack */
TEST(RtcpCopyPoolTest, ACopyPoolAt2048DescriptorsGets4095) {
  ASSERT_EQ(ut_rtk_init(), 0);
  EXPECT_EQ(ut_rtk_tx_pool_n(kNbTxDesc, kNbTxDesc),
            kNbTxDesc + kRetransmitBulk + kCacheSlack);
  EXPECT_EQ(ut_rtk_tx_pool_size(kNbTxDesc, kNbTxDesc), 4095u);
}

/* not deep, main's: the retransmit ring and nb_tx_desc */
TEST(RtcpCopyPoolTest, ACopyPoolAt512DescriptorsGets2047) {
  ASSERT_EQ(ut_rtk_init(), 0);
  EXPECT_EQ(ut_rtk_tx_pool_n(512, 1024), 1024u + 512);
  EXPECT_EQ(ut_rtk_tx_pool_size(512, 1024), 2047u);
}

struct ParityCase {
  const char* name;
  int ring_mode;
  bool hung;
};

class RtcpCopyPoolParityTest : public ::testing::TestWithParam<ParityCase> {
 protected:
  static void SetUpTestSuite() {
    ASSERT_EQ(ut_rtk_init(), 0);
  }
  /* nacks FCIs of the whole ring in one NACK */
  ut_rtk_stats NackTheRing(unsigned int pool_n, unsigned int hold, uint32_t nacks = 1) {
    ut_rtk_stats s = {};
    ut_rtk_ctx* ctx = ut_rtk_create(kNbTxDesc, 1, pool_n);
    EXPECT_NE(ctx, nullptr);
    if (!ctx) return s;
    EXPECT_EQ(ut_rtk_set_queue(ctx, GetParam().ring_mode, kNbTxDesc, GetParam().hung), 0);
    EXPECT_EQ(ut_rtk_hold_copies(ctx, hold), 0);
    EXPECT_EQ(ut_rtk_buffer(ctx, 0, kNbTxDesc), 0);
    std::vector<ut_rtk_fci> f(nacks, ut_rtk_fci{0, kNbTxDesc - 1});
    s = ut_rtk_nack(ctx, (uint16_t)(2 + nacks), f.data(), nacks, 12 + 4 * nacks);
    ut_rtk_destroy(ctx);
    return s;
  }
};

TEST_P(RtcpCopyPoolParityTest, AWholeRingNackRetransmitsAsWithARingMore) {
  ut_rtk_stats big = NackTheRing(kNbTxDesc + kNbTxDesc, 0);
  ut_rtk_stats capped = NackTheRing(ut_rtk_tx_pool_n(kNbTxDesc, kNbTxDesc), kCacheSlack);

  EXPECT_GT(big.succ, 0u);
  EXPECT_EQ(capped.succ, big.succ);
  EXPECT_EQ(capped.fail_burst, big.fail_burst);
  EXPECT_EQ(capped.fail_nobuf, 0u);
  EXPECT_EQ(big.fail_nobuf, 0u);
}

/* this PMD frees an rs batch of 32 before the loop copies its next bulk, so the bulk is
 * spare here: the queue and the slack are the least, one copy less runs dry */
TEST_P(RtcpCopyPoolParityTest, OneCopyLessThanTheQueueAndSlackRunsDry) {
  unsigned int least = ut_rtk_tx_pool_n(kNbTxDesc, kNbTxDesc) - kRetransmitBulk;

  for (uint32_t nacks : {1u, 2u}) {
    SCOPED_TRACE(nacks);
    EXPECT_EQ(NackTheRing(least, kCacheSlack, nacks).fail_nobuf, 0u);
    EXPECT_GT(NackTheRing(least - 1, kCacheSlack, nacks).fail_nobuf, 0u);
  }
}

INSTANTIATE_TEST_SUITE_P(
    Queues, RtcpCopyPoolParityTest,
    testing::Values(ParityCase{"Hung", UT_TXR_FREE_ON_CLEANUP, true},
                    ParityCase{"FreesOnReuse", UT_TXR_FREE_ON_REUSE, false}),
    [](const testing::TestParamInfo<ParityCase>& info) { return info.param.name; });

}  // namespace
