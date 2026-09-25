/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <gtest/gtest.h>
#include <mtl_api.h>
#include <mtl_sch_api.h>
#include <rte_ethdev.h>

#include "datapath/mt_srss_harness.h"

// Under MTL_FLAG_TASKLET_SLEEP a scheduler sleeps once every tasklet reports all done.
class MtSrssTaskletTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_srss_init(), 0);
  }

  void TearDown() override {
    ut_srss_destroy_ctx(ctx_);
  }

  ut_srss_ctx* ctx_ = nullptr;
};

TEST_F(MtSrssTaskletTest, ReportsPendingAfterReceivingPackets) {
  ctx_ = ut_srss_create_ctx(2, 4);
  ASSERT_NE(ctx_, nullptr);
  EXPECT_EQ(ut_srss_tasklet_handler(ctx_), MTL_TASKLET_HAS_PENDING);
}

TEST_F(MtSrssTaskletTest, ReportsAllDoneWhenNoQueueHadPackets) {
  ctx_ = ut_srss_create_ctx(2, 0);
  ASSERT_NE(ctx_, nullptr);
  EXPECT_EQ(ut_srss_tasklet_handler(ctx_), MTL_TASKLET_ALL_DONE);
}

class MtSrssInitTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_srss_init(), 0);
  }

  void TearDown() override {
    ut_srss_destroy_ctx(ctx_);
  }

  uint64_t RegisteredAdviceUs(uint32_t link_speed_mbps) {
    ut_srss_destroy_ctx(ctx_);
    ctx_ = ut_srss_init_port(link_speed_mbps);
    EXPECT_NE(ctx_, nullptr);
    return ctx_ ? ut_srss_registered_advice_sleep_us(ctx_) : 0;
  }

  ut_srss_ctx* ctx_ = nullptr;
};

TEST_F(MtSrssInitTest, AdviceAt25GFitsOneBurst) {
  const uint64_t burst_bits =
      static_cast<uint64_t>(ut_srss_burst_size()) * MTL_MTU_MAX_BYTES * 8;
  const uint64_t advice_us = RegisteredAdviceUs(RTE_ETH_SPEED_NUM_25G);

  EXPECT_GT(advice_us, 0u);
  EXPECT_LE(advice_us * RTE_ETH_SPEED_NUM_25G, burst_bits);
}

TEST_F(MtSrssInitTest, UnreportedSpeedAdvisesAs100G) {
  const uint64_t advice_100g_us = RegisteredAdviceUs(RTE_ETH_SPEED_NUM_100G);

  EXPECT_GT(advice_100g_us, 0u);
  EXPECT_EQ(RegisteredAdviceUs(RTE_ETH_SPEED_NUM_NONE), advice_100g_us);
  EXPECT_EQ(RegisteredAdviceUs(RTE_ETH_SPEED_NUM_UNKNOWN), advice_100g_us);
}

TEST_F(MtSrssInitTest, RequestsFullSchQuota) {
  ctx_ = ut_srss_init_port(RTE_ETH_SPEED_NUM_100G);
  ASSERT_NE(ctx_, nullptr);
  ASSERT_GT(ut_srss_main_sch_quota_limit_mbs(ctx_), 0);
  EXPECT_EQ(ut_srss_requested_quota_mbs(ctx_), ut_srss_main_sch_quota_limit_mbs(ctx_));
}
