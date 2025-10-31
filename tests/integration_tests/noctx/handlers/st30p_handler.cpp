/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include "st30p_handler.hpp"

#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <thread>

St30pHandler::St30pHandler(st_tests_context* ctx, FrameTestStrategy* frameTestStrategy,
                           st30p_tx_ops ops_tx, st30p_rx_ops ops_rx,
                           uint msPerFramebuffer, bool create, bool start)
    : Handlers(ctx, frameTestStrategy),
      nsPacketTime(0),
      msPerFramebuffer(msPerFramebuffer) {
  if (ops_tx.name == nullptr && ops_rx.name == nullptr) {
    fillSt30pOps();
    ops_tx = sessionsOpsTx;
    ops_rx = sessionsOpsRx;
  } else {
    sessionsOpsTx = ops_tx;
    sessionsOpsRx = ops_rx;
  }

  if (!frameTestStrategy) throw std::runtime_error("St30pHandler no frameTestStrategy");

  this->frameTestStrategy = frameTestStrategy;
  frameTestStrategy->parent = this;

  if (create) {
    createSession(ops_tx, ops_rx, start);
  }
}

St30pHandler::St30pHandler(st_tests_context* ctx, st30p_tx_ops ops_tx,
                           st30p_rx_ops ops_rx, uint msPerFramebuffer)
    : Handlers(ctx), nsPacketTime(0), msPerFramebuffer(msPerFramebuffer) {
  if (ops_tx.name == nullptr && ops_rx.name == nullptr) {
    fillSt30pOps();
  } else {
    sessionsOpsTx = ops_tx;
    sessionsOpsRx = ops_rx;
  }
}

St30pHandler::~St30pHandler() {
  session.stop();
  if (sessionsHandleTx) {
    st30p_tx_free(sessionsHandleTx);
  }

  if (sessionsHandleRx) {
    st30p_rx_free(sessionsHandleRx);
  }
}

void St30pHandler::fillSt30pOps(uint transmissionPort, uint framebufferQueueSize,
                                uint payloadType, st30_fmt format, st30_sampling sampling,
                                uint8_t channelCount, st30_ptime ptime) {
  uint frameBufferSize = st30_calculate_framebuff_size(
      format, ptime, sampling, channelCount, msPerFramebuffer * NS_PER_MS, nullptr);

  memset(&sessionsOpsTx, 0, sizeof(sessionsOpsTx));
  sessionsOpsTx.name = "st30_noctx_test_tx";
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
  sessionsOpsTx.fmt = format;
  sessionsOpsTx.channel = channelCount;
  sessionsOpsTx.sampling = sampling;
  sessionsOpsTx.ptime = ptime;
  sessionsOpsTx.framebuff_size = frameBufferSize;
  sessionsOpsTx.framebuff_cnt = framebufferQueueSize;
  sessionsOpsTx.notify_frame_available = nullptr;

  memset(&sessionsOpsRx, 0, sizeof(sessionsOpsRx));
  sessionsOpsRx.name = "st30_noctx_test_rx";
  sessionsOpsRx.priv = ctx;
  sessionsOpsRx.port.num_port = 1;
  memcpy(sessionsOpsRx.port.ip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
         MTL_IP_ADDR_LEN);
  memcpy(sessionsOpsRx.port.ip_addr[MTL_SESSION_PORT_R], ctx->mcast_ip_addr[MTL_PORT_R],
         MTL_IP_ADDR_LEN);

  sessionsOpsTx.port.num_port = 1;
  snprintf(sessionsOpsRx.port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
           ctx->para.port[MTL_PORT_R]);

  sessionsOpsRx.port.udp_port[MTL_SESSION_PORT_P] = transmissionPort;
  sessionsOpsRx.port.udp_port[MTL_SESSION_PORT_R] = transmissionPort + 1;
  sessionsOpsRx.port.payload_type = payloadType;
  sessionsOpsRx.fmt = format;
  sessionsOpsRx.channel = channelCount;
  sessionsOpsRx.sampling = sampling;
  sessionsOpsRx.ptime = ptime;
  sessionsOpsRx.framebuff_size = frameBufferSize;
  sessionsOpsRx.framebuff_cnt = framebufferQueueSize;
  sessionsOpsRx.notify_frame_available = nullptr;

  uint64_t totalPackets =
      sessionsOpsRx.framebuff_size /
      st30_get_packet_size(sessionsOpsRx.fmt, sessionsOpsRx.ptime, sessionsOpsRx.sampling,
                           sessionsOpsRx.channel);

  uint64_t framesPerSec =
      (double)NS_PER_S / st30_get_packet_time(sessionsOpsRx.ptime) / totalPackets;

  if (framesPerSec == 0) framesPerSec = 1;
  nsPacketTime = NS_PER_S / framesPerSec;
}

