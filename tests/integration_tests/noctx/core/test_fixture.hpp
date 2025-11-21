/* SPDX-License-Identifier: BSD-3-Clause */
#pragma once

#include <gtest/gtest.h>

#include <chrono>
#include <functional>
#include <memory>
#include <vector>

#include "tests.hpp"

class Session;
class FrameTestStrategy;
class St20pHandler;
class St30pHandler;
class StrategySharedState;

class NoCtxTest : public ::testing::Test {
 protected:
  static constexpr int SessionStartTimeoutMs = 1500;
  static constexpr int TxRxDelayMs = 200;

  struct st_tests_context* ctx = nullptr;

  void SetUp() override;
  void TearDown() override;

 public:
  struct St20pHandlerBundle {
    St20pHandler* handler = nullptr;
    FrameTestStrategy* strategy = nullptr;
    std::shared_ptr<StrategySharedState> sharedState;
  };

  uint defaultTestDuration = 0;

  static uint64_t TestPtpSourceSinceEpoch(void* priv);

  void sleepUntilFailure(int sleepDuration = 0);
  St20pHandlerBundle createSt20pHandlerBundle(
      bool createTx, bool createRx,
      std::function<FrameTestStrategy*(St20pHandler*)> strategyFactory,
      std::function<void(St20pHandler*)> configure = nullptr,
      std::shared_ptr<StrategySharedState> sharedState = nullptr);
  St20pHandlerBundle registerSt20pResources(std::unique_ptr<St20pHandler> handler,
                                            std::unique_ptr<FrameTestStrategy> strategy);
  void initSt20pDefaultContext();
  bool waitForSession(Session& session,
                      std::chrono::milliseconds timeout =
                          std::chrono::milliseconds(SessionStartTimeoutMs));
  bool startRxThenTx(St20pHandlerBundle& rxBundle, St20pHandlerBundle& txBundle,
                     std::chrono::milliseconds warmup =
                         std::chrono::milliseconds(SessionStartTimeoutMs));
  void stopTxThenRx(
      St20pHandlerBundle& txBundle, St20pHandlerBundle& rxBundle,
      std::chrono::milliseconds rxDelay = std::chrono::milliseconds(TxRxDelayMs));

  std::vector<std::unique_ptr<St30pHandler>> st30pHandlers;
  std::vector<std::unique_ptr<St20pHandler>> st20pHandlers;
  std::vector<std::unique_ptr<FrameTestStrategy>> frameTestStrategys;
};
