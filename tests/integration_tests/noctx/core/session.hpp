/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#pragma once

#include <atomic>
#include <functional>
#include <thread>
#include <vector>

/**
 * @brief Helper that owns the background threads used by TX/RX handlers.
 */
class Session {
 public:
  void addThread(std::function<void(std::atomic<bool>&)> func, bool isRx);
  void setTxWakeCallback(std::function<void()> wakeFn);
  void setRxWakeCallback(std::function<void()> wakeFn);
  void clearTxWakeCallback();
  void clearRxWakeCallback();
  bool isRunning() const;
  void stop();
  ~Session();

 private:
  std::vector<std::thread> threads_;
  std::atomic<bool> stopFlagRx{false};
  std::atomic<bool> stopFlagTx{false};
  bool hasTx_{false};
  bool hasRx_{false};
  std::function<void()> wakeTx_;
  std::function<void()> wakeRx_;
};
