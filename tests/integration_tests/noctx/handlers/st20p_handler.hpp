/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#pragma once

#include <atomic>
#include <functional>
#include <vector>

#include "core/constants.hpp"
#include "pipeline_handler_base.hpp"

class St20pHandler : public PipelineHandlerBase<st20p_tx_ops, st20p_rx_ops,
                                                st20p_tx_handle, st20p_rx_handle> {
 public:
  uint64_t nsFrameTime;

  St20pHandler(st_tests_context* ctx, FrameTestStrategy* frameTestStrategy,
               st20p_tx_ops ops_tx = {}, st20p_rx_ops ops_rx = {}, bool create = true,
               bool start = true);
  explicit St20pHandler(st_tests_context* ctx, st20p_tx_ops ops_tx = {},
                        st20p_rx_ops ops_rx = {});
  ~St20pHandler() override;

  void fillSt20Ops(uint transmissionPort = 20000, uint framebufferQueueSize = 3,
                   enum st20_fmt fmt = ST20_FMT_YUV_422_10BIT, uint width = 1920,
                   uint height = 1080, uint payloadType = 112,
                   enum st_fps fps = ST_FPS_P25, bool interlaced = false,
                   enum st20_packing packing = ST20_PACKING_BPM);

  void normalizeSessionOps();

  void startSessionTx() override;
  void startSessionRx() override;

  void st20TxDefaultFunction(std::atomic<bool>& stopFlag);
  void st20RxDefaultFunction(std::atomic<bool>& stopFlag);
};
