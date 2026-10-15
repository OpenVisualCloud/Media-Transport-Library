/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * A test that pins a known bug left for a follow-up: skipped, unless UT_RUN_KNOWN_BUGS
 * is set in the environment, which runs it to show it still fails.
 */

#ifndef _UT_KNOWN_BUG_H_
#define _UT_KNOWN_BUG_H_

#include <gtest/gtest.h>

#include <cstdlib>

#define UT_KNOWN_BUG(reason)                              \
  do {                                                    \
    if (!std::getenv("UT_RUN_KNOWN_BUGS"))                \
      GTEST_SKIP() << "known bug, follow-up: " << reason; \
  } while (0)

#endif /* _UT_KNOWN_BUG_H_ */
