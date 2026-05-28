/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Shared stderr-capture helper for backpressure / warn-line unit tests
 * across ST20, ST30, ST40 stats_test.cpp.
 */

#pragma once

#include <gtest/gtest.h>

#include <cstdio>
#include <functional>
#include <string>

namespace ut_session {

/* Run `body` with stderr captured; re-emit so the test log stays readable. */
inline std::string capture_stderr(const std::function<void()>& body) {
  testing::internal::CaptureStderr();
  body();
  fflush(stderr);
  std::string out = testing::internal::GetCapturedStderr();
  fprintf(stderr, "%s", out.c_str());
  return out;
}

}  // namespace ut_session
