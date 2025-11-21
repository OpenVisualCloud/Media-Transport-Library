/* SPDX-License-Identifier: BSD-3-Clause */
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
  void addThread(std::function<void(std::atomic<bool>&)> func);
  bool isRunning() const;
  void stop();
  ~Session();

 private:
  std::vector<std::thread> threads_;
  std::atomic<bool> stopFlag_{false};
};
