/* SPDX-License-Identifier: BSD-3-Clause */

#include "session.hpp"

void Session::addThread(std::function<void(std::atomic<bool>&)> func) {
  if (!func) {
    return;
  }

  if (threads_.empty()) {
    stopFlag_.store(false);
  }

  threads_.emplace_back(func, std::ref(stopFlag_));
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
  stopFlag_ = true;
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
