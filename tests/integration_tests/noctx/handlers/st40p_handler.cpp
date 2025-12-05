/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include "st40p_handler.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <thread>

namespace {
constexpr uint16_t kMaxAncUdwPerFrame = 255;
constexpr uint16_t kDefaultPayloadBytes = 16;
} /* namespace */

St40pHandler::St40pHandler(st_tests_context* ctx, FrameTestStrategy* frameTestStrategy,
                           st40p_tx_ops ops_tx, st40p_rx_ops ops_rx, bool create,
                           bool start)
    : PipelineHandlerBase(ctx, frameTestStrategy, st40p_tx_create, st40p_rx_create,
                          st40p_tx_free, st40p_rx_free) {
  if (ops_tx.name == nullptr && ops_rx.name == nullptr) {
    fillSt40pOps();
    ops_tx = sessionsOpsTx;
    ops_rx = sessionsOpsRx;
  } else {
    sessionsOpsTx = ops_tx;
    sessionsOpsRx = ops_rx;
  }

  setFrameTestStrategy(frameTestStrategy);

  if (create) {
    createSession(ops_tx, ops_rx, start);
  }
}

St40pHandler::St40pHandler(st_tests_context* ctx, st40p_tx_ops ops_tx,
                           st40p_rx_ops ops_rx)
    : PipelineHandlerBase(ctx, nullptr, st40p_tx_create, st40p_rx_create, st40p_tx_free,
                          st40p_rx_free) {
  if (ops_tx.name == nullptr && ops_rx.name == nullptr) {
    fillSt40pOps();
  } else {
    sessionsOpsTx = ops_tx;
    sessionsOpsRx = ops_rx;
  }
}

St40pHandler::~St40pHandler() = default;

void St40pHandler::fillSt40pOps(uint transmissionPort, uint framebufferQueueSize,
                                uint payloadType, enum st_fps fps, uint32_t maxUdwSize,
                                uint32_t rtpRingSize) {
  memset(&sessionsOpsTx, 0, sizeof(sessionsOpsTx));
  sessionsOpsTx.name = "st40p_noctx_test_tx";
  sessionsOpsTx.priv = ctx;
  memcpy(sessionsOpsTx.port.dip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
         MTL_IP_ADDR_LEN);
  memcpy(sessionsOpsTx.port.dip_addr[MTL_SESSION_PORT_R], ctx->mcast_ip_addr[MTL_PORT_R],
         MTL_IP_ADDR_LEN);
  sessionsOpsTx.port.num_port = 1;
  snprintf(sessionsOpsTx.port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
           ctx->para.port[MTL_PORT_P]);
  sessionsOpsTx.port.udp_port[MTL_SESSION_PORT_P] = transmissionPort;
  sessionsOpsTx.port.udp_port[MTL_SESSION_PORT_R] = transmissionPort + 1;
  sessionsOpsTx.port.payload_type = payloadType;
  sessionsOpsTx.fps = fps;
  sessionsOpsTx.interlaced = false;
  sessionsOpsTx.framebuff_cnt = framebufferQueueSize;
  sessionsOpsTx.max_udw_buff_size = maxUdwSize;
  sessionsOpsTx.flags = ST40P_TX_FLAG_BLOCK_GET;

  memset(&sessionsOpsRx, 0, sizeof(sessionsOpsRx));
  sessionsOpsRx.name = "st40p_noctx_test_rx";
  sessionsOpsRx.priv = ctx;
  sessionsOpsRx.port.num_port = 1;
  memcpy(sessionsOpsRx.port.ip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
         MTL_IP_ADDR_LEN);
  memcpy(sessionsOpsRx.port.ip_addr[MTL_SESSION_PORT_R], ctx->mcast_ip_addr[MTL_PORT_R],
         MTL_IP_ADDR_LEN);
  snprintf(sessionsOpsRx.port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
           ctx->para.port[MTL_PORT_R]);
  sessionsOpsRx.port.udp_port[MTL_SESSION_PORT_P] = transmissionPort;
  sessionsOpsRx.port.udp_port[MTL_SESSION_PORT_R] = transmissionPort + 1;
  sessionsOpsRx.port.payload_type = payloadType;
  sessionsOpsRx.interlaced = false;
  sessionsOpsRx.framebuff_cnt = framebufferQueueSize;
  sessionsOpsRx.max_udw_buff_size = maxUdwSize;
  sessionsOpsRx.rtp_ring_size = rtpRingSize;
  sessionsOpsRx.flags = ST40P_RX_FLAG_BLOCK_GET;

  resetFrameCounters();
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

    uint32_t frame_idx = txFrames();
    populateFrame(frame_info, frame_idx);

    applyTxModifier(frame_info, frame_info->udw_buffer_fill);

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

    ASSERT_TRUE(frame_info->meta != nullptr);
    ASSERT_GT(frame_info->meta_num, 0u);
    ASSERT_TRUE(frame_info->udw_buff_addr != nullptr);
    ASSERT_GT(frame_info->udw_buffer_fill, 0u);

    applyRxModifier(frame_info, frame_info->udw_buffer_fill);

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
  ASSERT_TRUE(frame_info != nullptr);
  ASSERT_TRUE(frame_info->meta != nullptr);
  ASSERT_TRUE(frame_info->udw_buff_addr != nullptr);

  size_t capacity = frame_info->udw_buffer_size;
  ASSERT_GT(capacity, 0u);
  uint32_t payload_bytes =
      static_cast<uint32_t>(std::min<size_t>(capacity, kMaxAncUdwPerFrame));

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
