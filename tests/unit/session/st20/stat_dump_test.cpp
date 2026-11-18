/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <gtest/gtest.h>

#include <climits>
#include <cstring>

#include "session/st20/st20_rx_test_base.h"

class St20RxStatDumpTest : public St20RxBaseTest {};

/* The RX tasklet try_gets the same spinlock and skips the session while it is held. */
TEST_F(St20RxStatDumpTest, LogsWithoutHoldingSessionLock) {
  int locked_lines = -1;

  int lines = ut20_run_sessions_stat(ctx_, &locked_lines);

  EXPECT_GT(lines, 0);
  EXPECT_EQ(0, locked_lines);
}

TEST_F(St20RxStatDumpTest, ReportsPktDeltaAndAdvancesSnapshot) {
  int locked_lines = 0;
  ut20_set_stat_pkts_received(ctx_, 250);

  ASSERT_GT(ut20_run_sessions_stat(ctx_, &locked_lines), 0);

  EXPECT_NE(nullptr, std::strstr(ut20_stat_log(), " pkts 250\n"));
  EXPECT_EQ(250u, ut20_stat_snapshot_pkts_received(ctx_));
}

TEST_F(St20RxStatDumpTest, LogsTimingParserStatWithoutHoldingSessionLock) {
  int locked_lines = -1;
  ASSERT_EQ(0, ut20_ctx_enable_timing_parser(ctx_, false));
  ut20_ctx_enable_timing_parser_stat(ctx_);
  ut20_tp_stat_add_frame(ctx_, MTL_SESSION_PORT_P, 2, 7, 100, 200);

  ut20_run_sessions_stat(ctx_, &locked_lines);

  ASSERT_NE(nullptr, std::strstr(ut20_stat_log(), "rv_tp_stat(0,0), COMPLIANT "));
  EXPECT_NE(nullptr, std::strstr(ut20_stat_log(), "MIN 2 MAX 7!"));
  EXPECT_EQ(INT_MIN, ut20_tp_stat_vrx_max(ctx_, MTL_SESSION_PORT_P));
  EXPECT_EQ(0, locked_lines);
}
