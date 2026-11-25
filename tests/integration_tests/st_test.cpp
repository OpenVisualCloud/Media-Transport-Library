/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include <vector>

#include "log.h"
#include "test_harness.hpp"

int st_test_sch_cnt(struct st_tests_context* ctx) {
  struct mtl_var_info var;
  int ret = mtl_get_var_info(ctx->handle, &var);
  if (ret < 0) return ret;
  return var.sch_cnt;
}

bool st_test_dma_available(struct st_tests_context* ctx) {
  if (ctx->iova == MTL_IOVA_MODE_PA) {
    info("%s, DMA not full supported under IOVA PA mode\n", __func__);
    return false;
  }

  struct mtl_var_info var;
  if (mtl_get_var_info(ctx->handle, &var) < 0) return false;

  struct mtl_fix_info fix;
  if (mtl_get_fix_info(ctx->handle, &fix) < 0) return false;

  return var.dma_dev_cnt < fix.dma_dev_cnt_max;
}

namespace {

/* Move-only RAII guard for an lcore acquired via mtl_get_lcore(). */
class ScopedLcore {
 public:
  ScopedLcore(mtl_handle h, unsigned int lcore)
      : handle_(h), lcore_(lcore), owned_(true) {
  }
  ~ScopedLcore() {
    if (owned_) {
      EXPECT_GE(release(), 0);
    }
  }
  ScopedLcore(const ScopedLcore&) = delete;
  ScopedLcore& operator=(const ScopedLcore&) = delete;
  ScopedLcore(ScopedLcore&& other) noexcept
      : handle_(other.handle_), lcore_(other.lcore_), owned_(other.owned_) {
    other.owned_ = false;
  }
  ScopedLcore& operator=(ScopedLcore&& other) noexcept {
    if (this != &other) {
      if (owned_) {
        EXPECT_GE(release(), 0);
      }
      handle_ = other.handle_;
      lcore_ = other.lcore_;
      owned_ = other.owned_;
      other.owned_ = false;
    }
    return *this;
  }

  int release() {
    if (!owned_) return 0;
    owned_ = false;
    return mtl_put_lcore(handle_, lcore_);
  }

 private:
  mtl_handle handle_ = nullptr;
  unsigned int lcore_ = 0;
  bool owned_ = false;
};

int test_lcore_cnt(struct st_tests_context* ctx) {
  struct mtl_var_info var;
  int ret = mtl_get_var_info(ctx->handle, &var);
  if (ret < 0) return ret;
  return var.lcore_cnt;
}

bool test_dev_started(struct st_tests_context* ctx) {
  struct mtl_var_info var;
  if (mtl_get_var_info(ctx->handle, &var) < 0) return false;
  return var.dev_started;
}

void start_stop_test(int repeat) {
  mtl_handle handle = st_test_ctx()->handle;
  mtl_test::ScopedMtlStart guard(handle);
  for (int i = 0; i < repeat; i++) {
    EXPECT_GE(guard.start(), 0);
    EXPECT_GE(guard.stop(), 0);
  }
}

void test_lcore_one(struct st_tests_context* ctx) {
  mtl_handle handle = ctx->handle;
  const int base_cnt = test_lcore_cnt(ctx);
  unsigned int lcore = 0;

  ASSERT_GE(mtl_get_lcore(handle, &lcore), 0);
  ScopedLcore guard(handle, lcore);
  EXPECT_EQ(test_lcore_cnt(ctx), base_cnt + 1);
  EXPECT_GE(guard.release(), 0);
  EXPECT_EQ(test_lcore_cnt(ctx), base_cnt);
}

}  // namespace

