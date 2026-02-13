/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#pragma once

#include <gtest/gtest.h>

#include <chrono>
#include <functional>
#include <memory>
#include <vector>

#include "handlers/st20p_handler.hpp"
#include "handlers/st30p_handler.hpp"
#include "handlers/st40p_handler.hpp"
#include "tests.hpp"

class Session;
class FrameTestStrategy;
class NoCtxTest : public ::testing::Test {
 protected:
  static constexpr int SessionStartTimeoutMs = 1500;

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

  static uint64_t FakePtpClockNow(void* priv);
  static void StartFakePtpClock();
  static void ResetFakePtpClock();

  void sleepUntilFailure(int sleepDuration = 0);
  St20pHandlerBundle createSt20pHandlerBundle(
      bool createTx, bool createRx,
      std::function<FrameTestStrategy*(St20pHandler*)> strategyFactory,
      std::function<void(St20pHandler*)> configure = nullptr);
  St20pHandlerBundle registerSt20pResources(std::unique_ptr<St20pHandler> handler,
                                            std::unique_ptr<FrameTestStrategy> strategy);
  St30pHandlerBundle createSt30pHandlerBundle(
      bool createTx, bool createRx,
      std::function<FrameTestStrategy*(St30pHandler*)> strategyFactory,
      std::function<void(St30pHandler*)> configure = nullptr);
  St30pHandlerBundle registerSt30pResources(std::unique_ptr<St30pHandler> handler,
                                            std::unique_ptr<FrameTestStrategy> strategy);
  St40pHandlerBundle createSt40pHandlerBundle(
      bool createTx, bool createRx,
      std::function<FrameTestStrategy*(St40pHandler*)> strategyFactory,
      std::function<void(St40pHandler*)> configure = nullptr);
  St40pHandlerBundle registerSt40pResources(std::unique_ptr<St40pHandler> handler,
                                            std::unique_ptr<FrameTestStrategy> strategy);
  void initDefaultContext();
  bool waitForSession(Session& session,
                      std::chrono::milliseconds timeout =
                          std::chrono::milliseconds(SessionStartTimeoutMs));

  std::vector<std::unique_ptr<St40pHandler>> st40pHandlers;
  std::vector<std::unique_ptr<St30pHandler>> st30pHandlers;
  std::vector<std::unique_ptr<St20pHandler>> st20pHandlers;
  std::vector<std::unique_ptr<FrameTestStrategy>> frameTestStrategies;
};
