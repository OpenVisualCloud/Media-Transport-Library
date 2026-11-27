/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#pragma once

#include <string>
#include <vector>

#include "tests.hpp"

namespace mtl_test {

/* Unified entry point for executing shared and/or own-context (NoCtxTest) suites. */
int run_unified_tests(struct st_tests_context* ctx,
                      const std::vector<std::string>& orig_argv);

}  // namespace mtl_test
