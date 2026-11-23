/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * A plugin .so stays loaded while a dev it registered is still in use or registered.
 */

#include <dlfcn.h>
#include <gtest/gtest.h>
#include <st_pipeline_api.h>

#include <cerrno>

#include "pipeline/st_plugin_harness.h"

static bool so_loaded(ut_plugin_so so) {
  void* handle = dlopen(ut_plugin_so_path(so), RTLD_LAZY | RTLD_NOLOAD);
  if (handle) dlclose(handle);
  return handle != nullptr;
}

class StPluginUnregister : public ::testing::Test {
 protected:
  void SetUp() override {
    mt = static_cast<mtl_handle>(ut_plugin_mgr_init());
  }

  mtl_handle mt;
};

TEST_F(StPluginUnregister, BusyDeviceKeepsThePluginLoaded) {
  const char* path = ut_plugin_so_path(UT_SO_PLAIN);
  ASSERT_EQ(st_plugin_register(mt, path), 0);
  void* session = ut_plugin_mgr_get_encoder();
  ASSERT_NE(session, nullptr);

  ASSERT_EQ(st_plugin_unregister(mt, path), -EBUSY);
  EXPECT_EQ(st_get_plugins_nb(mt), 1);
  EXPECT_EQ(ut_plugin_mgr_encoders(), 1);
  EXPECT_TRUE(so_loaded(UT_SO_PLAIN));

  ut_plugin_mgr_put_encoder(session);
  EXPECT_EQ(st_plugin_unregister(mt, path), 0);
  EXPECT_EQ(ut_plugin_mgr_encoders(), 0);
  EXPECT_FALSE(so_loaded(UT_SO_PLAIN));
}

TEST_F(StPluginUnregister, UnregisterDropsThePluginCount) {
  const char* path = ut_plugin_so_path(UT_SO_PLAIN);
  ASSERT_EQ(st_plugin_register(mt, path), 0);
  ASSERT_EQ(st_get_plugins_nb(mt), 1);

  EXPECT_EQ(st_plugin_unregister(mt, path), 0);
  EXPECT_EQ(st_get_plugins_nb(mt), 0);
}

TEST_F(StPluginUnregister, NoFreeSlotRegistersNoDevice) {
  ut_plugin_mgr_fill_slots(true);
  EXPECT_LT(st_plugin_register(mt, ut_plugin_so_path(UT_SO_PLAIN)), 0);
  EXPECT_EQ(ut_plugin_mgr_encoders(), 0);
  ut_plugin_mgr_fill_slots(false);
}

TEST_F(StPluginUnregister, UinitUnloadsAnIdlePlugin) {
  ASSERT_EQ(st_plugin_register(mt, ut_plugin_so_path(UT_SO_PLAIN)), 0);

  EXPECT_EQ(ut_plugin_mgr_uinit(), 0);
  EXPECT_EQ(ut_plugin_mgr_encoders(), 0);
  EXPECT_FALSE(so_loaded(UT_SO_PLAIN));
}

TEST_F(StPluginUnregister, DeviceLeftByThePluginKeepsItLoaded) {
  const char* path = ut_plugin_so_path(UT_SO_KEEP_DEV);
  ASSERT_EQ(st_plugin_register(mt, path), 0);

  ASSERT_LT(st_plugin_unregister(mt, path), 0);
  EXPECT_EQ(st_get_plugins_nb(mt), 0);
  EXPECT_TRUE(so_loaded(UT_SO_KEEP_DEV));
  EXPECT_EQ(ut_plugin_mgr_get_encoder(), nullptr);
}

TEST_F(StPluginUnregister, DeviceLeftByAFailedCreateIsNotGranted) {
  EXPECT_LT(st_plugin_register(mt, ut_plugin_so_path(UT_SO_FAIL_CREATE)), 0);
  EXPECT_EQ(st_get_plugins_nb(mt), 0);
  EXPECT_TRUE(so_loaded(UT_SO_FAIL_CREATE));
  EXPECT_EQ(ut_plugin_mgr_get_encoder(), nullptr);
}

TEST_F(StPluginUnregister, UinitKeepsABusyPluginAndItsDevice) {
  ASSERT_EQ(st_plugin_register(mt, ut_plugin_so_path(UT_SO_BUSY)), 0);
  void* session = ut_plugin_mgr_get_encoder();
  ASSERT_NE(session, nullptr);

  EXPECT_EQ(ut_plugin_mgr_uinit(), -EBUSY);
  EXPECT_EQ(ut_plugin_mgr_encoders(), 1);
  EXPECT_TRUE(so_loaded(UT_SO_BUSY));
}

TEST_F(StPluginUnregister, UinitKeepsADeviceInUseLeftByAFailedCreate) {
  EXPECT_LT(st_plugin_register(mt, ut_plugin_so_path(UT_SO_FAIL_CREATE_BUSY)), 0);
  ut_plugin_mgr_hold_encoder();

  EXPECT_EQ(ut_plugin_mgr_uinit(), -EBUSY);
  EXPECT_EQ(ut_plugin_mgr_encoders(), 1);
  EXPECT_TRUE(so_loaded(UT_SO_FAIL_CREATE_BUSY));
}
