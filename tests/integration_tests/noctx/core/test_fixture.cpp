/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include "test_fixture.hpp"

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <thread>

#include "handlers/st20p_handler.hpp"
#include "handlers/st30p_handler.hpp"
#include "session.hpp"
#include "strategy.hpp"

void NoCtxTest::SetUp() {
  ctx = new st_tests_context;
  if (!ctx) {
    throw std::runtime_error("NoCtxTest::SetUp no ctx");
  }

  memcpy(ctx, st_test_ctx(), sizeof(*ctx));

  if (ctx->handle) {
    throw std::runtime_error(
        "NoCtxTest::SetUp: ctx->handle is already initialized!\n"
        "This likely means the global context was not properly reset between tests.\n"
        "To run NOCTX tests, please use the '--no_ctx_tests' option to ensure a clean "
        "context.");
  }

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
  st30pHandlers.clear();
  st20pHandlers.clear();
  frameTestStrategies.clear();

  if (ctx) {
    if (ctx->handle) {
      mtl_uninit(ctx->handle);
      ctx->handle = (mtl_handle)0x1;
    }

    delete ctx;
    ctx = nullptr;
  }
}

uint64_t NoCtxTest::TestPtpSourceSinceEpoch(void* priv) {
  struct timespec spec;
  static std::atomic<uint64_t> adjustment_ns{0};

  if (adjustment_ns.load() == 0 || priv == nullptr) {
    struct timespec spec_adjustment_to_epoch;
    clock_gettime(CLOCK_MONOTONIC, &spec_adjustment_to_epoch);
    uint64_t temp_adjustment = (uint64_t)spec_adjustment_to_epoch.tv_sec * NS_PER_S +
                               spec_adjustment_to_epoch.tv_nsec;

    adjustment_ns.store(temp_adjustment);
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

NoCtxTest::St20pHandlerBundle NoCtxTest::createSt20pHandlerBundle(
    bool createTx, bool createRx,
    std::function<FrameTestStrategy*(St20pHandler*)> strategyFactory,
    std::function<void(St20pHandler*)> configure) {
  if (!ctx) {
    throw std::runtime_error("createSt20pHandlerBundle expects initialized ctx");
  }

  auto handlerOwned = std::make_unique<St20pHandler>(ctx);
  auto* handler = handlerOwned.get();
  if (configure) {
    configure(handler);
  }

  std::unique_ptr<FrameTestStrategy> strategyOwned;
  FrameTestStrategy* strategy = nullptr;
  if (strategyFactory) {
    strategyOwned.reset(strategyFactory(handler));
    strategy = strategyOwned.get();
    handler->setFrameTestStrategy(strategy);
  }

  if (createRx) {
    handler->createSessionRx();
  }
  if (createTx) {
    handler->createSessionTx();
  }

  auto bundle = registerSt20pResources(std::move(handlerOwned), std::move(strategyOwned));
  return bundle;
}

NoCtxTest::St20pHandlerBundle NoCtxTest::registerSt20pResources(
    std::unique_ptr<St20pHandler> handler, std::unique_ptr<FrameTestStrategy> strategy) {
  St20pHandlerBundle bundle;
  if (handler) {
    bundle.handler = handler.get();
    st20pHandlers.emplace_back(std::move(handler));
  }
  if (strategy) {
    bundle.strategy = strategy.get();
    frameTestStrategies.emplace_back(std::move(strategy));
  }
  return bundle;
}

NoCtxTest::St30pHandlerBundle NoCtxTest::createSt30pHandlerBundle(
    bool createTx, bool createRx,
    std::function<FrameTestStrategy*(St30pHandler*)> strategyFactory,
    std::function<void(St30pHandler*)> configure) {
  if (!ctx) {
    throw std::runtime_error("createSt30pHandlerBundle expects initialized ctx");
  }

  auto handlerOwned = std::make_unique<St30pHandler>(ctx);
  auto* handler = handlerOwned.get();
  if (configure) {
    configure(handler);
  }

  std::unique_ptr<FrameTestStrategy> strategyOwned;
  FrameTestStrategy* strategy = nullptr;
  if (strategyFactory) {
    strategyOwned.reset(strategyFactory(handler));
    strategy = strategyOwned.get();
    handler->setFrameTestStrategy(strategy);
  }

  if (createRx) {
    handler->createSessionRx();
  }
  if (createTx) {
    handler->createSessionTx();
  }

  return registerSt30pResources(std::move(handlerOwned), std::move(strategyOwned));
}

NoCtxTest::St30pHandlerBundle NoCtxTest::registerSt30pResources(
    std::unique_ptr<St30pHandler> handler, std::unique_ptr<FrameTestStrategy> strategy) {
  St30pHandlerBundle bundle;
  if (handler) {
    bundle.handler = handler.get();
    st30pHandlers.emplace_back(std::move(handler));
  }
  if (strategy) {
    bundle.strategy = strategy.get();
    frameTestStrategies.emplace_back(std::move(strategy));
  }
  return bundle;
}

void NoCtxTest::initSt20pDefaultContext() {
  if (!ctx) {
    throw std::runtime_error("initSt20pDefaultContext expects initialized ctx");
  }

  ctx->para.ptp_get_time_fn = NoCtxTest::TestPtpSourceSinceEpoch;
  ctx->para.log_level = MTL_LOG_LEVEL_INFO;
  ctx->para.flags &= ~MTL_FLAG_DEV_AUTO_START_STOP;
  ctx->handle = mtl_init(&ctx->para);
  ASSERT_TRUE(ctx->handle != nullptr);
}

bool NoCtxTest::waitForSession(Session& session, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (session.isRunning()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return session.isRunning();
}
