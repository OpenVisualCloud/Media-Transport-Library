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
  bool isRunning() const;
  void stop();
  ~Session();

 private:
  std::vector<std::thread> threads_;
  std::atomic<bool> stopFlagRx{false};
  std::atomic<bool> stopFlagTx{false};
};
