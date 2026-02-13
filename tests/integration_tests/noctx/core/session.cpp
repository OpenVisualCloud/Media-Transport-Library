/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include "session.hpp"

void Session::addThread(std::function<void(std::atomic<bool>&)> func, bool isRx) {
  if (!func) {
    return;
  }
  std::atomic<bool>& stopFlag = isRx ? stopFlagRx : stopFlagTx;

  if (threads_.empty()) {
    stopFlag.store(false);
  }

  threads_.emplace_back(func, std::ref(stopFlag));
}

bool Session::isRunning() const {
  if (threads_.empty()) {
    return false;
  }

  for (const auto& thread : threads_) {
    if (!thread.joinable()) {
      return false;
    }
  }

  return true;
}

void Session::stop() {
  stopFlagTx.store(true);
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  stopFlagRx.store(true);
  for (auto& thread : threads_) {
    if (thread.joinable()) {
      thread.join();
    }
  }
  threads_.clear();
}

Session::~Session() {
  stop();
}
