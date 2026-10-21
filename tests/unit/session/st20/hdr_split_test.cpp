/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * RX hdr split: the NIC splits after UDP, so the payload mbuf of each packet holds
 * the RTP header and the pixels, in its own slot of the frame memory.
 */

#include <gtest/gtest.h>

#include <cerrno>

#include "session/st20/st20_rx_test_base.h"
#include "st20_api.h"

/* 2 frames of 32 pkts exceed the 4 descriptors, rx_free_thresh 0, the 32 mbufs of the
 * rx_stage and a burst of 2 */
class St20RxHdrSplitTest : public St20RxBaseTest {
 protected:
  static constexpr uint32_t kTs = 1000;
  static constexpr int kPkts = 32;

  int num_port() const override {
    return 1;
  }
  int pkts_per_frame() const override {
    return kPkts;
  }

  void SetUp() override {
    St20RxBaseTest::SetUp();
    ASSERT_EQ(ut20_ctx_hdr_split_init(ctx_, 4, 2), 0);
  }

  int Feed(int line) {
    return ut20_feed_hdr_split_pkt(ctx_, line, kTs, line, line == kPkts - 1);
  }
};

TEST_F(St20RxHdrSplitTest, ReadsTheRtpHeaderFromThePayloadSegment) {
  for (int i = 0; i < kPkts; i++) EXPECT_EQ(Feed(i), 0);
  EXPECT_EQ(received(), (uint64_t)kPkts);
  EXPECT_EQ(ut20_frames_received(ctx_), 1);
}

TEST_F(St20RxHdrSplitTest, FrameHoldsEachPacketInTheSlotOfItsLine) {
  for (int i = 0; i < kPkts; i++) Feed(i);
  ASSERT_EQ(ut20_frames_received(ctx_), 1);
  for (int i = 0; i < kPkts; i++) EXPECT_EQ(ut20_hdr_split_frame_row(ctx_, i), i);
}

TEST_F(St20RxHdrSplitTest, PacketInTheSlotOfAnotherLineIsDropped) {
  ASSERT_EQ(Feed(0), 0);
  ut20_hdr_split_skip_slot(ctx_);
  EXPECT_LT(Feed(1), 0);
  EXPECT_EQ(ut20_stat_wrong_payload_hdr_split(ctx_), 1u);
}

TEST_F(St20RxHdrSplitTest, MarkerInTheSlotOfAnotherLineIsCopiedToItsSlot) {
  for (int i = 0; i < kPkts - 1; i++) ASSERT_EQ(Feed(i), 0);
  ut20_hdr_split_skip_slot(ctx_);
  EXPECT_EQ(Feed(kPkts - 1), 0);
  EXPECT_EQ(ut20_stat_copy_hdr_split(ctx_), 1u);
  ASSERT_EQ(ut20_frames_received(ctx_), 1);
  EXPECT_EQ(ut20_hdr_split_frame_row(ctx_, kPkts - 1), kPkts - 1);
}

class St20RxHdrSplitInitTest : public St20RxBaseTest {
 protected:
  int num_port() const override {
    return 1;
  }
  int pkts_per_frame() const override {
    return 32;
  }
};

TEST_F(St20RxHdrSplitInitTest, FailsWhenTheSlotsDoNotExceedThePmdMbufsAndBurst) {
  /* 30 descriptors, 32 rx_stage mbufs and 2 burst mbufs take all 64 slots */
  EXPECT_LT(ut20_ctx_hdr_split_init(ctx_, 30, 2), 0);
}

TEST_F(St20RxHdrSplitInitTest, FailsForGpmSlLinesWithAShorterLastPacket) {
  EXPECT_LT(ut20_ctx_hdr_split_init_width(ctx_, 1280), 0);
  EXPECT_LT(ut20_ctx_hdr_split_init_width(ctx_, 3840), 0);
}

TEST_F(St20RxHdrSplitInitTest, FailsWithDataPathOnly) {
  ut20_ctx_set_data_path_only(ctx_);
  EXPECT_LT(ut20_ctx_hdr_split_init(ctx_, 4, 2), 0);
}

TEST_F(St20RxHdrSplitTest, FailedRxqPutKeepsTheSlots) {
  ut20_hdr_split_uinit_hw(ctx_, -EBUSY);
  EXPECT_TRUE(ut20_hdr_split_slots_kept(ctx_));
}

TEST_F(St20RxHdrSplitTest, UpdateSourceFailsWhenTheSlotsAreKept) {
  ut20_hdr_split_uinit_hw(ctx_, -EBUSY);
  /* the old queue keeps the slots, a new one must not write the same frames */
  EXPECT_EQ(ut20_hdr_split_update_src(ctx_), -EBUSY);
}

TEST_F(St20RxHdrSplitTest, UinitKeepsTheFramesWhenTheSlotsAreKept) {
  ut20_hdr_split_uinit_hw(ctx_, -EBUSY);
  EXPECT_EQ(ut20_hdr_split_uinit(ctx_), -EBUSY);
  EXPECT_TRUE(ut20_hdr_split_frames_allocated(ctx_));
}

TEST_F(St20RxHdrSplitInitTest, OpsCheckTakesHdrSplitAlone) {
  EXPECT_EQ(ut20_ops_check_hdr_split(0), 0);
}

TEST_F(St20RxHdrSplitInitTest, OpsCheckRejectsAutoDetect) {
  EXPECT_LT(ut20_ops_check_hdr_split(ST20_RX_FLAG_AUTO_DETECT), 0);
}

TEST_F(St20RxHdrSplitInitTest, OpsCheckRejectsTheTimingParserStat) {
  EXPECT_LT(ut20_ops_check_hdr_split(ST20_RX_FLAG_TIMING_PARSER_STAT), 0);
}

TEST_F(St20RxHdrSplitInitTest, OpsCheckRejectsTheTimingParserMeta) {
  EXPECT_LT(ut20_ops_check_hdr_split(ST20_RX_FLAG_TIMING_PARSER_META), 0);
}
