/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <gtest/gtest.h>

#include <cstring>

#include "session/st20_tx_harness.h"

namespace {

class St20TxStatDumpTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(0, ut_txv_init());
    ctx_ = ut_txv_create();
    ASSERT_NE(nullptr, ctx_);
  }

  void TearDown() override {
    ut_txv_destroy(ctx_);
  }

  ut_txv_ctx* ctx_ = nullptr;
};

/* Tasklets try_get the same spinlock and skip the session while it is held. */
TEST_F(St20TxStatDumpTest, LogsWithoutHoldingSessionLock) {
  int locked_lines = -1;

  int lines = ut_txv_run_sessions_stat(ctx_, &locked_lines);

  EXPECT_GT(lines, 0);
  EXPECT_EQ(0, locked_lines);
}

TEST_F(St20TxStatDumpTest, ReportsFrameDeltaAndAdvancesSnapshot) {
  int locked_lines = 0;
  ut_txv_set_stat_port_frames(ctx_, 250);

  ASSERT_GT(ut_txv_run_sessions_stat(ctx_, &locked_lines), 0);

  EXPECT_NE(nullptr, std::strstr(ut_txv_stat_first_log_line(), "frames 250 "));
  EXPECT_EQ(250u, ut_txv_stat_snapshot_port_frames(ctx_));
}

}  // namespace
