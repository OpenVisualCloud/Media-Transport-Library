/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include "noctx.hpp"

void NoCtxTest::SetUp() {
  ctx = new struct st_tests_context;

  /* NOCTX test: always operate on a copy of the global ctx.
     Do not use the global ctx directly for anything except copying its values.
   */
  ASSERT_TRUE(ctx != nullptr);
  memcpy(ctx, st_test_ctx(), sizeof(*ctx));

  ASSERT_TRUE(ctx->handle == nullptr) << "NOCTX tests should be run wiht --no_ctx_tests flag!";

  ctx->level = ST_TEST_LEVEL_MANDATORY;
  ctx->para.flags |= MTL_FLAG_RANDOM_SRC_PORT;
  ctx->para.log_level = MTL_LOG_LEVEL_INFO;
  ctx->para.priv = ctx;
  ctx->para.tx_queues_cnt[MTL_PORT_P] = 16;
  ctx->para.tx_queues_cnt[MTL_PORT_R] = 16;
  ctx->para.rx_queues_cnt[MTL_PORT_P] = 16;
  ctx->para.rx_queues_cnt[MTL_PORT_R] = 16;
  defaultTestDuration = 20;
}

void NoCtxTest::TearDown() {
  mtl_stop(ctx->handle);
  st30pHandlers.clear();

  if (ctx) {
    if (ctx->handle) {
      mtl_uninit(ctx->handle);
      /* WA for reinitialization issues */
      sleep(10);
    }

    delete ctx;
    ctx = nullptr;
  }
}

/* create ptp time that will set the time to 0 */
uint64_t NoCtxTest::TestPtpSourceSinceEpoch(void* priv) {
  struct timespec spec;
  static std::atomic<uint64_t> adjustment_ns{0};

  if (adjustment_ns.load() == 0 || priv == nullptr) {
    struct timespec spec_adjustment_to_epoch;
    clock_gettime(CLOCK_MONOTONIC, &spec_adjustment_to_epoch);
    uint64_t temp_adjustment = (uint64_t)spec_adjustment_to_epoch.tv_sec * NS_PER_S +
                               spec_adjustment_to_epoch.tv_nsec;
    uint64_t expected = 0;
    adjustment_ns.compare_exchange_strong(expected, temp_adjustment);
  }

  clock_gettime(CLOCK_MONOTONIC, &spec);
  uint64_t result =
      ((uint64_t)spec.tv_sec * NS_PER_S + spec.tv_nsec) - adjustment_ns.load();
  return result;
}

void NoCtxTest::sleepUntilFailure(int sleep_duration) {
  if (!sleep_duration) {
    sleep_duration = defaultTestDuration;
  }

  for (int i = 0; i < sleep_duration; ++i) {
    if (HasFailure()) break;
    sleep(1);
  }
}
