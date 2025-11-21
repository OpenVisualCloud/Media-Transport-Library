/* SPDX-License-Identifier: BSD-3-Clause */
#pragma once

#include <atomic>
#include <functional>
#include <vector>

#include "core/constants.hpp"
#include "core/handler_base.hpp"

class St20pHandler : public Handlers {
 public:
  uint64_t nsFrameTime;

  St20pHandler(st_tests_context* ctx, FrameTestStrategy* frameTestStrategy,
               st20p_tx_ops ops_tx = {}, st20p_rx_ops ops_rx = {}, bool create = true,
               bool start = true);
  explicit St20pHandler(st_tests_context* ctx, st20p_tx_ops ops_tx = {},
                        st20p_rx_ops ops_rx = {});
  ~St20pHandler();

  struct st20p_tx_ops sessionsOpsTx;
  struct st20p_rx_ops sessionsOpsRx;
  st20p_tx_handle sessionsHandleTx = nullptr;
  st20p_rx_handle sessionsHandleRx = nullptr;

  void fillSt20Ops(uint transmissionPort = 20000, uint framebufferQueueSize = 3,
                   enum st20_fmt fmt = ST20_FMT_YUV_422_10BIT, uint width = 1920,
                   uint height = 1080, uint payloadType = 112,
                   enum st_fps fps = ST_FPS_P25, bool interlaced = false,
                   enum st20_packing packing = ST20_PACKING_BPM);

  void setFrameTestStrategy(FrameTestStrategy* frameTestStrategy);

  void createSession(st20p_tx_ops ops_tx, st20p_rx_ops ops_rx, bool start = true);
  void createSession(bool start = true);
  void createSessionTx();
  void createSessionRx();

  void startSession(std::vector<std::function<void(std::atomic<bool>&)>> threadFunctions);
  void startSession();
  void startSessionTx();
  void startSessionRx();

  void st20TxDefaultFunction(std::atomic<bool>& stopFlag);
  void st20RxDefaultFunction(std::atomic<bool>& stopFlag);

  void setSessionPorts(int txPortIdx = SESSION_SKIP_PORT,
                       int rxPortIdx = SESSION_SKIP_PORT,
                       int txPortRedundantIdx = SESSION_SKIP_PORT,
                       int rxPortRedundantIdx = SESSION_SKIP_PORT);
};
