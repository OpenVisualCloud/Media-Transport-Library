/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#pragma once

#include <string>
#include <vector>

#include "tests.hpp"

namespace mtl_test {

struct RunPlan {
  std::vector<std::string> shared_cases;
  std::vector<std::string> own_cases;
  bool sharded = false;
  bool invalid = false;
};

/* Returns true if the test suite owns its own mtl_init()/mtl_uninit() lifecycle. */
bool is_own_ctx_suite(const std::string& suite_name);

/* Evaluates GTest glob filter syntax (positive ':' patterns, optional '-' exclusions). */
bool gtest_filter_matches(const std::string& filter, const std::string& full_name);

/* Builds the execution plan for the current GTest filter and shard environment. */
RunPlan build_run_plan();

/* Prints JSON metadata for every registered test case (--list-json). */
void print_cases_json();

/* Unified entry point for executing the planned shared and/or own-context phases. */
int run_unified_tests(struct st_tests_context* ctx,
                      const std::vector<std::string>& orig_argv);

}  // namespace mtl_test
