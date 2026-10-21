/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <gtest/gtest.h>

#include "session/st20_hdr_split_pool_harness.h"

/* A session of 3 frames of 4 slots and 3 spare slots: 15 slots, on a pool of 20 mbufs. */
class St20RxHdrSplitPoolTest : public testing::Test {
 protected:
  static constexpr uint32_t kMbufsPerFrame = 4;
  static constexpr uint32_t kFrames = 3;
  static constexpr uint32_t kSlotSize = 1220;
  static constexpr uint32_t kExtraMbufs = 5;

  void SetUp() override {
    ctx_ = ut_hs_create(kMbufsPerFrame, kFrames, kSlotSize, kExtraMbufs);
    ASSERT_NE(ctx_, nullptr);
  }

  void TearDown() override {
    ut_hs_destroy(ctx_);
  }

  /* Gets and frees n mbufs, as a stream the handler took no frame start from. */
  void Cycle(int n) {
    for (int i = 0; i < n; i++) ut_hs_put(ctx_, ut_hs_get(ctx_));
  }

  ut_hs_ctx* ctx_ = nullptr;
};

TEST_F(St20RxHdrSplitPoolTest, StrideIsTheHeadroomAndSlotRoundedToACacheLine) {
  EXPECT_EQ(ut_hs_stride(ctx_), 1408u);
}

TEST_F(St20RxHdrSplitPoolTest, HandsOutTheSlotsInFrameOrder) {
  for (int i = 0; i < (int)(kMbufsPerFrame * kFrames); i++) EXPECT_EQ(ut_hs_get(ctx_), i);
}

TEST_F(St20RxHdrSplitPoolTest, DataOfASlotStartsAfterTheHeadroomOfItsStride) {
  Cycle(2);
  int idx = ut_hs_get(ctx_);
  ASSERT_EQ(idx, 2);
  EXPECT_EQ(ut_hs_data_offset(ctx_, idx), 2 * 1408 + 128);
  EXPECT_EQ(ut_hs_data_iova_offset(ctx_, idx), 2 * 1408 + 128);
}

TEST_F(St20RxHdrSplitPoolTest, WrapsToSlotZeroAtTheFirstFrameStartThatDoesNotFit) {
  Cycle(kMbufsPerFrame * kFrames);
  EXPECT_EQ(ut_hs_get(ctx_), 0);
}

TEST_F(St20RxHdrSplitPoolTest, WrapFollowsTheGridOfAnUnalignedFrameStart) {
  Cycle(2);
  ut_hs_set_frame_start(ctx_, 2);
  /* frames at 2, 6 and 10; one at 14 would need slots up to 17 */
  Cycle(12);
  EXPECT_EQ(ut_hs_get(ctx_), 0);
}

TEST_F(St20RxHdrSplitPoolTest, HandsOutNoMbufPastTheSlotsOfTheSession) {
  ut_hs_set_frame_start(ctx_, 3);
  /* frames at 3, 7 and 11 fill slots up to 14, the last slot of the session */
  Cycle(15);
  EXPECT_EQ(ut_hs_get(ctx_), 0);
}

TEST_F(St20RxHdrSplitPoolTest, SlotStillInUseFailsTheAlloc) {
  int first = ut_hs_get(ctx_);
  ASSERT_EQ(first, 0);
  Cycle(kMbufsPerFrame * kFrames - 1);
  EXPECT_EQ(ut_hs_get(ctx_), -1);
}

TEST_F(St20RxHdrSplitPoolTest, FailedAllocHandsOutTheSameSlotOnceItIsFree) {
  int first = ut_hs_get(ctx_);
  Cycle(kMbufsPerFrame * kFrames - 1);
  ASSERT_EQ(ut_hs_get(ctx_), -1);
  ut_hs_put(ctx_, first);
  EXPECT_EQ(ut_hs_get(ctx_), 0);
}

TEST_F(St20RxHdrSplitPoolTest, FailedBulkAllocKeepsEveryMbufAndTheNextSlot) {
  ASSERT_EQ(ut_hs_get(ctx_), 0);
  Cycle(10);
  /* slot 11, then the wrap to slot 0, which is still in use */
  EXPECT_EQ(ut_hs_get_bulk(ctx_, 4), -1);
  EXPECT_EQ(ut_hs_avail(ctx_), ut_hs_slots(ctx_) + kExtraMbufs - 1);
  EXPECT_EQ(ut_hs_get(ctx_), 11);
}

TEST_F(St20RxHdrSplitPoolTest, AvailCountIsTheMbufsNotInUse) {
  ut_hs_get(ctx_);
  ut_hs_get(ctx_);
  EXPECT_EQ(ut_hs_avail(ctx_), ut_hs_slots(ctx_) + kExtraMbufs - 2);
}

TEST_F(St20RxHdrSplitPoolTest, BindFailsWhenTheSessionHasMoreSlotsThanThePoolMbufs) {
  EXPECT_LT(ut_hs_bind(ctx_, kExtraMbufs + 1), 0);
}

TEST_F(St20RxHdrSplitPoolTest, BindFailsWhileAMbufIsInUse) {
  ut_hs_get(ctx_);
  EXPECT_LT(ut_hs_bind(ctx_, 0), 0);
}

TEST_F(St20RxHdrSplitPoolTest, BindStartsAgainAtSlotZero) {
  Cycle(5);
  ASSERT_EQ(ut_hs_bind(ctx_, 0), 0);
  EXPECT_EQ(ut_hs_get(ctx_), 0);
}

TEST_F(St20RxHdrSplitPoolTest, UnbindPointsTheMbufsAtTheScratchBuffers) {
  ASSERT_EQ(ut_hs_unbind(ctx_), 0);
  int idx = ut_hs_get(ctx_);
  ASSERT_GE(idx, 0);
  EXPECT_TRUE(ut_hs_in_scratch(ctx_, idx));
}

TEST_F(St20RxHdrSplitPoolTest, UnboundPoolHandsOutEveryMbuf) {
  ASSERT_EQ(ut_hs_unbind(ctx_), 0);
  Cycle(ut_hs_slots(ctx_));
  EXPECT_EQ(ut_hs_get(ctx_), (int)ut_hs_slots(ctx_));
}
