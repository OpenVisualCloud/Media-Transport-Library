/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <gtest/gtest.h>

#include <cerrno>
#include <vector>

#include "dev/mt_dev_harness.h"

class MtDevHdrSplitTest : public testing::Test {
 protected:
  void SetUp() override {
    ctx_ = ut_dev_create_ctx();
    ASSERT_NE(ctx_, nullptr);
  }

  void TearDown() override {
    ut_dev_destroy_ctx(ctx_);
  }

  void ExpectEvents(std::vector<ut_dev_event> expected) {
    ASSERT_EQ(ut_dev_event_count(ctx_), (int)expected.size());
    for (size_t i = 0; i < expected.size(); i++)
      EXPECT_EQ(ut_dev_event_at(ctx_, i), expected[i]) << "event " << i;
  }

  ut_dev_ctx* ctx_ = nullptr;
};

TEST_F(MtDevHdrSplitTest, GetsAPayloadPoolQueueOnAPortThatCanSplit) {
  EXPECT_TRUE(ut_dev_get_hdr_split_rx_queue(ctx_, true, true, true));
}

TEST_F(MtDevHdrSplitTest, GetFailsOnAPortWithoutTheFeature) {
  EXPECT_FALSE(ut_dev_get_hdr_split_rx_queue(ctx_, false, false, true));
}

TEST_F(MtDevHdrSplitTest, GetFailsWithoutTheFeatureEvenIfAQueueHasAPayloadPool) {
  EXPECT_FALSE(ut_dev_get_hdr_split_rx_queue(ctx_, false, true, true));
}

TEST_F(MtDevHdrSplitTest, GetBindsTheSessionBetweenAStopAndAStart) {
  ASSERT_TRUE(ut_dev_get_hdr_split_rx_queue(ctx_, true, true, false));
  ExpectEvents({UT_DEV_EVENT_RX_QUEUE_STOP, UT_DEV_EVENT_HDR_SPLIT_BIND,
                UT_DEV_EVENT_RX_QUEUE_START});
}

TEST_F(MtDevHdrSplitTest, GetBindsTheSessionBeforeARuntimeQueueStarts) {
  ASSERT_TRUE(ut_dev_get_hdr_split_rx_queue(ctx_, true, true, true));
  ExpectEvents({UT_DEV_EVENT_HDR_SPLIT_BIND, UT_DEV_EVENT_RX_QUEUE_START});
}

TEST_F(MtDevHdrSplitTest, GetFailsAndStartsTheQueueAgainWhenTheBindFails) {
  ut_dev_fail_hdr_split_bind(ctx_, -ENOSPC);
  EXPECT_FALSE(ut_dev_get_hdr_split_rx_queue(ctx_, true, true, false));
  ExpectEvents({UT_DEV_EVENT_RX_QUEUE_STOP, UT_DEV_EVENT_HDR_SPLIT_BIND,
                UT_DEV_EVENT_RX_QUEUE_START});
}

TEST_F(MtDevHdrSplitTest, PutUnbindsTheSessionBetweenAStopAndAStart) {
  ASSERT_TRUE(ut_dev_get_hdr_split_rx_queue(ctx_, true, true, false));
  ASSERT_EQ(ut_dev_put_rx_queue(ctx_), 0);
  ExpectEvents({UT_DEV_EVENT_RX_QUEUE_STOP, UT_DEV_EVENT_HDR_SPLIT_BIND,
                UT_DEV_EVENT_RX_QUEUE_START, UT_DEV_EVENT_RX_QUEUE_STOP,
                UT_DEV_EVENT_HDR_SPLIT_UNBIND, UT_DEV_EVENT_RX_QUEUE_START});
}

TEST_F(MtDevHdrSplitTest, PutLeavesARuntimeQueueStoppedAfterTheUnbind) {
  ASSERT_TRUE(ut_dev_get_hdr_split_rx_queue(ctx_, true, true, true));
  ASSERT_EQ(ut_dev_put_rx_queue(ctx_), 0);
  ExpectEvents({UT_DEV_EVENT_HDR_SPLIT_BIND, UT_DEV_EVENT_RX_QUEUE_START,
                UT_DEV_EVENT_RX_QUEUE_STOP, UT_DEV_EVENT_HDR_SPLIT_UNBIND});
}

TEST_F(MtDevHdrSplitTest, GetUnbindsWhenTheStartAfterTheBindFails) {
  ut_dev_fail_rx_queue_start(ctx_, 1);
  EXPECT_FALSE(ut_dev_get_hdr_split_rx_queue(ctx_, true, true, false));
  ExpectEvents({UT_DEV_EVENT_RX_QUEUE_STOP, UT_DEV_EVENT_HDR_SPLIT_BIND,
                UT_DEV_EVENT_RX_QUEUE_START, UT_DEV_EVENT_HDR_SPLIT_UNBIND,
                UT_DEV_EVENT_RX_QUEUE_START});
}

