/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include "session.hpp"

#include <chrono>
#include <utility>

void Session::addThread(std::function<void(std::atomic<bool>&)> func, bool isRx) {
  if (!func) {
    return;
  }
  std::atomic<bool>& stopFlag = isRx ? stopFlagRx : stopFlagTx;
  stopFlag.store(false);
  if (isRx) {
    hasRx_ = true;
  } else {
    hasTx_ = true;
  }

  threads_.emplace_back(func, std::ref(stopFlag));
}

void Session::setTxWakeCallback(std::function<void()> wakeFn) {
  wakeTx_ = std::move(wakeFn);
}

void Session::setRxWakeCallback(std::function<void()> wakeFn) {
  wakeRx_ = std::move(wakeFn);
}

void Session::stop() {
  /* Never started or already stopped: skip drain and join. */
  if (threads_.empty()) return;
  stopFlagTx.store(true);
  if (wakeTx_) {
    wakeTx_();
  }
  if (hasTx_ && hasRx_) {
    /* Let in-flight frames finish on RX before stopping the RX thread. */
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
  }
  stopFlagRx.store(true);
  if (wakeRx_) {
    wakeRx_();
  }
  for (auto& thread : threads_) {
    if (thread.joinable()) {
      thread.join();
    }
  }
  threads_.clear();
  hasTx_ = false;
  hasRx_ = false;
}

Session::~Session() {
  stop();
}
