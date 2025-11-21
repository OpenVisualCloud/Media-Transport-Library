/* SPDX-License-Identifier: BSD-3-Clause */
#pragma once

#include <atomic>
#include <functional>
#include <vector>

#include "session.hpp"
#include "strategy.hpp"
#include "tests.hpp"

class Handlers {
 public:
  explicit Handlers(st_tests_context* ctx,
                    FrameTestStrategy* frameTestStrategy = nullptr);
  virtual ~Handlers();

  void startSession(
      std::vector<std::function<void(std::atomic<bool>&)>> threadFunctions = {});
  void stopSession();

  void setSessionPortsRx(struct st_rx_port* port, int rxPortIdx, int rxPortRedundantIdx);
  void setSessionPortsTx(struct st_tx_port* port, int txPortIdx, int txPortRedundantIdx);

  Session session;
  st_tests_context* ctx;
  FrameTestStrategy* frameTestStrategy;
};