TEST_F(MtDevHdrSplitTest, PutFailsAndKeepsTheQueueStoppedWhenTheUnbindFails) {
  ASSERT_TRUE(ut_dev_get_hdr_split_rx_queue(ctx_, true, true, false));
  ut_dev_fail_hdr_split_unbind(ctx_, -EBUSY);
  EXPECT_LT(ut_dev_put_rx_queue(ctx_), 0);
  ExpectEvents({UT_DEV_EVENT_RX_QUEUE_STOP, UT_DEV_EVENT_HDR_SPLIT_BIND,
                UT_DEV_EVENT_RX_QUEUE_START, UT_DEV_EVENT_RX_QUEUE_STOP,
                UT_DEV_EVENT_HDR_SPLIT_UNBIND});
}

TEST_F(MtDevHdrSplitTest, CheckKeepsHdrSplitWithIovaVaAndTheBulkAllocRxPath) {
  EXPECT_EQ(ut_dev_hdr_split_check(ctx_, true, 2048, 32, 0), 0);
  EXPECT_TRUE(ut_dev_has_hdr_split_queues(ctx_));
}

TEST_F(MtDevHdrSplitTest, CheckDropsHdrSplitWithIovaPa) {
  EXPECT_EQ(ut_dev_hdr_split_check(ctx_, false, 2048, 32, 0), 0);
  EXPECT_FALSE(ut_dev_has_hdr_split_queues(ctx_));
}

TEST_F(MtDevHdrSplitTest, CheckDropsHdrSplitWhenTheRingIsNoMultipleOfRxFreeThresh) {
  EXPECT_EQ(ut_dev_hdr_split_check(ctx_, true, 2080, 64, 0), 0);
  EXPECT_FALSE(ut_dev_has_hdr_split_queues(ctx_));
}

TEST_F(MtDevHdrSplitTest, CheckFailsWhenHdrSplitSlotsAreBelowThePmdMbufs) {
  /* 2048 descriptors, 32 rx_free_thresh and 32 rx_stage mbufs */
  EXPECT_LT(ut_dev_hdr_split_check(ctx_, true, 2048, 32, 2111), 0);
  EXPECT_EQ(ut_dev_hdr_split_check(ctx_, true, 2048, 32, 2112), 0);
}

TEST_F(MtDevHdrSplitTest, RateLimitChangeFailsWhileAHdrSplitPoolIsBound) {
  /* an ice commit stops and starts the port outside the RX session lock */
  EXPECT_EQ(ut_dev_set_rl_rate_with_bound_hdr_split(ctx_, 1000000), -EBUSY);
  EXPECT_EQ(ut_dev_tm_commit_count(ctx_), 0);
}

TEST_F(MtDevHdrSplitTest, RlTxQueueGetFailsAndKeepsRlPacingWhileAPoolIsBound) {
  ut_dev_set_pacing_port(ctx_, false, ST21_TX_PACING_WAY_RL);
  EXPECT_FALSE(ut_dev_get_rl_tx_queue_with_bound_hdr_split(ctx_, 1000000));
  EXPECT_EQ(ut_dev_pacing_way(ctx_), ST21_TX_PACING_WAY_RL);
}

TEST_F(MtDevHdrSplitTest, SetTxBpsReturnsTheErrorWhileAPoolIsBound) {
  ut_dev_set_pacing_port(ctx_, false, ST21_TX_PACING_WAY_RL);
  EXPECT_EQ(ut_dev_set_tx_bps_with_bound_hdr_split(ctx_, 1000000), -EBUSY);
}

TEST_F(MtDevHdrSplitTest, KeepsHdrSplitWhenAFrameOfTheMtuFitsTheRxBuffer) {
  /* 1664 - 128 = 1536 holds 1500 + 26 */
  EXPECT_TRUE(ut_dev_hdr_split_nic_buf_ok(ctx_, 1664, 1500));
  EXPECT_TRUE(ut_dev_has_hdr_split_queues(ctx_));
}

TEST_F(MtDevHdrSplitTest, DropsHdrSplitWhenTheRxBufferIsBelowTheMtu) {
  /* 1536 - 128 = 1408, ice would take the scattered Rx path */
  EXPECT_FALSE(ut_dev_hdr_split_nic_buf_ok(ctx_, 1536, 1500));
  EXPECT_FALSE(ut_dev_has_hdr_split_queues(ctx_));
}

TEST_F(MtDevHdrSplitTest, DropsHdrSplitWhenTheMtuExceedsTheRxBuffer) {
  EXPECT_FALSE(ut_dev_hdr_split_nic_buf_ok(ctx_, 1664, 9000));
  EXPECT_FALSE(ut_dev_has_hdr_split_queues(ctx_));
}

TEST_F(MtDevHdrSplitTest, PortStartSetsASplitQueueUpWithTheInitPayloadPool) {
  ASSERT_EQ(ut_dev_start_hdr_split_port(ctx_), 0);
  EXPECT_EQ(ut_dev_event_at(ctx_, 0), UT_DEV_EVENT_RX_QUEUE_SETUP);
  EXPECT_EQ(ut_dev_last_rx_nseg(ctx_), 2);
  EXPECT_EQ(ut_dev_last_rx_payload_pool(ctx_), UT_DEV_POOL_INIT_PAYLOAD);
}
