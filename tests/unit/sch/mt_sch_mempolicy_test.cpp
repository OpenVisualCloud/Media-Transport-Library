/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <gtest/gtest.h>
#include <numaif.h>

#include <cerrno>
#include <cstring>
#include <thread>

#include "sch/mt_sch_harness.h"

class MtSchMempolicyTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_sch_init(), 0);
    int probe_errno = 0;
    std::thread probe([&] {
      if (set_mempolicy(MPOL_LOCAL, NULL, 0) < 0)
        probe_errno = errno;
      else if (get_mempolicy(&local_mode_, NULL, 0, NULL, 0) < 0)
        probe_errno = errno;
    });
    probe.join();
    if (probe_errno == EPERM || probe_errno == ENOSYS)
      GTEST_SKIP() << "set_mempolicy refused: " << strerror(probe_errno);
    ASSERT_EQ(probe_errno, 0);
    ASSERT_NE(local_mode_, MPOL_DEFAULT);
  }

  /* A fresh default-policy thread, so a policy leaked onto the gtest thread is moot. */
  template <typename Fn>
  void RunOnDefaultPolicyThread(Fn fn) {
    std::thread t([&] {
      ASSERT_EQ(set_mempolicy(MPOL_DEFAULT, NULL, 0), 0);
      fn();
    });
    t.join();
  }

  template <typename Fn>
  void RunOnNode0BindThread(Fn fn) {
    std::thread t([&] {
      unsigned long node0 = 1;
      if (set_mempolicy(MPOL_BIND, &node0, sizeof(node0) * 8) < 0)
        GTEST_SKIP() << "MPOL_BIND to node 0 refused: " << strerror(errno);
      fn();
    });
    t.join();
  }

  /* What get_mempolicy() reports for MPOL_LOCAL; older kernels say MPOL_PREFERRED. */
  int local_mode_ = MPOL_DEFAULT;
};

TEST_F(MtSchMempolicyTest, TaskletThreadSchedulerRunsWithLocalPolicy) {
  RunOnDefaultPolicyThread([&] {
    int mode = -1;
    ASSERT_EQ(ut_sch_thread_mode_mempolicy(&mode), 0);
    EXPECT_EQ(mode, local_mode_);
  });
}

TEST_F(MtSchMempolicyTest, LcoreSchedulerRunsWithLocalPolicy) {
  RunOnDefaultPolicyThread([&] {
    int mode = -1, exit_mode = -1;
    ASSERT_EQ(ut_sch_lcore_entry_mempolicy(&mode, &exit_mode), 0);
    EXPECT_EQ(mode, local_mode_);
  });
}

TEST_F(MtSchMempolicyTest, LcoreSchedulerRestoresDefaultPolicyOnExit) {
  RunOnDefaultPolicyThread([&] {
    int mode = -1, exit_mode = -1;
    ASSERT_EQ(ut_sch_lcore_entry_mempolicy(&mode, &exit_mode), 0);
    EXPECT_EQ(exit_mode, MPOL_DEFAULT);
  });
}

TEST_F(MtSchMempolicyTest, TaskletThreadSchedulerKeepsInheritedBindPolicy) {
  RunOnNode0BindThread([&] {
    int mode = -1;
    ASSERT_EQ(ut_sch_thread_mode_mempolicy(&mode), 0);
    EXPECT_EQ(mode, MPOL_BIND);
  });
}

TEST_F(MtSchMempolicyTest, LcoreSchedulerKeepsBindPolicyOnExit) {
  RunOnNode0BindThread([&] {
    int mode = -1, exit_mode = -1;
    ASSERT_EQ(ut_sch_lcore_entry_mempolicy(&mode, &exit_mode), 0);
    EXPECT_EQ(exit_mode, MPOL_BIND);
  });
}
