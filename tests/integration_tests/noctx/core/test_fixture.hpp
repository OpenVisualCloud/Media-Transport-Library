/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#pragma once

#include <gtest/gtest.h>
#include <time.h>

#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "handlers/st20p_handler.hpp"
#include "handlers/st30p_handler.hpp"
#include "handlers/st40p_handler.hpp"
#include "tests.hpp"

/* CLOCK_MONOTONIC_RAW in ns: the clock mt_get_tsc() calibrates against. */
inline uint64_t monotonicRawNs() {
  struct timespec spec;
  (void)clock_gettime(CLOCK_MONOTONIC_RAW, &spec);
  return static_cast<uint64_t>(spec.tv_sec) * NS_PER_S + spec.tv_nsec;
}

class NoCtxTest : public ::testing::Test {
 protected:
  struct st_tests_context* ctx = nullptr;

  void SetUp() override;
  void TearDown() override;

 public:
  template <typename HandlerT>
  struct HandlerBundle {
    HandlerT* handler = nullptr;
    FrameTestStrategy* strategy = nullptr;
  };

  using St20pHandlerBundle = HandlerBundle<St20pHandler>;
  using St30pHandlerBundle = HandlerBundle<St30pHandler>;
  using St40pHandlerBundle = HandlerBundle<St40pHandler>;

  uint defaultTestDuration = 0;

  /* PTP time for mtl_init(): CLOCK_MONOTONIC_RAW since the last StartFakePtpClock(),
   * or since the first call after SetUp(). */
  static uint64_t FakePtpClockNow(void* priv);
  static void StartFakePtpClock();
  static void ResetFakePtpClock();

  /* Sleeps sleepDuration s (0: defaultTestDuration, 20 s or NOCTX_TEST_DURATION),
   * up to the first failure. */
  void sleepUntilFailure(int sleepDuration = 0);

  /* Default ops TX TEST_PORT_1 -> RX TEST_PORT_2, then configure(), then the sessions,
   * then the strategy, so a user-pacing plan starts after session setup. */
  template <typename HandlerT>
  HandlerBundle<HandlerT> createHandlerBundle(
      bool createTx, bool createRx,
      std::function<FrameTestStrategy*(HandlerT*)> strategyFactory,
      std::function<void(HandlerT*)> configure = nullptr) {
    if (!ctx) {
      throw std::runtime_error("createHandlerBundle expects initialized ctx");
    }

    auto handlerOwned = std::make_unique<HandlerT>(ctx);
    auto* handler = handlerOwned.get();
    if (configure) {
      configure(handler);
    }
    if constexpr (std::is_same_v<HandlerT, St30pHandler>) {
      handler->normalizeSessionOps();
    }

    if (createRx) {
      handler->createSessionRx();
    }
    if (createTx) {
      handler->createSessionTx();
    }

    /* User-pacing strategies plan from construction; setup must not eat the lead. */
    HandlerBundle<HandlerT> bundle;
    bundle.handler = handler;
    handlers_.emplace_back(std::move(handlerOwned));
    if (strategyFactory) {
      std::unique_ptr<FrameTestStrategy> strategyOwned(strategyFactory(handler));
      bundle.strategy = strategyOwned.get();
      handler->setFrameTestStrategy(bundle.strategy);
      frameTestStrategies.emplace_back(std::move(strategyOwned));
    }
    return bundle;
  }

  St20pHandlerBundle createSt20pHandlerBundle(
      bool createTx, bool createRx,
      std::function<FrameTestStrategy*(St20pHandler*)> strategyFactory,
      std::function<void(St20pHandler*)> configure = nullptr) {
    return createHandlerBundle<St20pHandler>(
        createTx, createRx, std::move(strategyFactory), std::move(configure));
  }
  St30pHandlerBundle createSt30pHandlerBundle(
      bool createTx, bool createRx,
      std::function<FrameTestStrategy*(St30pHandler*)> strategyFactory,
      std::function<void(St30pHandler*)> configure = nullptr) {
    return createHandlerBundle<St30pHandler>(
        createTx, createRx, std::move(strategyFactory), std::move(configure));
  }
  St40pHandlerBundle createSt40pHandlerBundle(
      bool createTx, bool createRx,
      std::function<FrameTestStrategy*(St40pHandler*)> strategyFactory,
      std::function<void(St40pHandler*)> configure = nullptr) {
    return createHandlerBundle<St40pHandler>(
        createTx, createRx, std::move(strategyFactory), std::move(configure));
  }

  using PtpTimeFn = uint64_t (*)(void* priv);
  /* mtl_init() with the command-line params, the SetUp() changes (RANDOM_SRC_PORT, INFO
   * log, 16 TX and RX queues per port), ptpTimeFn and DEV_AUTO_START_STOP off. */
  void initDefaultContext(PtpTimeFn ptpTimeFn = FakePtpClockNow);
  /* SKIPs, or FAILs with NOCTX_REQUIRE_STRICT=1, unless unsupported is empty; the
   * caller returns on IsSkipped() || HasFatalFailure(). */
  static void requireStrictTopology(const std::string& unsupported);
  /* initDefaultContext() plus MTL_FLAG_ENABLE_HW_TIMESTAMP, after
   * requireStrictTopology(strictPacingTopologyError()) of TEST_PORT_1 -> TEST_PORT_2. */
  void initStrictPacingContext(PtpTimeFn ptpTimeFn = FakePtpClockNow);

  std::vector<std::unique_ptr<Handlers>> handlers_;
  std::vector<std::unique_ptr<FrameTestStrategy>> frameTestStrategies;
};
