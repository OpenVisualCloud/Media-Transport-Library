/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include "noctx.hpp"
#define TEST_CASE_WITH_48KHZ 2

/* Starts a thread as a part of session if it doesn't exists creates the session itself */
int SessionManager::startAsPartOfSession(int id, std::function<void(std::atomic<bool>&)> func,
                                 void* user_data) {
  auto session = std::make_unique<TransmissionThread>(id, func, user_data);

  sessionGroups[id].push_back(std::move(session));

  auto& session_ref = *sessionGroups[id].back();
  session_ref.thread = std::thread([func, &session_ref]() { 
    func(session_ref.stop_flag); 
  });
  
  return id;
}

/* returns 0 if there are no session with this id */
int SessionManager::stopSession(int id) {
  int unjoinableThreads = 0;

  auto group_it = sessionGroups.find(id);
  if (group_it == sessionGroups.end()) {
    return 0;
  }

  auto& sessions = group_it->second;
  int stopped_count = sessions.size();

  for (auto& session : sessions) {
    session->stop_flag = true;
    if (session->thread.joinable()) {
      session->thread.join();
    } else {
      unjoinableThreads++;
    }
  }

  sessionGroups.erase(group_it);

  if (unjoinableThreads) {
    return -unjoinableThreads;
  }

  return stopped_count;
}

void SessionManager::stopAll() {
  for (auto& [id, sessions] : sessionGroups) {
    for (auto& session : sessions) {
      session->stop_flag = true;
      if (session->thread.joinable()) {
        session->thread.join();
      }
    }
  }
  sessionGroups.clear();
}

void* SessionManager::getUserData(int id) {
  auto group_it = sessionGroups.find(id);
  if (group_it != sessionGroups.end() && !group_it->second.empty()) {
    return group_it->second[0]->user_data;  // Return first session's data
  }
  return nullptr;
}

bool SessionManager::isRunning(int id) {
  auto group_it = sessionGroups.find(id);
  if (group_it == sessionGroups.end()) return false;

  for (const auto& session : group_it->second) {
    if (!session->stop_flag.load()) {
      return true;
    }
  }
  return false;
}

size_t SessionManager::getSessionCount() const {
  size_t total = 0;
  for (const auto& [id, sessions] : sessionGroups) {
    total += sessions.size();
  }
  return total;
}

void NoCtxTest::SetUp() {
  ctx = new struct st_tests_context;

  /* NOCTX test: always operate on a copy of the global ctx.
     Do not use the global ctx directly for anything except copying its values.
   */
  ASSERT_TRUE(ctx != nullptr);
  memcpy(ctx, st_test_ctx(), sizeof(*ctx));

  ctx->level = ST_TEST_LEVEL_MANDATORY;
  ctx->para.flags |= MTL_FLAG_RANDOM_SRC_PORT;
  ctx->para.log_level = MTL_LOG_LEVEL_INFO;
  ctx->para.priv = ctx;
  ctx->para.tx_queues_cnt[MTL_PORT_P] = 16;
  ctx->para.tx_queues_cnt[MTL_PORT_R] = 16;
  ctx->para.rx_queues_cnt[MTL_PORT_P] = 16;
  ctx->para.rx_queues_cnt[MTL_PORT_R] = 16;
  defaultTestDuration = 20;
}

void NoCtxTest::TearDown() {
  stopAllManagedSessions();
  st30pHandlers.clear();

  if (ctx) {
    if (ctx->handle) {
      mtl_uninit(ctx->handle);
      /* WA for reinitialization issues */
      sleep(10);
    }

    delete ctx;
    ctx = nullptr;
  }
}

int NoCtxTest::startManagedSession(int index, std::function<void(std::atomic<bool>&)> func,
                                 void* user_data) {
  return session_manager.startAsPartOfSession(index, func, user_data);
}

int NoCtxTest::startManagedSession(int index, std::function<void(std::atomic<bool>&)> func_tx,
                                 std::function<void(std::atomic<bool>&)> func_rx,
                                 void* user_data) {
  int ret = session_manager.startAsPartOfSession(index, func_tx, user_data);
  if (ret < 0) return ret;

  ret = session_manager.startAsPartOfSession(index, func_rx, user_data);
  return ret;
}

