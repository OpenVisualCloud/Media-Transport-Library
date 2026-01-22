/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include "st20p_handler.hpp"

#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <thread>

St20pHandler::St20pHandler(st_tests_context* ctx, FrameTestStrategy* frameTestStrategy,
                           st20p_tx_ops ops_tx, st20p_rx_ops ops_rx, bool create,
                           bool start)
    : PipelineHandlerBase(ctx, frameTestStrategy, st20p_tx_create, st20p_rx_create,
                          st20p_tx_free, st20p_rx_free),
      nsFrameTime(0) {
  if (ops_tx.name == nullptr && ops_rx.name == nullptr) {
    fillSt20Ops();
    ops_tx = sessionsOpsTx;
    ops_rx = sessionsOpsRx;
  } else {
    sessionsOpsTx = ops_tx;
    sessionsOpsRx = ops_rx;
  }

  EXPECT_TRUE(frameTestStrategy != nullptr);
  if (!frameTestStrategy) return;

  setFrameTestStrategy(frameTestStrategy);

  if (create) {
    createSession(ops_tx, ops_rx, start);
  }
}

St20pHandler::St20pHandler(st_tests_context* ctx, st20p_tx_ops ops_tx,
                           st20p_rx_ops ops_rx)
    : PipelineHandlerBase(ctx, nullptr, st20p_tx_create, st20p_rx_create, st20p_tx_free,
                          st20p_rx_free),
      nsFrameTime(0) {
  if (ops_tx.name == nullptr && ops_rx.name == nullptr) {
    fillSt20Ops();
  } else {
    sessionsOpsTx = ops_tx;
    sessionsOpsRx = ops_rx;
  }
}

St20pHandler::~St20pHandler() = default;

void St20pHandler::fillSt20Ops(uint transmissionPort, uint framebufferQueueSize,
                               enum st20_fmt fmt, uint width, uint height,
                               uint payloadType, enum st_fps fps, bool interlaced,
                               enum st20_packing packing) {
  memset(&sessionsOpsTx, 0, sizeof(sessionsOpsTx));
  sessionsOpsTx.name = "st20p_noctx_test_tx";
  sessionsOpsTx.priv = ctx;
  memcpy(sessionsOpsTx.port.dip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
         MTL_IP_ADDR_LEN);
  memcpy(sessionsOpsTx.port.dip_addr[MTL_SESSION_PORT_R], ctx->mcast_ip_addr[MTL_PORT_R],
         MTL_IP_ADDR_LEN);
  /* Don't enable Redundant by default */
  sessionsOpsTx.port.num_port = 1;
  snprintf(sessionsOpsTx.port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
           ctx->para.port[MTL_PORT_P]);

  sessionsOpsTx.port.udp_port[MTL_SESSION_PORT_P] = transmissionPort;
  sessionsOpsTx.port.udp_port[MTL_SESSION_PORT_R] = transmissionPort + 1;
  sessionsOpsTx.port.payload_type = payloadType;
  sessionsOpsTx.width = width;
  sessionsOpsTx.height = height;
  sessionsOpsTx.fps = fps;
  sessionsOpsTx.input_fmt = (enum st_frame_fmt)fmt;
  sessionsOpsTx.interlaced = interlaced;
  sessionsOpsTx.framebuff_cnt = framebufferQueueSize;
  sessionsOpsTx.notify_frame_done = nullptr;

  memset(&sessionsOpsRx, 0, sizeof(sessionsOpsRx));
  sessionsOpsRx.name = "st20p_noctx_test_rx";
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
  sessionsOpsRx.width = width;
  sessionsOpsRx.height = height;
  sessionsOpsRx.fps = fps;
  sessionsOpsRx.output_fmt = (enum st_frame_fmt)fmt;
  sessionsOpsRx.interlaced = interlaced;
  sessionsOpsRx.framebuff_cnt = framebufferQueueSize;

  normalizeSessionOps();
}

void St20pHandler::normalizeSessionOps() {
  auto fpsToInteger = [](enum st_fps fps) {
    return static_cast<uint64_t>(st_frame_rate(fps));
  };

  uint64_t frameRate = fpsToInteger(sessionsOpsTx.fps);
  if (!frameRate) {
    frameRate = fpsToInteger(sessionsOpsRx.fps);
  }

  if (!frameRate) {
    nsFrameTime = NS_PER_S / 25;
    return;
  }

  nsFrameTime = NS_PER_S / frameRate;
}

void St20pHandler::st20TxDefaultFunction(std::atomic<bool>& stopFlag) {
  struct st_frame* frame;
  st20p_tx_handle handle = sessionsHandleTx;
  ASSERT_TRUE(handle != nullptr);
  uint32_t width = sessionsOpsTx.width;
  uint32_t height = sessionsOpsTx.height;
  enum st20_fmt fmt = (enum st20_fmt)sessionsOpsTx.input_fmt;

  uint frameSize = st_frame_size((enum st_frame_fmt)fmt, width, height, false);

  while (!stopFlag) {
    frame = st20p_tx_get_frame(handle);
    if (!frame) {
      std::this_thread::sleep_for(std::chrono::microseconds(100));
      continue;
    }

    ASSERT_NE(frame->addr[0], nullptr);
    ASSERT_EQ(frame->fmt, (enum st_frame_fmt)fmt);
    ASSERT_EQ(frame->width, width);
    ASSERT_EQ(frame->height, height);

    applyTxModifier(frame->addr, frameSize);

    frame->data_size = frameSize;
    int ret = st20p_tx_put_frame(handle, frame);
    EXPECT_GE(ret, 0);
    if (ret >= 0) {
      recordTxFrame();
    }
  }
}

void St20pHandler::st20RxDefaultFunction(std::atomic<bool>& stopFlag) {
  struct st_frame* frame;
  st20p_rx_handle handle = sessionsHandleRx;
  ASSERT_TRUE(handle != nullptr);
  st_frame_fmt fmt = sessionsOpsRx.output_fmt;
  uint32_t width = sessionsOpsRx.width;
  uint32_t height = sessionsOpsRx.height;
  bool interlaced = sessionsOpsRx.interlaced;

  uint frameSize = st_frame_size(fmt, width, height, interlaced);

  while (!stopFlag) {
    frame = st20p_rx_get_frame(handle);
    if (!frame) {
      std::this_thread::sleep_for(std::chrono::microseconds(100));
      continue;
    }

    ASSERT_NE(frame->addr[0], nullptr);
    ASSERT_EQ(frame->fmt, (enum st_frame_fmt)fmt);
    ASSERT_EQ(frame->width, width);
    ASSERT_EQ(frame->height, height);
    ASSERT_GE(frame->data_size, frameSize);

    applyRxModifier(frame->addr, frame->data_size);

    int ret = st20p_rx_put_frame(handle, frame);
    EXPECT_GE(ret, 0);
    if (ret >= 0) {
      recordRxFrame();
    }
  }
}

void St20pHandler::startSessionTx() {
  startTxThread(
      [this](std::atomic<bool>& stopFlag) { this->st20TxDefaultFunction(stopFlag); });
}

void St20pHandler::startSessionRx() {
  startRxThread(
      [this](std::atomic<bool>& stopFlag) { this->st20RxDefaultFunction(stopFlag); });
}
