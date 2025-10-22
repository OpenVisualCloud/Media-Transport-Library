/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include "noctx.hpp"

St20pHandler::St20pHandler(st_tests_context* ctx, FrameTestStrategy* sessionUserData,
                         st20p_tx_ops ops_tx, st20p_rx_ops ops_rx, bool create, bool start)
    : Handlers(ctx, sessionUserData) {
  if (ops_tx.name == nullptr && ops_rx.name == nullptr) {
    fillSt20Ops();
    ops_tx = sessionsOpsTx;
    ops_rx = sessionsOpsRx;
  } else {
    sessionsOpsTx = ops_tx;
    sessionsOpsRx = ops_rx;
  }

  EXPECT_TRUE(sessionUserData != nullptr);
  if (!sessionUserData) return;

  this->sessionUserData = sessionUserData;
  sessionUserData->parent = this;

  if (create) {
    createSession(ops_tx, ops_rx, start);
  }
}

St20pHandler::St20pHandler(st_tests_context* ctx, st20p_tx_ops ops_tx, st20p_rx_ops ops_rx)
    : Handlers(ctx) {
  if (ops_tx.name == nullptr && ops_rx.name == nullptr) {
    fillSt20Ops();
  } else {
    sessionsOpsTx = ops_tx;
    sessionsOpsRx = ops_rx;
  }
}

St20pHandler::~St20pHandler() {
  session.stop();
  if (sessionsHandleTx) {
    st20p_tx_free(sessionsHandleTx);
  }

  if (sessionsHandleRx) {
    st20p_rx_free(sessionsHandleRx);
  }
}

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


  nsFrameTime = st_frame_rate(fps);
  if (nsFrameTime == 0) nsFrameTime = NS_PER_S / 25;
  else
    nsFrameTime = NS_PER_S / nsFrameTime;
}

void St20pHandler::createSession(st20p_tx_ops ops_tx, st20p_rx_ops ops_rx, bool start) {
  sessionsOpsTx = ops_tx;
  sessionsOpsRx = ops_rx;

  createSessionTx();
  createSessionRx();

  if (start) {
    startSession();
  }
}

void St20pHandler::createSession(bool start) {
  createSessionTx();
  createSessionRx();

  if (start) {
    startSession();
  }
}

void St20pHandler::createSessionTx() {
  ASSERT_TRUE(ctx && ctx->handle != nullptr);
  auto ops = sessionsOpsTx;

  st20p_tx_handle tx_handle = st20p_tx_create(ctx->handle, &ops);
  EXPECT_TRUE(tx_handle != nullptr);
  sessionsHandleTx = tx_handle;
}

void St20pHandler::createSessionRx() {
  ASSERT_TRUE(ctx && ctx->handle != nullptr);
  auto ops = sessionsOpsRx;

  st20p_rx_handle rx_handle = st20p_rx_create(ctx->handle, &ops);
  EXPECT_TRUE(rx_handle != nullptr);
  sessionsHandleRx = rx_handle;
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

    ASSERT_TRUE(frame->addr != nullptr);
    ASSERT_EQ(frame->fmt, (enum st_frame_fmt)fmt);
    ASSERT_EQ(frame->width, width);
    ASSERT_EQ(frame->height, height);

    if (sessionUserData->enable_tx_modifier) {
      sessionUserData->txTestFrameModifier(frame->addr, frameSize);
    }

    frame->data_size = frameSize;
    st20p_tx_put_frame(handle, frame);
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

    ASSERT_TRUE(frame->addr != nullptr);
    ASSERT_EQ(frame->fmt, (enum st_frame_fmt)fmt);
    ASSERT_EQ(frame->width, width);
    ASSERT_EQ(frame->height, height);
    ASSERT_GE(frame->data_size, frameSize);

    if (sessionUserData->enable_rx_modifier) {
      sessionUserData->rxTestFrameModifier(frame->addr, frame->data_size);
    }

    st20p_rx_put_frame(handle, frame);
  }
}

void St20pHandler::startSession() {
  Handlers::startSession(
      {[this](std::atomic<bool>& stopFlag) { this->st20TxDefaultFunction(stopFlag); },
       [this](std::atomic<bool>& stopFlag) { this->st20RxDefaultFunction(stopFlag); }});
}

void St20pHandler::startSessionTx() {
  Handlers::startSession(
      {[this](std::atomic<bool>& stopFlag) { this->st20TxDefaultFunction(stopFlag); }});
}

void St20pHandler::startSessionRx() {
  Handlers::startSession(
      {[this](std::atomic<bool>& stopFlag) { this->st20RxDefaultFunction(stopFlag); }});
}

void St20pHandler::startSession(
    std::vector<std::function<void(std::atomic<bool>&)>> threadFunctions) {
  Handlers::startSession(threadFunctions);
}

/**
 * @brief Set the session port names for TX and RX, including redundant ports if
 * specified.
 *
 * This function updates the port names in sessionsOpsTx and sessionsOpsRx based on the
 * provided indices. If an index is SESSION_SKIP_PORT, that port is not set. If both
 * primary and redundant ports are set, num_port is set to 2, otherwise to 1.
 *
 * @param txPortIdx Index for the primary TX port in ctx->para.port, or SESSION_SKIP_PORT
 * to skip.
 * @param rxPortIdx Index for the primary RX port in ctx->para.port, or SESSION_SKIP_PORT
 * to skip.
 * @param txPortRedundantIdx Index for the redundant TX port in ctx->para.port, or
 * SESSION_SKIP_PORT to skip.
 * @param rxPortRedundantIdx Index for the redundant RX port in ctx->para.port, or
 * SESSION_SKIP_PORT to skip.
 */
void St20pHandler::setSessionPorts(int txPortIdx, int rxPortIdx, int txPortRedundantIdx,
                                  int rxPortRedundantIdx) {
  setSessionPortsTx(&(this->sessionsOpsTx.port), txPortIdx, txPortRedundantIdx);
  setSessionPortsRx(&(this->sessionsOpsRx.port), rxPortIdx, rxPortRedundantIdx);
}