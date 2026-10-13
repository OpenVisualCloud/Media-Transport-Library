/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * ST40p TX framebuffer lifetime: every UDW buffer tx_st40p_init_fbs()
 * allocates must be released exactly once by tx_st40p_uinit_fbs(), on the
 * normal free path and after an init that failed part way.
 */

#include <gtest/gtest.h>
#include <rte_log.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "common/ut_common.h"
#include "pipeline/st40p_tx_harness.h"

TEST(St40PipelineTxFbs, UinitReleasesEveryUdwBuffer) {
  ASSERT_EQ(ut40p_tx_init(), 0) << "EAL init failed";
  const uint64_t before = ut_rte_alloc_count();

  ASSERT_EQ(ut40p_tx_fbs_init_uinit(3, 1024), 0);

  EXPECT_EQ(ut_rte_alloc_count(), before) << "UDW buffers leaked by uinit";
}

TEST(St40PipelineTxFbs, FailedInitIsReleasedOnce) {
  ASSERT_EQ(ut40p_tx_init(), 0) << "EAL init failed";
  const uint64_t before = ut_rte_alloc_count();

  /* rte_free() of memory that is not a live allocation logs this and returns. */
  char* log = nullptr;
  size_t log_len = 0;
  FILE* log_stream = open_memstream(&log, &log_len);
  ASSERT_NE(log_stream, nullptr);
  FILE* saved = rte_log_get_stream();
  rte_openlog_stream(log_stream);

  /* Larger than the --no-huge heap, so the first UDW allocation fails. */
  int ret = ut40p_tx_fbs_init_uinit(2, 1U << 31);

  rte_openlog_stream(saved);
  fclose(log_stream);
  std::string captured(log ? log : "");
  free(log);

  EXPECT_LT(ret, 0);
  EXPECT_EQ(captured.find("Invalid memory"), std::string::npos)
      << "the framebuffer array was freed twice: " << captured;
  EXPECT_EQ(ut_rte_alloc_count(), before);
}
