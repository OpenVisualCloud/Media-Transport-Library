/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#pragma once

#include <mtl/st40_pipeline_api.h>

#include <atomic>
#include <functional>
#include <vector>

#include "core/constants.hpp"
#include "pipeline_handler_base.hpp"

class St40pHandler : public PipelineHandlerBase<st40p_tx_ops, st40p_rx_ops,
                                                st40p_tx_handle, st40p_rx_handle> {
 public:
  St40pHandler(st_tests_context* ctx, FrameTestStrategy* frameTestStrategy,
               st40p_tx_ops ops_tx = {}, st40p_rx_ops ops_rx = {}, bool create = true,
               bool start = true);
  explicit St40pHandler(st_tests_context* ctx, st40p_tx_ops ops_tx = {},
                        st40p_rx_ops ops_rx = {});
  ~St40pHandler() override;

  void fillSt40pOps(uint transmissionPort = 31000, uint framebufferQueueSize = 4,
                    uint payloadType = 113, enum st_fps fps = ST_FPS_P60,
                    uint32_t maxUdwSize = 256, uint32_t rtpRingSize = 2048);

  void startSessionTx() override;
  void startSessionRx() override;

  void st40pTxDefaultFunction(std::atomic<bool>& stopFlag);
  void st40pRxDefaultFunction(std::atomic<bool>& stopFlag);

 private:
  void populateFrame(struct st40_frame_info* frame_info, uint32_t frame_idx);
};
