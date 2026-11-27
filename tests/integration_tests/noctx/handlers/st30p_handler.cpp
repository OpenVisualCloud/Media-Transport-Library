/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include "st30p_handler.hpp"

#include <chrono>
#include <stdexcept>
#include <thread>

St30pHandler::St30pHandler(st_tests_context* ctx)
    : PipelineHandlerBase(ctx, nullptr, st30p_tx_create, st30p_rx_create, st30p_tx_free,
                          st30p_rx_free, st30p_tx_wake_block, st30p_rx_wake_block) {
  fillSt30pOps();
}

void St30pHandler::fillSt30pOps(uint transmissionPort, uint framebufferQueueSize,
                                uint payloadType, st30_fmt format, st30_sampling sampling,
                                uint8_t channelCount, st30_ptime ptime) {
  initDefaultPorts("st30_noctx_test_tx", "st30_noctx_test_rx", transmissionPort,
                   payloadType);
  sessionsOpsTx.fmt = format;
  sessionsOpsTx.channel = channelCount;
  sessionsOpsTx.sampling = sampling;
  sessionsOpsTx.ptime = ptime;
  sessionsOpsTx.framebuff_cnt = framebufferQueueSize;

  sessionsOpsRx.fmt = format;
  sessionsOpsRx.channel = channelCount;
  sessionsOpsRx.sampling = sampling;
  sessionsOpsRx.ptime = ptime;
  sessionsOpsRx.framebuff_cnt = framebufferQueueSize;

  normalizeSessionOps();
}

void St30pHandler::normalizeSessionOps() {
  auto recomputeFramebuff = [](const auto& ops) {
    return st30_calculate_framebuff_size(ops.fmt, ops.ptime, ops.sampling, ops.channel,
                                         kMsPerFramebuffer * NS_PER_MS, nullptr);
  };

  uint32_t txFrameBuff = recomputeFramebuff(sessionsOpsTx);
  uint32_t rxFrameBuff = recomputeFramebuff(sessionsOpsRx);
  if (!txFrameBuff || !rxFrameBuff) {
    throw std::runtime_error("Failed to compute st30 frame buffer size");
  }

  sessionsOpsTx.framebuff_size = txFrameBuff;
  sessionsOpsRx.framebuff_size = rxFrameBuff;

  int pktSize = st30_get_packet_size(sessionsOpsRx.fmt, sessionsOpsRx.ptime,
                                     sessionsOpsRx.sampling, sessionsOpsRx.channel);
  if (pktSize <= 0) {
    throw std::runtime_error("Invalid st30 packet configuration");
  }

  double pktTime = st30_get_packet_time(sessionsOpsRx.ptime);
  if (pktTime <= 0) {
    throw std::runtime_error("Invalid st30 packet time");
  }

  uint64_t totalPackets = sessionsOpsRx.framebuff_size / pktSize;
  if (!totalPackets) totalPackets = 1;
  uint64_t framesPerSec = (double)NS_PER_S / pktTime / totalPackets;
  if (!framesPerSec) framesPerSec = 1;
  nsFramebuffTime = NS_PER_S / framesPerSec;
}

template <bool IsRx, typename HandleT, typename OpsT, typename GetFn, typename PutFn>
void St30pHandler::runDefaultLoop(std::atomic<bool>& stopFlag, HandleT handle,
                                  const OpsT& ops, GetFn getFn, PutFn putFn) {
  ASSERT_TRUE(handle != nullptr);

  while (!stopFlag) {
    struct st30_frame* frame = getFn(handle);
    if (!frame) {
      std::this_thread::sleep_for(std::chrono::microseconds(100));
      continue;
    }

    EXPECT_NE(frame->addr, nullptr);
    EXPECT_EQ(frame->buffer_size, ops.framebuff_size);
    EXPECT_EQ(frame->data_size, ops.framebuff_size);
    EXPECT_EQ(frame->fmt, ops.fmt);
    EXPECT_EQ(frame->channel, ops.channel);
    EXPECT_EQ(frame->ptime, ops.ptime);
    EXPECT_EQ(frame->sampling, ops.sampling);

    if (frame->addr) {
      if constexpr (IsRx) {
        applyRxModifier(frame, frame->data_size);
      } else {
        applyTxModifier(frame, frame->data_size);
      }
    }

    int ret = putFn(handle, frame);
    EXPECT_GE(ret, 0);
    if (ret >= 0) {
      if constexpr (IsRx) {
        recordRxFrame();
      } else {
        recordTxFrame();
      }
    }
  }
}

void St30pHandler::st30pTxDefaultFunction(std::atomic<bool>& stopFlag) {
  runDefaultLoop<false>(stopFlag, sessionsHandleTx, sessionsOpsTx, st30p_tx_get_frame,
                        st30p_tx_put_frame);
}

void St30pHandler::st30pRxDefaultFunction(std::atomic<bool>& stopFlag) {
  runDefaultLoop<true>(stopFlag, sessionsHandleRx, sessionsOpsRx, st30p_rx_get_frame,
                       st30p_rx_put_frame);
}

void St30pHandler::startSessionTx() {
  startTxThread(
      [this](std::atomic<bool>& stopFlag) { this->st30pTxDefaultFunction(stopFlag); });
}

void St30pHandler::startSessionRx() {
  startRxThread(
      [this](std::atomic<bool>& stopFlag) { this->st30pRxDefaultFunction(stopFlag); });
}
