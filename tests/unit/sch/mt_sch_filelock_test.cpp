/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <gtest/gtest.h>

#include <ios>

#include "sch/mt_sch_harness.h"

/* The lock file is only opened O_RDONLY for flock(), so it is created 0444. The helper
 * creates it under umask 0, so the mode seen is the one requested. */
TEST(MtSchFilelock, CreatesLockFileReadOnly) {
  unsigned int mode = 0;
  ASSERT_EQ(ut_sch_filelock_create_mode(&mode), 0);
  EXPECT_EQ(mode, 0444u) << std::oct << std::showbase << "created with mode " << mode;
}