void St30pHandler::setFrameTestStrategy(FrameTestStrategy* newStrategy) {
  frameTestStrategy = newStrategy;
  if (frameTestStrategy) {
    frameTestStrategy->parent = this;
  }
}

void St30pHandler::createSession(st30p_tx_ops ops_tx, st30p_rx_ops ops_rx, bool start) {
  sessionsOpsTx = ops_tx;
  sessionsOpsRx = ops_rx;

  createSessionTx();
  createSessionRx();

  if (start) {
    startSession();
  }
}

void St30pHandler::createSession(bool start) {
  createSessionTx();
  createSessionRx();

  if (start) {
    startSession();
  }
}

void St30pHandler::createSessionTx() {
  if (!ctx || !ctx->handle) {
    throw std::runtime_error("St30pHandler::createSessionTx no ctx or ctx->handle");
  }
  auto ops = sessionsOpsTx;

  st30p_tx_handle tx_handle = st30p_tx_create(ctx->handle, &ops);
  EXPECT_TRUE(tx_handle != nullptr);
  sessionsHandleTx = tx_handle;
}

void St30pHandler::createSessionRx() {
  if (!ctx || !ctx->handle) {
    throw std::runtime_error("St30pHandler::createSessionRx no ctx or ctx->handle");
  }
  auto ops = sessionsOpsRx;

  st30p_rx_handle rx_handle = st30p_rx_create(ctx->handle, &ops);
  EXPECT_TRUE(rx_handle != nullptr);
  sessionsHandleRx = rx_handle;
}

void St30pHandler::st30pTxDefaultFunction(std::atomic<bool>& stopFlag) {
  struct st30_frame* frame;
  st30p_tx_handle handle = sessionsHandleTx;
  ASSERT_TRUE(handle != nullptr);

  while (!stopFlag) {
    frame = st30p_tx_get_frame(handle);
    if (!frame) {
      continue;
    }

    ASSERT_EQ(frame->buffer_size, sessionsOpsTx.framebuff_size);
    ASSERT_EQ(frame->data_size, sessionsOpsTx.framebuff_size);
    ASSERT_EQ(frame->fmt, sessionsOpsTx.fmt);
    ASSERT_EQ(frame->channel, sessionsOpsTx.channel);
    ASSERT_EQ(frame->ptime, sessionsOpsTx.ptime);
    ASSERT_EQ(frame->sampling, sessionsOpsTx.sampling);

    if (frameTestStrategy && frameTestStrategy->enable_tx_modifier) {
      frameTestStrategy->txTestFrameModifier(frame, frame->data_size);
    }

    st30p_tx_put_frame((st30p_tx_handle)handle, frame);
  }
}

void St30pHandler::st30pRxDefaultFunction(std::atomic<bool>& stopFlag) {
  struct st30_frame* frame;
  st30p_rx_handle handle = sessionsHandleRx;
  ASSERT_TRUE(handle != nullptr);

  while (!stopFlag) {
    frame = st30p_rx_get_frame((st30p_rx_handle)handle);
    if (!frame) {
      continue;
    }

    ASSERT_EQ(frame->buffer_size, sessionsOpsRx.framebuff_size);
    ASSERT_EQ(frame->data_size, sessionsOpsRx.framebuff_size);
    ASSERT_EQ(frame->fmt, sessionsOpsRx.fmt);
    ASSERT_EQ(frame->channel, sessionsOpsRx.channel);
    ASSERT_EQ(frame->ptime, sessionsOpsRx.ptime);
    ASSERT_EQ(frame->sampling, sessionsOpsRx.sampling);

    if (frameTestStrategy && frameTestStrategy->enable_rx_modifier) {
      frameTestStrategy->rxTestFrameModifier(frame, frame->data_size);
    }

    st30p_rx_put_frame((st30p_rx_handle)handle, frame);
  }
}

void St30pHandler::startSession() {
  startSessionRx();
  startSessionTx();
}

void St30pHandler::startSessionTx() {
  Handlers::startSession(
      {[this](std::atomic<bool>& stopFlag) { this->st30pTxDefaultFunction(stopFlag); }},
      /*isRx=*/false);
}

void St30pHandler::startSessionRx() {
  Handlers::startSession(
      {[this](std::atomic<bool>& stopFlag) { this->st30pRxDefaultFunction(stopFlag); }},
      /*isRx=*/true);
}

void St30pHandler::startSession(
    std::vector<std::function<void(std::atomic<bool>&)>> threadFunctions, bool isRx) {
  Handlers::startSession(threadFunctions, isRx);
}

void St30pHandler::setSessionPorts(int txPortIdx, int rxPortIdx, int txPortRedundantIdx,
                                   int rxPortRedundantIdx) {
  setSessionPortsTx(&(this->sessionsOpsTx.port), txPortIdx, txPortRedundantIdx);
  setSessionPortsRx(&(this->sessionsOpsRx.port), rxPortIdx, rxPortRedundantIdx);
}
