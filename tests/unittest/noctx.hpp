/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include <gtest/gtest.h>

#include <thread>
#include <vector>

#include "tests.hpp"

template <typename SessionOps>
struct ManagedSession {
  std::thread tx_thread;
  std::thread rx_thread;
  std::atomic<bool> stop_flag{false};
  SessionOps ops;

  template <typename... Args>
  ManagedSession(Args&&... args) : ops(std::forward<Args>(args)...) {
  }

  void start_tx(std::function<void(SessionOps&, std::atomic<bool>&)> tx_func) {
    tx_thread = std::thread([this, tx_func]() { tx_func(ops, stop_flag); });
  }

  void start_rx(std::function<void(SessionOps&, std::atomic<bool>&)> rx_func) {
    rx_thread = std::thread([this, rx_func]() { rx_func(ops, stop_flag); });
  }

  void stop_and_join() {
    stop_flag = true;
    if (tx_thread.joinable()) tx_thread.join();
    if (rx_thread.joinable()) rx_thread.join();
  }
};

class NoCtxTest : public ::testing::Test {
 protected:
  struct st_tests_context* ctx = nullptr;

  void SetUp() override;
  void TearDown() override;

 public:
  static uint64_t TestPtpSourceSinceEpoch(void* priv);

  std::vector<std::thread> tx_thread;
  std::vector<std::atomic<bool>> stop_flag_tx;
  std::vector<std::thread> rx_thread;
  std::vector<std::atomic<bool>> stop_flag_rx;
};

class NoCtxTestSt30p : public NoCtxTest {
 public:
  std::vector<enum st30_sampling> samplingModesDefault;
  std::vector<enum st30_ptime> ptimeModesDefault;
  std::vector<uint16_t> channelCountsDefault;
  std::vector<enum st30_fmt> fmtModesDefault;
  std::vector<struct st30p_tx_ops> sessionsOpsTx;
  std::vector<struct st30p_rx_ops> sessionsOpsRx;
  std::vector<std::unique_ptr<ManagedSession<st30p_tx_ops>>> tx_sessions;
  std::vector<std::unique_ptr<ManagedSession<st30p_rx_ops>>> rx_sessions;

  void SetUp() override;

  void createSessions(st30p_tx_ops ops, st30p_rx_ops ops_rx) {
    sessionsOpsTx.push_back(ops);
    sessionsOpsRx.push_back(ops_rx);
  }

  size_t create_tx_session(const st30p_tx_ops& ops) {
    tx_sessions.push_back(std::make_unique<ManagedSession<st30p_tx_ops>>(ops));
    return tx_sessions.size() - 1;
  }
};
