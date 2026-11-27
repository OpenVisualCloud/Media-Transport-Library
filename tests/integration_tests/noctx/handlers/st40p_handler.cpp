/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include "st40p_handler.hpp"

#include <algorithm>
#include <chrono>
#include <thread>

namespace {
constexpr uint16_t kMaxAncUdwPerFrame = 255;
} /* namespace */

St40pHandler::St40pHandler(st_tests_context* ctx)
    : PipelineHandlerBase(ctx, nullptr, st40p_tx_create, st40p_rx_create, st40p_tx_free,
                          st40p_rx_free, st40p_tx_wake_block, st40p_rx_wake_block) {
  fillSt40pOps();
}

void St40pHandler::fillSt40pOps(uint transmissionPort, uint framebufferQueueSize,
                                uint payloadType, enum st_fps fps, uint32_t maxUdwSize,
                                uint32_t rtpRingSize) {
  initDefaultPorts("st40p_noctx_test_tx", "st40p_noctx_test_rx", transmissionPort,
                   payloadType);
  sessionsOpsTx.fps = fps;
  sessionsOpsTx.framebuff_cnt = framebufferQueueSize;
  sessionsOpsTx.max_udw_buff_size = maxUdwSize;
  sessionsOpsTx.flags = ST40P_TX_FLAG_BLOCK_GET;

  sessionsOpsRx.framebuff_cnt = framebufferQueueSize;
  sessionsOpsRx.max_udw_buff_size = maxUdwSize;
  sessionsOpsRx.rtp_ring_size = rtpRingSize;
  sessionsOpsRx.flags = ST40P_RX_FLAG_BLOCK_GET;
}

void St40pHandler::st40pTxDefaultFunction(std::atomic<bool>& stopFlag) {
  st40p_tx_handle handle = sessionsHandleTx;
  ASSERT_TRUE(handle != nullptr);

  while (!stopFlag) {
    struct st40_frame_info* frame_info = st40p_tx_get_frame(handle);
    if (!frame_info) {
      std::this_thread::sleep_for(std::chrono::microseconds(100));
      continue;
    }

    EXPECT_NE(frame_info->meta, nullptr);
    EXPECT_NE(frame_info->udw_buff_addr, nullptr);
    EXPECT_GT(frame_info->udw_buffer_size, 0u);

    if (frame_info->meta && frame_info->udw_buff_addr &&
        frame_info->udw_buffer_size > 0u) {
      uint32_t frame_idx = txFrames();
      populateFrame(frame_info, frame_idx);
      applyTxModifier(frame_info, frame_info->udw_buffer_fill);
    }

    int ret = st40p_tx_put_frame(handle, frame_info);
    EXPECT_GE(ret, 0);
    if (ret >= 0) {
      recordTxFrame();
    }
  }
}

void St40pHandler::st40pRxDefaultFunction(std::atomic<bool>& stopFlag) {
  st40p_rx_handle handle = sessionsHandleRx;
  ASSERT_TRUE(handle != nullptr);

  while (!stopFlag) {
    struct st40_frame_info* frame_info = st40p_rx_get_frame(handle);
    if (!frame_info) {
      std::this_thread::sleep_for(std::chrono::microseconds(100));
      continue;
    }

    EXPECT_NE(frame_info->meta, nullptr);
    EXPECT_GT(frame_info->meta_num, 0u);
    EXPECT_NE(frame_info->udw_buff_addr, nullptr);
    EXPECT_GT(frame_info->udw_buffer_fill, 0u);

    if (frame_info->meta && frame_info->udw_buff_addr) {
      applyRxModifier(frame_info, frame_info->udw_buffer_fill);
    }

    int ret = st40p_rx_put_frame(handle, frame_info);
    EXPECT_GE(ret, 0);
    if (ret >= 0) {
      recordRxFrame();
    }
  }
}

void St40pHandler::startSessionTx() {
  startTxThread(
      [this](std::atomic<bool>& stopFlag) { this->st40pTxDefaultFunction(stopFlag); });
}

void St40pHandler::startSessionRx() {
  startRxThread(
      [this](std::atomic<bool>& stopFlag) { this->st40pRxDefaultFunction(stopFlag); });
}

void St40pHandler::populateFrame(struct st40_frame_info* frame_info, uint32_t frame_idx) {
  uint32_t payload_bytes = static_cast<uint32_t>(
      std::min<size_t>(frame_info->udw_buffer_size, kMaxAncUdwPerFrame));

  for (uint32_t i = 0; i < payload_bytes; i++) {
    frame_info->udw_buff_addr[i] = static_cast<uint8_t>((frame_idx + i) & 0xff);
  }

  frame_info->meta_num = 1;
  frame_info->udw_buffer_fill = payload_bytes;
  struct st40_meta& meta = frame_info->meta[0];
  meta.c = 0;
  meta.line_number = 0;
  meta.hori_offset = 0;
  meta.s = 0;
  meta.stream_num = 0;
  meta.did = 0x45;
  meta.sdid = 0x01;
  meta.udw_size = static_cast<uint16_t>(payload_bytes);
  meta.udw_offset = 0;
}
