/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include "st20p_handler.hpp"

#include <chrono>
#include <thread>

St20pHandler::St20pHandler(st_tests_context* ctx)
    : PipelineHandlerBase(ctx, nullptr, st20p_tx_create, st20p_rx_create, st20p_tx_free,
                          st20p_rx_free, st20p_tx_wake_block, st20p_rx_wake_block) {
  fillSt20Ops();
}

void St20pHandler::fillSt20Ops(uint transmissionPort, uint framebufferQueueSize,
                               enum st20_fmt fmt, uint width, uint height,
                               uint payloadType, enum st_fps fps, bool interlaced) {
  initDefaultPorts("st20p_noctx_test_tx", "st20p_noctx_test_rx", transmissionPort,
                   payloadType);
  sessionsOpsTx.width = width;
  sessionsOpsTx.height = height;
  sessionsOpsTx.fps = fps;
  sessionsOpsTx.input_fmt = (enum st_frame_fmt)fmt;
  sessionsOpsTx.interlaced = interlaced;
  sessionsOpsTx.framebuff_cnt = framebufferQueueSize;

  sessionsOpsRx.width = width;
  sessionsOpsRx.height = height;
  sessionsOpsRx.fps = fps;
  sessionsOpsRx.output_fmt = (enum st_frame_fmt)fmt;
  sessionsOpsRx.interlaced = interlaced;
  sessionsOpsRx.framebuff_cnt = framebufferQueueSize;
}

void St20pHandler::st20TxDefaultFunction(std::atomic<bool>& stopFlag) {
  struct st_frame* frame;
  st20p_tx_handle handle = sessionsHandleTx;
  ASSERT_TRUE(handle != nullptr);
  uint32_t width = sessionsOpsTx.width;
  uint32_t height = sessionsOpsTx.height;
  const enum st_frame_fmt fmt = sessionsOpsTx.input_fmt;
  bool interlaced = sessionsOpsTx.interlaced;

  uint frameSize = st_frame_size(fmt, width, height, interlaced);

  while (!stopFlag) {
    frame = st20p_tx_get_frame(handle);
    if (!frame) {
      std::this_thread::sleep_for(std::chrono::microseconds(100));
      continue;
    }

    EXPECT_NE(frame->addr[0], nullptr);
    EXPECT_EQ(frame->fmt, fmt);
    EXPECT_EQ(frame->width, width);
    EXPECT_EQ(frame->height, height);

    if (frame->addr[0]) {
      applyTxModifier(frame, frameSize);
    }

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

    EXPECT_NE(frame->addr[0], nullptr);
    EXPECT_EQ(frame->fmt, fmt);
    EXPECT_EQ(frame->width, width);
    EXPECT_EQ(frame->height, height);
    EXPECT_GE(frame->data_size, frameSize);

    if (frame->addr[0]) {
      applyRxModifier(frame, frame->data_size);
    }

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
