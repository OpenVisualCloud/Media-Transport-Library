/* SPDX-License-Identifier: LicenseRef-IPMX-TBD */

/*
 * A library built without enable_ipmx rejects the IPMX flag with -ENOTSUP,
 * even for ops that are IPMX compliant. tv_ops_check() / rv_ops_check() call
 * the same ipmx_*_ops_check() in both builds; this pins the stub version.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='IpmxDisabled*'
 */

#include <gtest/gtest.h>

#include <cerrno>

#include "ipmx/ipmx_stub_harness.h"
#include "ipmx/ipmx_test_ops.h"

TEST(IpmxDisabledTest, TxFlagRejectedWithEnotsup) {
  struct st20_tx_ops ops = ut_ipmx::TxOps();
  EXPECT_EQ(ut_ipmx_stub_tx_ops_check(&ops), -ENOTSUP);
}

TEST(IpmxDisabledTest, RxFlagRejectedWithEnotsup) {
  struct st20_rx_ops ops = ut_ipmx::RxOps();
  EXPECT_EQ(ut_ipmx_stub_rx_ops_check(&ops), -ENOTSUP);
}
