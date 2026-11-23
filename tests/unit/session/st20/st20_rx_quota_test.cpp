/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * How many 1080p59 ST20 RX frame or slice sessions without a DMA dev the default
 * scheduler quota holds: the quota over what each charges. A frame session packs denser
 * when each of its ports fits its RX ring to the L3 and has a ring of at most 1024, for
 * ST 2022-7 the deeper of its two.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='St20RxQuotaTest*'
 */

#include <gtest/gtest.h>

#include "session/st20_harness.h"

namespace {

struct Port {
  uint16_t nb_rx_desc;
  bool l3_fit = true; /* ice/iavf with dedicated rx queues and cpus */
};

/* a 2022-7 session charges a 1080p59 bandwidth on each of its two ports */
int PerSch(enum st20_type type, Port p, Port r = Port{0, false}) {
  int num_port = r.nb_rx_desc ? 2 : 1;
  int bandwidth = static_cast<int>(st20_1080p59_yuv422_10bit_bandwidth_mps());
  int sch_quota = ut20_quota_tx1080p_per_sch() * bandwidth; /* dev_sch_init() */
  return sch_quota / ut20_rx_quota_mbs_wo_dma(type, p.nb_rx_desc, p.l3_fit, r.nb_rx_desc,
                                              r.l3_fit, num_port * bandwidth);
}

TEST(St20RxQuotaTest, FrameSessionsOnA1024RingPack11) {
  EXPECT_EQ(PerSch(ST20_TYPE_FRAME_LEVEL, Port{1024}), 11);
}

TEST(St20RxQuotaTest, FrameSessionsOnA512RingPack11) {
  EXPECT_EQ(PerSch(ST20_TYPE_FRAME_LEVEL, Port{512}), 11);
}

TEST(St20RxQuotaTest, FrameSessionsOnA1025RingPack8) {
  EXPECT_EQ(PerSch(ST20_TYPE_FRAME_LEVEL, Port{1025}), 8);
}

/* 144 x 1080p59.94 at 11 a core collapsed on 14 cores with 2048 rings */
TEST(St20RxQuotaTest, FrameSessionsOnA2048RingPack8) {
  EXPECT_EQ(PerSch(ST20_TYPE_FRAME_LEVEL, Port{2048}), 8);
}

/* iavf with HW RX timestamps raises a 1024 ring to 1056 */
TEST(St20RxQuotaTest, FrameSessionsOnAnIavfTimestampRingPack8) {
  EXPECT_EQ(PerSch(ST20_TYPE_FRAME_LEVEL, Port{1056}), 8);
}

/* other drivers, MTL_FLAG_TASKLET_THREAD, a shared RX queue or RSS: see
 * MtDevRxDescDefaultL3Test for which ports fit the L3 */
TEST(St20RxQuotaTest, FrameSessionsOnAUser512RingOutsideTheL3FitPack8) {
  EXPECT_EQ(PerSch(ST20_TYPE_FRAME_LEVEL, Port{512, false}), 8);
}

TEST(St20RxQuotaTest, RedundantFrameSessionsOnTwoShallowRingsPack5) {
  EXPECT_EQ(PerSch(ST20_TYPE_FRAME_LEVEL, Port{512}, Port{1024}), 5);
}

TEST(St20RxQuotaTest, RedundantFrameSessionsWithADeepRingPack4) {
  EXPECT_EQ(PerSch(ST20_TYPE_FRAME_LEVEL, Port{1024}, Port{2048}), 4);
  EXPECT_EQ(PerSch(ST20_TYPE_FRAME_LEVEL, Port{2048}, Port{512}), 4);
}

TEST(St20RxQuotaTest, RedundantFrameSessionsWithAPortOutsideTheL3FitPack4) {
  EXPECT_EQ(PerSch(ST20_TYPE_FRAME_LEVEL, Port{512}, Port{512, false}), 4);
}

TEST(St20RxQuotaTest, SliceSessionsPack8OnAnyRing) {
  EXPECT_EQ(PerSch(ST20_TYPE_SLICE_LEVEL, Port{512}), 8);
  EXPECT_EQ(PerSch(ST20_TYPE_SLICE_LEVEL, Port{2048}), 8);
}

}  // namespace
