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
    : PipelineHandlerBase(ctx, frameTestStrategy, st30p_tx_create, st30p_rx_create,
                          st30p_tx_free, st30p_rx_free),
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

  setFrameTestStrategy(frameTestStrategy);

  if (create) {
    createSession(ops_tx, ops_rx, start);
  }
}

St30pHandler::St30pHandler(st_tests_context* ctx, st30p_tx_ops ops_tx,
                           st30p_rx_ops ops_rx, uint msPerFramebuffer)
    : PipelineHandlerBase(ctx, nullptr, st30p_tx_create, st30p_rx_create, st30p_tx_free,
                          st30p_rx_free),
      nsPacketTime(0),
      msPerFramebuffer(msPerFramebuffer) {
  if (ops_tx.name == nullptr && ops_rx.name == nullptr) {
    fillSt30pOps();
  } else {
    sessionsOpsTx = ops_tx;
    sessionsOpsRx = ops_rx;
  }
}

St30pHandler::~St30pHandler() = default;

void St30pHandler::fillSt30pOps(uint transmissionPort, uint framebufferQueueSize,
                                uint payloadType, st30_fmt format, st30_sampling sampling,
                                uint8_t channelCount, st30_ptime ptime) {
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
  sessionsOpsRx.framebuff_cnt = framebufferQueueSize;
  sessionsOpsRx.notify_frame_available = nullptr;

  normalizeSessionOps();
}

void St30pHandler::normalizeSessionOps() {
  auto recomputeFramebuff = [this](const auto& ops) {
    return st30_calculate_framebuff_size(ops.fmt, ops.ptime, ops.sampling, ops.channel,
                                         msPerFramebuffer * NS_PER_MS, nullptr);
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
  nsPacketTime = NS_PER_S / framesPerSec;
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

    applyTxModifier(frame, frame->data_size);

    int ret = st30p_tx_put_frame((st30p_tx_handle)handle, frame);
    EXPECT_GE(ret, 0);
    if (ret >= 0) {
      recordTxFrame();
    }
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

    applyRxModifier(frame, frame->data_size);

    int ret = st30p_rx_put_frame((st30p_rx_handle)handle, frame);
    EXPECT_GE(ret, 0);
    if (ret >= 0) {
      recordRxFrame();
    }
  }
}

void St30pHandler::startSessionTx() {
  startTxThread(
      [this](std::atomic<bool>& stopFlag) { this->st30pTxDefaultFunction(stopFlag); });
}

void St30pHandler::startSessionRx() {
  startRxThread(
      [this](std::atomic<bool>& stopFlag) { this->st30pRxDefaultFunction(stopFlag); });
}
