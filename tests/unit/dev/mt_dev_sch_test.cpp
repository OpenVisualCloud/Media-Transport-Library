/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <gtest/gtest.h>

#include "dev/mt_dev_harness.h"

namespace {

/* the quota a 2160p119 YUV 4:2:2 10-bit TX video session asks for */
constexpr int kTx2160p119QuotaMbs = 20715;

/* An ancillary session takes zero quota, which fits the system scheduler too. */
TEST(MtDevSchTest, DedicatedSysLcoreStaysSystemAfterAnAncillarySession) {
  struct ut_dev_sch_setup setup = {};
  setup.flags = MTL_FLAG_DEDICATED_SYS_LCORE;
  setup.anc_session_first = true;
  setup.tx_video_quota_mbs = kTx2160p119QuotaMbs;
  struct ut_dev_sch_layout layout = {};
  ASSERT_EQ(ut_dev_sch_layout(&setup, &layout), 0);
  EXPECT_NE(layout.tx_video_sch, layout.main_sch);
}

struct ut_dev_sch_setup two_ports(enum mtl_rss_mode rss_mode, uint16_t rss_sch_nb) {
  struct ut_dev_sch_setup setup = {};
  setup.tx_video_quota_mbs = kTx2160p119QuotaMbs;
  setup.num_ports = 2;
  setup.rss_mode[MTL_PORT_P] = rss_mode;
  setup.rss_mode[MTL_PORT_R] = rss_mode;
  setup.rss_sch_nb = rss_sch_nb;
  return setup;
}

/* A shared-RSS tasklet polls every RX packet of its port, too much to share. */
TEST(MtDevSchTest, TxVideoSessionSkipsTheSharedRssSchedulerAfterAnAncillarySession) {
  struct ut_dev_sch_setup setup = two_ports(MTL_RSS_MODE_L3, 0);
  setup.anc_session_first = true;
  struct ut_dev_sch_layout layout = {};
  ASSERT_EQ(ut_dev_sch_layout(&setup, &layout), 0);
  ASSERT_EQ(layout.srss_tasklets, 2);
  EXPECT_EQ(layout.srss_on_tx_video_sch, 0);
}

TEST(MtDevSchTest, TxVideoSessionSkipsEveryRssDispatchScheduler) {
  struct ut_dev_sch_setup setup = two_ports(MTL_RSS_MODE_L3, 2);
  struct ut_dev_sch_layout layout = {};
  ASSERT_EQ(ut_dev_sch_layout(&setup, &layout), 0);
  ASSERT_EQ(layout.srss_tasklets, 4);
  EXPECT_EQ(layout.srss_on_tx_video_sch, 0);
}

TEST(MtDevSchTest, SharedRssOnTheSecondPortOnlyClosesTheMainScheduler) {
  struct ut_dev_sch_setup setup = two_ports(MTL_RSS_MODE_NONE, 0);
  setup.rss_mode[MTL_PORT_R] = MTL_RSS_MODE_L3;
  struct ut_dev_sch_layout layout = {};
  ASSERT_EQ(ut_dev_sch_layout(&setup, &layout), 0);
  ASSERT_EQ(layout.srss_tasklets, 1);
  EXPECT_NE(layout.tx_video_sch, layout.main_sch);
}

/* Without shared RSS the main scheduler takes media, so it costs no extra lcore. */
TEST(MtDevSchTest, WithoutSharedRssTxVideoSessionJoinsTheMainScheduler) {
  struct ut_dev_sch_setup setup = two_ports(MTL_RSS_MODE_NONE, 0);
  struct ut_dev_sch_layout layout = {};
  ASSERT_EQ(ut_dev_sch_layout(&setup, &layout), 0);
  EXPECT_EQ(layout.tx_video_sch, layout.main_sch);
}

}  // namespace
