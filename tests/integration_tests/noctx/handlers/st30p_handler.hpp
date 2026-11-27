/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#pragma once

#include <atomic>

#include "pipeline_handler_base.hpp"

class St30pHandler : public PipelineHandlerBase<st30p_tx_ops, st30p_rx_ops,
                                                st30p_tx_handle, st30p_rx_handle> {
 public:
  explicit St30pHandler(st_tests_context* ctx);
  ~St30pHandler() override = default;

  void fillSt30pOps(uint transmissionPort = 30000, uint framebufferQueueSize = 3,
                    uint payloadType = 111, st30_fmt format = ST30_FMT_PCM16,
                    st30_sampling sampling = ST30_SAMPLING_48K, uint8_t channelCount = 2,
                    st30_ptime ptime = ST30_PTIME_1MS);

  void startSessionTx() override;
  void startSessionRx() override;

  /* Every buffer, both directions: ASSERT buffer_size and data_size == framebuff_size,
   * fmt, channel, ptime, sampling as in the ops; strategy; EXPECT put_frame >= 0. */
  void st30pTxDefaultFunction(std::atomic<bool>& stopFlag);
  void st30pRxDefaultFunction(std::atomic<bool>& stopFlag);

  void normalizeSessionOps();

  uint64_t nsFramebuffTime = 0;

 private:
  static constexpr uint kMsPerFramebuffer = 10;

  template <bool IsRx, typename HandleT, typename OpsT, typename GetFn, typename PutFn>
  void runDefaultLoop(std::atomic<bool>& stopFlag, HandleT handle, const OpsT& ops,
                      GetFn getFn, PutFn putFn);
};
