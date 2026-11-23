/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * ST 2110-22 interlaced RX field budget (rv_attach + rv_handle_st22_pkt).
 *
 * An ST 2110-22 framebuff_max_size is already one field's codestream budget,
 * so an interlaced session must accept a field up to that size, not half of it,
 * just as a progressive session accepts a frame up to it.
 */

#include <gtest/gtest.h>

#include "session/st20/st20_rx_test_base.h"

static constexpr size_t kFieldBudget = 4096;
static constexpr uint16_t kPayload = 1000;

class St22RxFieldBudgetProgressiveTest : public St20RxBaseTest {
 protected:
  int num_port() const override {
    return 1;
  }
  virtual bool interlaced() const {
    return false;
  }

  void SetUp() override {
    St20RxBaseTest::SetUp();
    ASSERT_EQ(ut20_ctx_attach_st22(ctx_, interlaced(), kFieldBudget), 0);
  }

  /* Feed `pkts` pkts of kPayload bytes as one field, the marker on the last. */
  void feed_field(int pkts, uint32_t ts) {
    uint8_t payload[kPayload] = {0};
    for (int i = 0; i < pkts; i++)
      ut20_feed_st22_pkt(ctx_, 100 + i, ts, i, i == pkts - 1, payload, sizeof(payload),
                         MTL_SESSION_PORT_P);
  }
};

class St22RxFieldBudgetInterlacedTest : public St22RxFieldBudgetProgressiveTest {
 protected:
  bool interlaced() const override {
    return true;
  }
};

TEST_F(St22RxFieldBudgetProgressiveTest, FrameWithinBudgetAssembles) {
  feed_field(3, 1000);

  EXPECT_EQ(offset_dropped(), 0u);
  EXPECT_EQ(received(), 3u);
  EXPECT_EQ(ut20_st22_frames_ready(ctx_), 1u);
  EXPECT_EQ(ut20_st22_last_frame_size(ctx_), 3000u);
}

TEST_F(St22RxFieldBudgetInterlacedTest, FieldLargerThanHalfBudgetAssembles) {
  feed_field(3, 1000); /* 3000 bytes: above 4096 / 2, within 4096 */

  EXPECT_EQ(offset_dropped(), 0u);
  EXPECT_EQ(received(), 3u);
  EXPECT_EQ(ut20_st22_frames_ready(ctx_), 1u);
  EXPECT_EQ(ut20_st22_last_frame_size(ctx_), 3000u);
}

TEST_F(St22RxFieldBudgetInterlacedTest, FieldLargerThanBudgetDropped) {
  feed_field(5, 1000); /* 5000 bytes: the 5th pkt ends past 4096 */

  EXPECT_EQ(offset_dropped(), 1u);
  EXPECT_EQ(received(), 4u);
  EXPECT_EQ(ut20_st22_frames_ready(ctx_), 0u);
}