/* create ptp time that will set the time to 0 */
uint64_t NoCtxTest::TestPtpSourceSinceEpoch(void* priv) {
  struct timespec spec;
  static std::atomic<uint64_t> adjustment_ns{0};

  if (adjustment_ns.load() == 0 || priv == nullptr) {
    struct timespec spec_adjustment_to_epoch;
    clock_gettime(CLOCK_MONOTONIC, &spec_adjustment_to_epoch);
    uint64_t temp_adjustment = (uint64_t)spec_adjustment_to_epoch.tv_sec * NS_PER_S +
                               spec_adjustment_to_epoch.tv_nsec;
    uint64_t expected = 0;
    adjustment_ns.compare_exchange_strong(expected, temp_adjustment);
  }

  clock_gettime(CLOCK_MONOTONIC, &spec);
  uint64_t result =
      ((uint64_t)spec.tv_sec * NS_PER_S + spec.tv_nsec) - adjustment_ns.load();
  return result;
}

St30pHandler::St30pHandler(uint sessionIdx, st_tests_context* ctx) {
  samplingModesDefault = {ST31_SAMPLING_44K, ST30_SAMPLING_96K, ST30_SAMPLING_48K};
  ptimeModesDefault = {ST31_PTIME_1_09MS, ST30_PTIME_125US, ST30_PTIME_1MS};
  channelCountsDefault = {3, 5, 7};
  fmtModesDefault = {ST31_FMT_AM824, ST30_FMT_PCM16, ST30_FMT_PCM24};
  transmissionPortDefault = 30000;
  payloadTypeDefault = 111;
  framebufferSizeDefault = 3;
  msPerFramebuffer = 10;
  sessionsUserData = {};

  this->sessionIdx = sessionIdx;
  this->ctx = ctx;

  /* we need at least 1 tx and 1 rx */
  fillDefaultSt30pTxOps(TEST_CASE_WITH_48KHZ);
  fillDefaultSt30pRxOps(TEST_CASE_WITH_48KHZ);
}

St30pHandler::~St30pHandler() {
  if (sessionsHandleTx) {
    st30p_tx_free(sessionsHandleTx);
  }

  if (sessionsHandleRx) {
    st30p_rx_free(sessionsHandleRx);
  }
}

st30p_tx_ops* St30pHandler::fillDefaultSt30pTxOps(int defaultValuesIdx) {
  memset(&sessionsOpsTx, 0, sizeof(sessionsOpsTx));
  sessionsOpsTx.name = "st30_noctx_test_tx";
  sessionsOpsTx.priv = ctx;
  sessionsOpsTx.port.num_port = 1;
  memcpy(sessionsOpsTx.port.dip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
         MTL_IP_ADDR_LEN);
  snprintf(sessionsOpsTx.port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
           ctx->para.port[MTL_PORT_P]);
  sessionsOpsTx.port.udp_port[MTL_SESSION_PORT_P] = transmissionPortDefault + sessionIdx;
  sessionsOpsTx.port.payload_type = payloadTypeDefault;

  sessionsOpsTx.fmt = fmtModesDefault[defaultValuesIdx % fmtModesDefault.size()];
  sessionsOpsTx.channel =
      channelCountsDefault[defaultValuesIdx % channelCountsDefault.size()];
  sessionsOpsTx.sampling =
      samplingModesDefault[defaultValuesIdx % samplingModesDefault.size()];
  sessionsOpsTx.ptime = ptimeModesDefault[defaultValuesIdx % ptimeModesDefault.size()];

  /* count frame size for 10ms */
  sessionsOpsTx.framebuff_size = st30_calculate_framebuff_size(
      sessionsOpsTx.fmt, sessionsOpsTx.ptime, sessionsOpsTx.sampling,
      sessionsOpsTx.channel, 10 * NS_PER_MS, nullptr);

  sessionsOpsTx.framebuff_cnt = framebufferSizeDefault;
  sessionsOpsTx.notify_frame_available = nullptr;

  return &sessionsOpsTx;
}

