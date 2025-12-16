/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#pragma once

#include <atomic>
#include <functional>
#include <vector>

#include "core/constants.hpp"
#include "pipeline_handler_base.hpp"

class St30pHandler : public PipelineHandlerBase<st30p_tx_ops, st30p_rx_ops,
                                                st30p_tx_handle, st30p_rx_handle> {
 public:
  explicit St30pHandler(st_tests_context* ctx, FrameTestStrategy* frameTestStrategy,
                        st30p_tx_ops ops_tx = {}, st30p_rx_ops ops_rx = {},
                        uint msPerFramebuffer = 10, bool create = true,
                        bool start = true);
  explicit St30pHandler(st_tests_context* ctx, st30p_tx_ops ops_tx = {},
                        st30p_rx_ops ops_rx = {}, uint msPerFramebuffer = 10);
  ~St30pHandler() override;

  void fillSt30pOps(uint transmissionPort = 30000, uint framebufferQueueSize = 3,
                    uint payloadType = 111, st30_fmt format = ST30_FMT_PCM16,
                    st30_sampling sampling = ST30_SAMPLING_48K, uint8_t channelCount = 2,
                    st30_ptime ptime = ST30_PTIME_1MS);

  void startSessionTx() override;
  void startSessionRx() override;

  void st30pTxDefaultFunction(std::atomic<bool>& stopFlag);
  void st30pRxDefaultFunction(std::atomic<bool>& stopFlag);

  void normalizeSessionOps();

  uint64_t nsPacketTime;

 private:
  uint msPerFramebuffer;
};
