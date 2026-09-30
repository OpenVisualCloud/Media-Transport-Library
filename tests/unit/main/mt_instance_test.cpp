/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <gtest/gtest.h>
#include <signal.h>

#include "main/mt_instance_harness.h"

namespace {

volatile sig_atomic_t g_sigpipe_cnt;

void count_sigpipe(int) {
  g_sigpipe_cnt = g_sigpipe_cnt + 1;
}

}  // namespace

/* SIGPIPE is caught rather than left at its default, which would end the process. */
class MtInstanceManagerGoneTest : public ::testing::Test {
 protected:
  void SetUp() override {
    struct sigaction sa = {};
    sa.sa_handler = count_sigpipe;
    sigemptyset(&sa.sa_mask);
    ASSERT_EQ(sigaction(SIGPIPE, &sa, &old_sa_), 0);
    g_sigpipe_cnt = 0;
    ctx_ = ut_instance_create_orphan();
    ASSERT_NE(ctx_, nullptr);
  }

  void TearDown() override {
    ut_instance_destroy(ctx_);
    sigaction(SIGPIPE, &old_sa_, nullptr);
  }

  ut_instance_ctx* ctx_ = nullptr;
  struct sigaction old_sa_ = {};
};

TEST_F(MtInstanceManagerGoneTest, PutLcoreFailsWithoutSigpipe) {
  EXPECT_LT(ut_instance_put_lcore(ctx_, 1), 0);
  EXPECT_EQ(g_sigpipe_cnt, 0);
}

TEST_F(MtInstanceManagerGoneTest, RequestXsksMapFdFailsWithoutSigpipe) {
  EXPECT_LT(ut_instance_request_xsks_map_fd(ctx_, 1), 0);
  EXPECT_EQ(g_sigpipe_cnt, 0);
}
