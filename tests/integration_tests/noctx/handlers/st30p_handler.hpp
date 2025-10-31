/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

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

  void startSession(std::vector<std::function<void(std::atomic<bool>&)>> threadFunctions,
                    bool isRx);
  void startSession();
  void startSessionTx();
  void startSessionRx();

  void st30pTxDefaultFunction(std::atomic<bool>& stopFlag);
  void st30pRxDefaultFunction(std::atomic<bool>& stopFlag);

  /**
   * @brief Set the session port names for TX and RX, including redundant ports if
   * specified.
   *
   * This function updates the port names in sessionsOpsTx and sessionsOpsRx based on the
   * provided indices. If an index is SESSION_SKIP_PORT, that port is not set. If both
   * primary and redundant ports are set, num_port is set to 2, otherwise to 1.
   *
   * @param txPortIdx Index for the primary TX port in ctx->para.port, or
   * SESSION_SKIP_PORT to skip.
   * @param rxPortIdx Index for the primary RX port in ctx->para.port, or
   * SESSION_SKIP_PORT to skip.
   * @param txPortRedundantIdx Index for the redundant TX port in ctx->para.port, or
   * SESSION_SKIP_PORT to skip.
   * @param rxPortRedundantIdx Index for the redundant RX port in ctx->para.port, or
   * SESSION_SKIP_PORT to skip.
   */
  void setSessionPorts(int txPortIdx = SESSION_SKIP_PORT,
                       int rxPortIdx = SESSION_SKIP_PORT,
                       int txPortRedundantIdx = SESSION_SKIP_PORT,
                       int rxPortRedundantIdx = SESSION_SKIP_PORT);

  uint64_t nsPacketTime;

 private:
  uint msPerFramebuffer;
};
