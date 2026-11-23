/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * The admin thread runs its work after each alarm signal, also one that came while it
 * was not waiting: else the work, which arms the next alarm, never runs again.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='MtAdminWakeTest.*'
 */

#include <gtest/gtest.h>

#include "session/st20_tx_harness.h"

namespace {

TEST(MtAdminWakeTest, SignalBeforeTheWaitIsNotLost) {
  ASSERT_EQ(ut_txv_init(), 0);

  EXPECT_GT(ut_admin_polls_after_early_wake(1000), 0);
}

TEST(MtAdminWakeTest, UinitRetriesAlarmCancelWhileHandlerIsInProgress) {
  ASSERT_EQ(ut_txv_init(), 0);

  EXPECT_EQ(ut_admin_uinit_alarm_cancel_calls(2), 3);
}

}  // namespace
