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
#include "handlers/st40p_handler.hpp"
#include "session.hpp"
#include "strategy.hpp"

namespace {

struct TestPtpClockState {
  std::atomic<uint64_t> start_ns{0};
  std::atomic<bool> running{false};
};

TestPtpClockState g_test_ptp_clock;

uint64_t monotonicNowNs() {
  struct timespec spec;
  clock_gettime(CLOCK_MONOTONIC, &spec);
  return (uint64_t)spec.tv_sec * NS_PER_S + spec.tv_nsec;
}

void startClock(uint64_t now) {
  g_test_ptp_clock.start_ns.store(now, std::memory_order_release);
  g_test_ptp_clock.running.store(true, std::memory_order_release);
}

void resetClock() {
  g_test_ptp_clock.running.store(false, std::memory_order_release);
  g_test_ptp_clock.start_ns.store(0, std::memory_order_release);
}

} /* namespace */

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
        "To run NOCTX tests, please use the '--no_ctx' option to ensure a clean "
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
  ResetFakePtpClock();
  defaultTestDuration = 20;
}

void NoCtxTest::TearDown() {
  st40pHandlers.clear();
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

uint64_t NoCtxTest::FakePtpClockNow(void* priv) {
  (void)priv;
  if (!g_test_ptp_clock.running.load(std::memory_order_acquire)) {
    StartFakePtpClock();
  }

  const uint64_t start = g_test_ptp_clock.start_ns.load(std::memory_order_acquire);
  const uint64_t now = monotonicNowNs();
  if (now <= start) {
    return 0;
  }

  return now - start;
}

void NoCtxTest::StartFakePtpClock() {
  startClock(monotonicNowNs());
}

void NoCtxTest::ResetFakePtpClock() {
  resetClock();
}

void NoCtxTest::sleepUntilFailure(int sleep_duration) {
  if (!sleep_duration) {
    sleep_duration = defaultTestDuration;
  }

  for (int i = 0; i < sleep_duration * 10; ++i) {
    if (HasFailure()) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
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

  handler->normalizeSessionOps();

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

  handler->normalizeSessionOps();

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

NoCtxTest::St40pHandlerBundle NoCtxTest::createSt40pHandlerBundle(
    bool createTx, bool createRx,
    std::function<FrameTestStrategy*(St40pHandler*)> strategyFactory,
    std::function<void(St40pHandler*)> configure) {
  if (!ctx) {
    throw std::runtime_error("createSt40pHandlerBundle expects initialized ctx");
  }

  auto handlerOwned = std::make_unique<St40pHandler>(ctx);
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

  return registerSt40pResources(std::move(handlerOwned), std::move(strategyOwned));
}

NoCtxTest::St40pHandlerBundle NoCtxTest::registerSt40pResources(
    std::unique_ptr<St40pHandler> handler, std::unique_ptr<FrameTestStrategy> strategy) {
  St40pHandlerBundle bundle;
  if (handler) {
    bundle.handler = handler.get();
    st40pHandlers.emplace_back(std::move(handler));
  }
  if (strategy) {
    bundle.strategy = strategy.get();
    frameTestStrategies.emplace_back(std::move(strategy));
  }
  return bundle;
}

void NoCtxTest::initDefaultContext() {
  if (!ctx) {
    throw std::runtime_error("initDefaultContext expects initialized ctx");
  }

  ctx->para.ptp_get_time_fn = NoCtxTest::FakePtpClockNow;
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