TEST(Main, init_expect_fail) {
  auto* ctx = st_test_ctx();
  struct mtl_init_params para = {};

  /* Empty params must fail. */
  EXPECT_EQ(mtl_init(&para), nullptr);

  /* Missing port name and IP must fail. */
  para.num_ports = 1;
  EXPECT_EQ(mtl_init(&para), nullptr);

  memcpy(mtl_p_sip_addr(&para), ctx->para.sip_addr[MTL_PORT_P], MTL_IP_ADDR_LEN);
  EXPECT_EQ(mtl_init(&para), nullptr);

  /* Invalid PCI BDF must fail. */
  snprintf(para.port[MTL_PORT_P], sizeof(para.port[MTL_PORT_P]), "0000:55:00.0");
  EXPECT_EQ(mtl_init(&para), nullptr);

  memcpy(mtl_r_sip_addr(&para), ctx->para.sip_addr[MTL_PORT_R], MTL_IP_ADDR_LEN);

  /* Invalid num_ports values must fail. */
  for (int invalid_ports : {0, 100, -1}) {
    para.num_ports = invalid_ports;
    EXPECT_EQ(mtl_init(&para), nullptr);
  }
}

TEST(Main, re_init_fail) {
  auto* ctx = st_test_ctx();
  EXPECT_EQ(mtl_init(&ctx->para), nullptr);
}

TEST(Main, start_stop_single) {
  start_stop_test(1);
}

TEST(Main, start_stop_multi) {
  start_stop_test(5);
}

TEST(Main, start_expect_fail) {
  mtl_handle handle = st_test_ctx()->handle;
  mtl_test::ScopedMtlStart guard(handle);
  EXPECT_GE(guard.start(), 0);
  EXPECT_GE(mtl_start(handle), 0);
  EXPECT_GE(guard.stop(), 0);
}

TEST(Main, stop_expect_fail) {
  mtl_handle handle = st_test_ctx()->handle;
  mtl_test::ScopedMtlStart guard(handle);
  EXPECT_GE(mtl_stop(handle), 0);
  EXPECT_GE(guard.start(), 0);
  EXPECT_GE(guard.stop(), 0);
  EXPECT_GE(mtl_stop(handle), 0);
}

TEST(Main, get_fix) {
  struct mtl_fix_info fix = {};
  EXPECT_GE(mtl_get_fix_info(st_test_ctx()->handle, &fix), 0);
  info("dma dev count %u\n", fix.dma_dev_cnt_max);
  info("init_flags 0x%" PRIx64 "\n", fix.init_flags);
}

TEST(Main, get_var) {
  struct mtl_var_info var = {};
  EXPECT_GE(mtl_get_var_info(st_test_ctx()->handle, &var), 0);
}

TEST(Main, lcore) {
  test_lcore_one(st_test_ctx());
}

TEST(Main, lcore_max) {
  auto* ctx = st_test_ctx();
  mtl_handle handle = ctx->handle;
  const int base_cnt = test_lcore_cnt(ctx);
  constexpr int kMaxLcores = 100;

  std::vector<ScopedLcore> lcores;
  lcores.reserve(kMaxLcores);
  for (int i = 0; i < kMaxLcores; i++) {
    unsigned int lcore = 0;
    if (mtl_get_lcore(handle, &lcore) < 0) break;
    lcores.emplace_back(handle, lcore);
  }
  EXPECT_EQ(test_lcore_cnt(ctx), base_cnt + static_cast<int>(lcores.size()));
  lcores.clear();
  EXPECT_EQ(test_lcore_cnt(ctx), base_cnt);

  test_lcore_one(ctx);
}

TEST(Main, lcore_expect_fail) {
  auto* ctx = st_test_ctx();
  ASSERT_LT(mtl_put_lcore(ctx->handle, 10000), 0);
  test_lcore_one(ctx);
}

TEST(Main, dev_started) {
  auto* ctx = st_test_ctx();
  mtl_test::ScopedMtlStart guard(ctx->handle);
  EXPECT_GE(guard.start(), 0);
  EXPECT_TRUE(test_dev_started(ctx));
  EXPECT_GE(guard.stop(), 0);
}