st30p_rx_ops* St30pHandler::fillDefaultSt30pRxOps(int defaultValuesIdx) {
  memset(&sessionsOpsRx, 0, sizeof(sessionsOpsRx));
  sessionsOpsRx.name = "st30_noctx_test_rx";
  sessionsOpsRx.priv = ctx;
  sessionsOpsRx.port.num_port = 1;
  memcpy(sessionsOpsRx.port.ip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
         MTL_IP_ADDR_LEN);
  snprintf(sessionsOpsRx.port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
           ctx->para.port[MTL_PORT_R]);
  sessionsOpsRx.port.udp_port[MTL_SESSION_PORT_P] = transmissionPortDefault + sessionIdx;
  sessionsOpsRx.port.payload_type = payloadTypeDefault;

  sessionsOpsRx.fmt = fmtModesDefault[defaultValuesIdx % fmtModesDefault.size()];
  sessionsOpsRx.channel =
      channelCountsDefault[defaultValuesIdx % channelCountsDefault.size()];
  sessionsOpsRx.sampling =
      samplingModesDefault[defaultValuesIdx % samplingModesDefault.size()];
  sessionsOpsRx.ptime = ptimeModesDefault[defaultValuesIdx % ptimeModesDefault.size()];

  /* count frame size for 10ms  */
  sessionsOpsRx.framebuff_size = st30_calculate_framebuff_size(
      sessionsOpsRx.fmt, sessionsOpsRx.ptime, sessionsOpsRx.sampling,
      sessionsOpsRx.channel, 10 * NS_PER_MS, nullptr);

  sessionsOpsRx.framebuff_cnt = framebufferSizeDefault;
  sessionsOpsRx.flags |= ST30P_RX_FLAG_BLOCK_GET;
  sessionsOpsRx.notify_frame_available = nullptr;

  uint64_t totalPackets =
      sessionsOpsRx.framebuff_size /
      st30_get_packet_size(sessionsOpsRx.fmt, sessionsOpsRx.ptime, sessionsOpsRx.sampling,
                           sessionsOpsRx.channel);

  uint64_t framesPerSec =
      (double)NS_PER_S / st30_get_packet_time(sessionsOpsRx.ptime) / totalPackets;

  nsPacketTime = NS_PER_S / framesPerSec;
  return &sessionsOpsRx;
}

void St30pHandler::createSession(st30p_tx_ops ops_tx, st30p_rx_ops ops_rx) {
  st30p_tx_ops zero_tx_ops = {};
  st30p_rx_ops zero_rx_ops = {};

  /* check if deafult values should be used */
  if (memcmp(&ops_tx, &zero_tx_ops, sizeof(st30p_tx_ops)) != 0) {
    sessionsOpsTx = ops_tx;
  }

  if (memcmp(&ops_rx, &zero_rx_ops, sizeof(st30p_rx_ops)) != 0) {
    sessionsOpsRx = ops_rx;
  }

  createTxSession();
  createRxSession();
}

/* NOT THREAD SAFE, DON'T call from more than one thread */
void St30pHandler::createTxSession() {
  ASSERT_TRUE(ctx && ctx->handle != nullptr);
  auto ops = sessionsOpsTx;

  st30p_tx_handle tx_handle = st30p_tx_create(ctx->handle, &ops);
  ASSERT_TRUE(tx_handle != nullptr);
  ASSERT_TRUE(sessionsHandleTx == nullptr);

  sessionsHandleTx = tx_handle;
}

/* NOT THREAD SAFE, DON'T call from more than one thread */
void St30pHandler::createRxSession() {
  ASSERT_TRUE(ctx && ctx->handle != nullptr);
  auto ops = sessionsOpsRx;

  st30p_rx_handle rx_handle = st30p_rx_create(ctx->handle, &ops);
  ASSERT_TRUE(rx_handle != nullptr);
  ASSERT_TRUE(sessionsHandleRx == nullptr);
  sessionsHandleRx = rx_handle;
}

void St30pHandler::st30pTxDefaultFunction(int sessionIdx, std::atomic<bool>& stop_flag) {
  struct st30_frame* frame;
  st30p_tx_handle handle = sessionsHandleTx;
  ASSERT_TRUE(handle != nullptr);

  while (!stop_flag) {
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
      txTestFrameModifier(sessionIdx, frame, frame->data_size);
    }

    st30p_tx_put_frame((st30p_tx_handle)handle, frame);
  }
}

void St30pHandler::st30pRxDefaultFunction(int sessionIdx, std::atomic<bool>& stop_flag) {
  struct st30_frame* frame;
  st30p_rx_handle handle = sessionsHandleRx;
  ASSERT_TRUE(handle != nullptr);

  while (!stop_flag) {
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
      rxTestFrameModifier(sessionIdx, frame, frame->data_size);
    }

    st30p_rx_put_frame((st30p_rx_handle)handle, frame);
  }
}

/* NOT THREAD SAFE, DON'T call from more than one thread */
void St30pHandler::rxSt30DefaultTimestampsCheck(int sessionIdx, void* frame,
                                                size_t frame_size) {
  st30_frame* f = (st30_frame*)frame;
  static uint64_t last_timestamp = 0;
  EXPECT_NEAR(st10_media_clk_to_ns(f->timestamp, 48000),
              (sessionsUserData.idx_rx) * nsPacketTime, (nsPacketTime) / 5);

  if (last_timestamp != 0) {
    uint64_t diff = f->timestamp - last_timestamp;
    EXPECT_TRUE(diff == st10_tai_to_media_clk(nsPacketTime, 48000));
  }

  last_timestamp = f->timestamp;
  sessionsUserData.idx_rx++;
}

