/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

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

/* a session migrated to another scheduler: the stats API waits for the lock its tasklets
 * take there */
TEST_F(St20TxStatDumpTest, StatsAfterAMigrateTakeTheNewSchedulersLock) {
  ut_txv_run_migrate(ctx_);
  ut_txv_lock_linked_session(ctx_, true);
  std::atomic<bool> read{false};
  std::thread reader([&] {
    struct st20_tx_user_stats stats;
    ut_txv_get_session_stats(ctx_, &stats);
    read = true;
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  EXPECT_FALSE(read) << "read the stats while the new scheduler held the session";
  ut_txv_lock_linked_session(ctx_, false);
  reader.join();
  EXPECT_TRUE(read);
}

/* a migrate between the stats API's read of the session's slot and its lock: it ends on
 * the new scheduler's lock and leaves neither slot locked */
TEST_F(St20TxStatDumpTest, StatsReadAcrossAMigrateTakeTheNewSchedulersLock) {
  struct st20_tx_user_stats stats;

  ut_txv_migrate_at_next_lock(ctx_);
  ASSERT_EQ(0, ut_txv_get_session_stats(ctx_, &stats));
  EXPECT_TRUE(ut_txv_last_lock_linked(ctx_));
  EXPECT_FALSE(ut_txv_session_slots_locked(ctx_));
}

TEST_F(St20TxStatDumpTest, StatsResetAcrossAMigrateTakesTheNewSchedulersLock) {
  ut_txv_migrate_at_next_lock(ctx_);
  ASSERT_EQ(0, ut_txv_reset_session_stats(ctx_));
  EXPECT_TRUE(ut_txv_last_lock_linked(ctx_));
  EXPECT_FALSE(ut_txv_session_slots_locked(ctx_));
}

/* The old scheduler's transmitter released the session in its calls, which the new
 * one's count from 1 may reach: a migrated session starts released in none of them. */
TEST_F(St20TxStatDumpTest, AMigratedSessionWasReleasedInNoCallOfTheNewScheduler) {
  ut_txv_set_trs_released(ctx_, 0, 1);
  ut_txv_run_migrate(ctx_);
  EXPECT_EQ(0u, ut_txv_trs_released(ctx_, 0));
}

}  // namespace
