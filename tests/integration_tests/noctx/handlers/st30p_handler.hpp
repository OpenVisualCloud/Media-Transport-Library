/* SPDX-License-Identifier: BSD-3-Clause */
#pragma once

#include <atomic>
#include <functional>
#include <vector>

#include "core/constants.hpp"
#include "core/handler_base.hpp"

class St30pHandler : public Handlers {
 public:
  explicit St30pHandler(st_tests_context* ctx, FrameTestStrategy* frameTestStrategy,
                        st30p_tx_ops ops_tx = {}, st30p_rx_ops ops_rx = {},
                        uint msPerFramebuffer = 10, bool create = true,
                        bool start = true);
  explicit St30pHandler(st_tests_context* ctx, st30p_tx_ops ops_tx = {},
                        st30p_rx_ops ops_rx = {}, uint msPerFramebuffer = 10);
  ~St30pHandler();

  struct st30p_tx_ops sessionsOpsTx;
  struct st30p_rx_ops sessionsOpsRx;
  st30p_tx_handle sessionsHandleTx = nullptr;
  st30p_rx_handle sessionsHandleRx = nullptr;

  void fillSt30pOps(uint transmissionPort = 30000, uint framebufferQueueSize = 3,
                    uint payloadType = 111, st30_fmt format = ST30_FMT_PCM16,
                    st30_sampling sampling = ST30_SAMPLING_48K, uint8_t channelCount = 2,
                    st30_ptime ptime = ST30_PTIME_1MS);

  void setFrameTestStrategy(FrameTestStrategy* frameTestStrategy);

  void createSession(st30p_tx_ops ops_tx, st30p_rx_ops ops_rx, bool start = true);
  void createSession(bool start = true);
  void createSessionTx();
  void createSessionRx();

  void startSession(std::vector<std::function<void(std::atomic<bool>&)>> threadFunctions);
  void startSession();
  void startSessionTx();
  void startSessionRx();

  void st30pTxDefaultFunction(std::atomic<bool>& stopFlag);
  void st30pRxDefaultFunction(std::atomic<bool>& stopFlag);

  void setSessionPorts(int txPortIdx = SESSION_SKIP_PORT,
                       int rxPortIdx = SESSION_SKIP_PORT,
                       int txPortRedundantIdx = SESSION_SKIP_PORT,
                       int rxPortRedundantIdx = SESSION_SKIP_PORT);

  uint64_t nsPacketTime;

 private:
  uint msPerFramebuffer;
};
