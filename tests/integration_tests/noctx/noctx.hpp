/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <thread>
#include <vector>

#include "../tests.hpp"

#define SESSION_SKIP_PORT -1
#define VIDEO_CLOCK_HZ 90000

class Session;
class FrameTestStrategy;
class NoCtxTest;
class St30pHandler;
class St20pHandler;
class Handlers;

/* Session class represents a media session that can run multiple threads */
class Session {
 public:
  /* can be more than one thread */
  std::vector<std::thread> threads;
  /* shoudnt be more than one */
  std::atomic<bool> stopFlag{false};

  void addThread(std::function<void(std::atomic<bool>&)> func) {
    threads.emplace_back(func, std::ref(stopFlag));
  }

  bool isRunning() const {
    for (const auto& thread : threads) {
      if (!thread.joinable()) {
        return false;
      }
    }

    return !threads.empty();
  }

  void stop() {
    stopFlag = true;
    for (auto& thread : threads) {
      if (thread.joinable()) {
        thread.join();
      }
    }
    threads.clear();
  }

  ~Session() {
    stop();
  }
};

class NoCtxTest : public ::testing::Test {
 protected:
  struct st_tests_context* ctx = nullptr;

  void SetUp() override;
  void TearDown() override;

 public:
  uint defaultTestDuration;
  static uint64_t TestPtpSourceSinceEpoch(void* priv);
  /* Structures that will be cleaned automaticly every test */
  std::vector<St30pHandler*> st30pHandlers;
  std::vector<St20pHandler*> st20pHandlers;
  std::vector<FrameTestStrategy*> sessionUserDatas;

  void sleepUntilFailure(int sleepDuration = 0);
};

class Handlers {
 public:
  Session session;
  st_tests_context* ctx = nullptr;
  FrameTestStrategy* sessionUserData;

  Handlers(st_tests_context* ctx, FrameTestStrategy* sessionUserData)
      : ctx(ctx), sessionUserData(sessionUserData) {
  }

  Handlers(st_tests_context* ctx) : ctx(ctx), sessionUserData(nullptr) {
  }

  void startSession(
      std::vector<std::function<void(std::atomic<bool>&)>> threadFunctions = {}) {
    for (auto& func : threadFunctions) {
      session.addThread(func);
    }
  }

  void stopSession() {
    session.stop();
  }

  void setSessionPortsRx(struct st_rx_port* port, int rxPortIdx, int rxPortRedundantIdx);
  void setSessionPortsTx(struct st_tx_port* port, int txPortIdx, int txPortRedundantIdx);

  ~Handlers() {
    session.stop();
  }
};

class FrameTestStrategy {
 public:
  Handlers* parent;
  uint32_t idx_tx;
  uint32_t idx_rx;
  double expect_fps;
  bool enable_tx_modifier = false;
  bool enable_rx_modifier = false;

  virtual void txTestFrameModifier(void* frame, size_t frame_size){};
  virtual void rxTestFrameModifier(void* frame, size_t frame_size){};

  virtual ~FrameTestStrategy() {
    parent = nullptr;
  }
};

class St30pHandler : public Handlers {
 private:
  uint msPerFramebuffer;

 public:
  uint64_t nsPacketTime;
  St30pHandler(st_tests_context* ctx, FrameTestStrategy* sessionUserData,
               st30p_tx_ops ops_tx = {}, st30p_rx_ops ops_rx = {},
               uint msPerFramebuffer = 10, bool create = true, bool start = true);

  St30pHandler(st_tests_context* ctx, st30p_tx_ops ops_tx = {}, st30p_rx_ops ops_rx = {},
               uint msPerFramebuffer = 10);
  ~St30pHandler();

  struct st30p_tx_ops sessionsOpsTx;
  struct st30p_rx_ops sessionsOpsRx;
  st30p_tx_handle sessionsHandleTx = nullptr;
  st30p_rx_handle sessionsHandleRx = nullptr;

  void fillSt30pOps(uint transmissionPort = 30000, uint framebufferQueueSize = 3,
                    uint payloadType = 111, st30_fmt format = ST30_FMT_PCM16,
                    st30_sampling sampling = ST30_SAMPLING_48K, uint8_t channelCount = 2,
                    st30_ptime ptime = ST30_PTIME_1MS);

  void setModifiers(FrameTestStrategy* sessionUserData) {
    this->sessionUserData = sessionUserData;
    sessionUserData->parent = this;
  }

  void createSession(st30p_tx_ops ops_tx, st30p_rx_ops ops_rx, bool start = true);
  void createSession(bool start = true);
  void createSessionTx();
  void createSessionRx();

  void startSession(std::vector<std::function<void(std::atomic<bool>&)>> threadFunctions);
  void startSession();
  void startSessionTx();
  void startSessionRx();

  void st30pTxDefaultFunction(std::atomic<bool>& stopFlag);
  void st30pRxDefaultFunction(std::atomic<bool>& stopFlag);
  void setSessionPorts(int txPortIdx = SESSION_SKIP_PORT,
                       int rxPortIdx = SESSION_SKIP_PORT,
                       int txPortRedundantIdx = SESSION_SKIP_PORT,
                       int rxPortRedundantIdx = SESSION_SKIP_PORT);
};

class St20pHandler : public Handlers {
 public:
  uint64_t nsFrameTime;
  St20pHandler(st_tests_context* ctx, FrameTestStrategy* sessionUserData,
              st20p_tx_ops ops_tx = {}, st20p_rx_ops ops_rx = {},
              bool create = true, bool start = true);

  St20pHandler(st_tests_context* ctx, st20p_tx_ops ops_tx = {}, st20p_rx_ops ops_rx = {});
  ~St20pHandler();

  struct st20p_tx_ops sessionsOpsTx;
  struct st20p_rx_ops sessionsOpsRx;
  st20p_tx_handle sessionsHandleTx = nullptr;
  st20p_rx_handle sessionsHandleRx = nullptr;

  void fillSt20Ops(uint transmissionPort = 20000, uint framebufferQueueSize = 3,
                    enum st20_fmt fmt = ST20_FMT_YUV_422_10BIT, uint width = 1920,
                    uint height = 1080, uint payloadType = 112, enum st_fps fps = ST_FPS_P25,
                   bool interlaced = false, enum st20_packing packing = ST20_PACKING_BPM);

  void setModifiers(FrameTestStrategy* sessionUserData) {
    this->sessionUserData = sessionUserData;
    sessionUserData->parent = this;
  }

  void createSession(st20p_tx_ops ops_tx, st20p_rx_ops ops_rx, bool start = true);
  void createSession(bool start = true);
  void createSessionTx();
  void createSessionRx();

  void startSession(std::vector<std::function<void(std::atomic<bool>&)>> threadFunctions);
  void startSession();
  void startSessionTx();
  void startSessionRx();

  void st20TxDefaultFunction(std::atomic<bool>& stopFlag);
  void st20RxDefaultFunction(std::atomic<bool>& stopFlag);
  void setSessionPorts(int txPortIdx = SESSION_SKIP_PORT,
                       int rxPortIdx = SESSION_SKIP_PORT,
                       int txPortRedundantIdx = SESSION_SKIP_PORT,
                       int rxPortRedundantIdx = SESSION_SKIP_PORT);
};
