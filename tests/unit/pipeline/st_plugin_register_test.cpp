/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * st_plugin_register() keys a plugin by its whole path.
 */

#include <gtest/gtest.h>

#include <cerrno>
#include <climits>
#include <string>

#include "pipeline/st_plugin_harness.h"
#include "st_pipeline_api.h"

/* A path over 128 chars, and the longest path that fits */
TEST(StPluginRegister, LongPathRegistersOnceAndUnregisters) {
  for (size_t len : {size_t(200), size_t(PATH_MAX - 1)}) {
    mtl_handle mt = ut_plugin_dl_reset();
    ASSERT_NE(mt, nullptr);
    std::string path = "/" + std::string(len - 1, 'p');

    EXPECT_EQ(st_plugin_register(mt, path.c_str()), 0) << len;
    EXPECT_EQ(st_get_plugins_nb(mt), 1) << len;
    EXPECT_LT(st_plugin_register(mt, path.c_str()), 0) << len;
    EXPECT_EQ(ut_plugin_dlopen_calls(), 1) << len;
    EXPECT_EQ(st_plugin_unregister(mt, path.c_str()), 0) << len;
  }
}

TEST(StPluginRegister, PathOfPathMaxCharsFailsBeforeDlopen) {
  mtl_handle mt = ut_plugin_dl_reset();
  ASSERT_NE(mt, nullptr);
  std::string path = "/" + std::string(PATH_MAX - 1, 'p');

  EXPECT_EQ(st_plugin_register(mt, path.c_str()), -ENAMETOOLONG);
  EXPECT_EQ(ut_plugin_dlopen_calls(), 0);
  EXPECT_EQ(st_get_plugins_nb(mt), 0);
}
