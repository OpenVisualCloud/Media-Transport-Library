/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include "noctx.hpp"

St30pHandler::St30pHandler(
    st_tests_context* ctx,
    std::function<void(void* frame, size_t frame_size)> txTestFrameModifier,
    std::function<void(void* frame, size_t frame_size)> rxTestFrameModifier,
    st30p_tx_ops ops_tx,
    st30p_rx_ops ops_rx,
    uint msPerFramebuffer)
    : Handlers(ctx, txTestFrameModifier, rxTestFrameModifier, 48000),
      msPerFramebuffer(msPerFramebuffer) {

  if (ops_tx.name == nullptr || ops_rx.name == nullptr) {
    fillSt30pOps();
  } else {
    sessionsOpsTx = ops_tx;
    sessionsOpsRx = ops_rx;
  }

  startSession({
    [this](std::atomic<bool>& stopFlag) { this->st30pTxDefaultFunction(stopFlag); },
    [this](std::atomic<bool>& stopFlag) { this->st30pRxDefaultFunction(stopFlag); }
  });
}

St30pHandler::St30pHandler(
    st_tests_context* ctx,
    st30p_tx_ops ops_tx,
    st30p_rx_ops ops_rx,
    uint msPerFramebuffer)
    : Handlers(ctx, 48000),
      msPerFramebuffer(msPerFramebuffer) {

  if (ops_tx.name == nullptr || ops_rx.name == nullptr) {
    fillSt30pOps();
  } else {
    sessionsOpsTx = ops_tx;
    sessionsOpsRx = ops_rx;
  }
}

St30pHandler::~St30pHandler() {
  stopSession();
  if (sessionsHandleTx) {
    st30p_tx_free(sessionsHandleTx);
  }

  if (sessionsHandleRx) {
    st30p_rx_free(sessionsHandleRx);
  }
}

void St30pHandler::fillSt30pOps(
      uint transmissionPort,
      uint framebufferQueueSize,
      uint payloadType,
      st30_fmt format,
      st30_sampling sampling,
      uint8_t channelCount,
      st30_ptime ptime
) {
  uint frameBufferSize = st30_calculate_framebuff_size(
      format, ptime, sampling, channelCount, msPerFramebuffer * NS_PER_MS, nullptr);

  memset(&sessionsOpsTx, 0, sizeof(sessionsOpsTx));
  sessionsOpsTx.name = "st30_noctx_test_tx";
  sessionsOpsTx.priv = ctx;
  sessionsOpsTx.port.num_port = 1;
  memcpy(sessionsOpsTx.port.dip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
         MTL_IP_ADDR_LEN);
  snprintf(sessionsOpsTx.port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
           ctx->para.port[MTL_PORT_P]);

  sessionsOpsTx.port.udp_port[MTL_SESSION_PORT_P] = transmissionPort;
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
  snprintf(sessionsOpsRx.port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
           ctx->para.port[MTL_PORT_R]);

  sessionsOpsRx.port.udp_port[MTL_SESSION_PORT_P] = transmissionPort;
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

void St30pHandler::createSession(st30p_tx_ops ops_tx, st30p_rx_ops ops_rx, bool start) {
  sessionsOpsTx = ops_tx;
  sessionsOpsRx = ops_rx;

  createTxSession();
  createRxSession();

  if (start) {
    startSession();
  }
}

void St30pHandler::createSession(bool start) {
  createTxSession();
  createRxSession();

  if (start) {
    startSession();
  }
}

void St30pHandler::createTxSession() {
  ASSERT_TRUE(ctx && ctx->handle != nullptr);
  auto ops = sessionsOpsTx;

  st30p_tx_handle tx_handle = st30p_tx_create(ctx->handle, &ops);
  ASSERT_TRUE(tx_handle != nullptr);
  sessionsHandleTx = tx_handle;
}

void St30pHandler::createRxSession() {
  ASSERT_TRUE(ctx && ctx->handle != nullptr);
  auto ops = sessionsOpsRx;

  st30p_rx_handle rx_handle = st30p_rx_create(ctx->handle, &ops);
  ASSERT_TRUE(rx_handle != nullptr);
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

    if (txTestFrameModifier) {
      txTestFrameModifier(frame, frame->data_size);
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

    if (rxTestFrameModifier) {
      rxTestFrameModifier(frame, frame->data_size);
    }

    st30p_rx_put_frame((st30p_rx_handle)handle, frame);
  }
}

void St30pHandler::rxSt30DefaultTimestampsCheck(void* frame,
                                                size_t frame_size) {
  st30_frame* f = (st30_frame*)frame;
  static uint64_t lastTimestamp = 0;
  static uint64_t framebuffTime = st10_tai_to_media_clk(nsPacketTime, clockHrtz);
  EXPECT_NEAR(f->timestamp, st10_tai_to_media_clk((sessionsUserData.idx_rx) * nsPacketTime, clockHrtz), framebuffTime) << " idx_rx: " << sessionsUserData.idx_rx;

  if (lastTimestamp != 0) {
    uint64_t diff = f->timestamp - lastTimestamp;
    EXPECT_TRUE(diff == framebuffTime) << " idx_rx: " << sessionsUserData.idx_rx;
  }

  lastTimestamp = f->timestamp;
  sessionsUserData.idx_rx++;
}

void St30pHandler::rxSt30DefaultUserPacingCheck(void* frame,
                                                size_t frame_size) {
  st30_frame* f = (st30_frame*)frame;
  static uint64_t startingTime = 10 * NS_PER_MS;
  static uint64_t lastTimestamp = 0;
  sessionsUserData.idx_rx++;

  uint64_t expectedTimestamp = startingTime + (nsPacketTime * (sessionsUserData.idx_rx - 1));
  uint64_t expected_media_clk = st10_tai_to_media_clk(expectedTimestamp, 48000);

  EXPECT_EQ(f->timestamp, expected_media_clk) << " idx_rx: " << sessionsUserData.idx_rx;

  if (lastTimestamp != 0) {
    uint64_t diff = f->timestamp - lastTimestamp;
    EXPECT_TRUE(diff == st10_tai_to_media_clk(nsPacketTime, 48000)) << " idx_rx: " << sessionsUserData.idx_rx;
  }

  lastTimestamp = f->timestamp;
}

void St30pHandler::txSt30DefaultUserPacing(void* frame,
                                           size_t frame_size) {
  st30_frame* f = (st30_frame*)frame;
  static uint64_t startingTime = 10 * NS_PER_MS;

  f->tfmt = ST10_TIMESTAMP_FMT_TAI;
  f->timestamp = startingTime + (nsPacketTime * (sessionsUserData.idx_tx));
  sessionsUserData.idx_tx++;
}

void St30pHandler::startSession() {
  Handlers::startSession({
    [this](std::atomic<bool>& stopFlag) { this->st30pTxDefaultFunction(stopFlag);},
    [this](std::atomic<bool>& stopFlag) { this->st30pRxDefaultFunction(stopFlag); }
  });
}

void St30pHandler::startSession(std::vector<std::function<void(std::atomic<bool>&)>> threadFunctions) {
  Handlers::startSession(threadFunctions);
}