TEST_F(NoCtxTest, st30p_default_timestamps) {
  int sessionIdx = 0;

  /* This tests makes mtl start from 0 */
  ctx->para.ptp_get_time_fn = NoCtxTest::TestPtpSourceSinceEpoch;
  ctx->para.log_level = MTL_LOG_LEVEL_INFO;

  ASSERT_TRUE(ctx && ctx->handle == nullptr);
  ctx->handle = mtl_init(&ctx->para);
  ASSERT_TRUE(ctx->handle != nullptr);

  st30pHandlers.emplace_back(std::make_unique<St30pHandler>(sessionIdx, ctx));
  auto& handler = st30pHandlers[sessionIdx];
  handler->createSession();

  handler->rxTestFrameModifier = [this, sessionIdx](int idx, void* frame,
                                                    size_t frame_size) {
    this->st30pHandlers[sessionIdx]->rxSt30DefaultTimestampsCheck(idx, frame, frame_size);
  };

  TestPtpSourceSinceEpoch(nullptr);


  for (int i = 0; i < defaultTestDuration; ++i) {
    if (HasFailure()) break;
    sleep(1);
  }

  mtl_stop(ctx->handle);
}

void St30pHandler::rxSt30DefaultUserPacingCheck(int sessionIdx, void* frame,
                                                size_t frame_size) {
  st30_frame* f = (st30_frame*)frame;
  static uint64_t startingTime = 10 * NS_PER_MS;
  static uint64_t last_timestamp = 0;
  sessionsUserData.idx_rx++;

  uint64_t expectedTimestamp = startingTime + (nsPacketTime * (sessionsUserData.idx_rx - 1));
  uint64_t expected_media_clk = st10_tai_to_media_clk(expectedTimestamp, 48000);

  EXPECT_EQ(f->timestamp, expected_media_clk);

  if (last_timestamp != 0) {
    uint64_t diff = f->timestamp - last_timestamp;
    EXPECT_TRUE(diff == st10_tai_to_media_clk(nsPacketTime, 48000));
  }

  last_timestamp = f->timestamp;
}

void St30pHandler::txSt30DefaultUserPacing(int sessionIdx, void* frame,
                                           size_t frame_size) {
  st30_frame* f = (st30_frame*)frame;
  static uint64_t startingTime = 10 * NS_PER_MS;

  f->tfmt = ST10_TIMESTAMP_FMT_TAI;
  f->timestamp = startingTime + (nsPacketTime * (sessionsUserData.idx_tx));
  sessionsUserData.idx_tx++;
}

TEST_F(NoCtxTest, st30p_user_pacing) {
  int sessionIdx = 0;

  /* This tests makes mtl start from 0 */
  ctx->para.ptp_get_time_fn = NoCtxTest::TestPtpSourceSinceEpoch;
  ctx->para.log_level = MTL_LOG_LEVEL_INFO;

  ASSERT_TRUE(ctx && ctx->handle == nullptr);
  ctx->handle = mtl_init(&ctx->para);
  ASSERT_TRUE(ctx->handle != nullptr);

  st30pHandlers.emplace_back(std::make_unique<St30pHandler>(sessionIdx, ctx));
  auto& handler = st30pHandlers[sessionIdx];
  handler->sessionsOpsTx.flags |= ST30P_TX_FLAG_USER_PACING;

  handler->createSession();

  handler->txTestFrameModifier = [this, sessionIdx](int idx, void* frame,
                                                    size_t frame_size) {
    this->st30pHandlers[sessionIdx]->txSt30DefaultUserPacing(idx, frame, frame_size);
  };

  handler->rxTestFrameModifier = [this, sessionIdx](int idx, void* frame,
                                                    size_t frame_size) {
    this->st30pHandlers[sessionIdx]->rxSt30DefaultUserPacingCheck(idx, frame, frame_size);
  };

  uint64_t packet_time_ns = st30_get_packet_time(handler->sessionsOpsTx.ptime);
  handler->sessionsUserData.expect_fps = (double)NS_PER_S / packet_time_ns;

  TestPtpSourceSinceEpoch(nullptr);

  startManagedSession(
    sessionIdx,
    [&, sessionIdx](std::atomic<bool>& stop_flag) {
      handler->st30pTxDefaultFunction(sessionIdx, stop_flag);
    },
    [&, sessionIdx](std::atomic<bool>& stop_flag) {
      handler->st30pRxDefaultFunction(sessionIdx, stop_flag);
    },
    &handler->sessionsUserData
  );

  ASSERT_EQ(session_manager.getSessionCount(), 2);

  for (int i = 0; i < defaultTestDuration; ++i) {
    if (HasFailure()) break;
    sleep(1);
  }

  mtl_stop(ctx->handle);
}
