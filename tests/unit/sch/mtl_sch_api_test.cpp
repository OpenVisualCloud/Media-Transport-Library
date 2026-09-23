/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <gtest/gtest.h>
#include <mtl_api.h>
#include <pthread.h>

#include "sch/mt_sch_harness.h"

class MtlBindToLcoreTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_sch_init(), 0);
    mt_ = ut_sch_create_main();
    ASSERT_NE(mt_, nullptr);
  }

  void TearDown() override {
    ut_sch_destroy_main(mt_);
  }

  mtl_handle mt_ = nullptr;
};

/* mt_sch_lcore_valid() used to return -EIO from a bool, which reads as true. */
TEST_F(MtlBindToLcoreTest, RejectsALcoreIdPastTheEalMaximum) {
  EXPECT_LT(mtl_bind_to_lcore(mt_, pthread_self(), ut_sch_max_lcore()), 0);
}
