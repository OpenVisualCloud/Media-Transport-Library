/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include <gtest/gtest.h>

#include <thread>
#include <vector>
#include <map>
#include <memory>

#include "tests.hpp"

#define AUDIO_CLOCK_HRTZ 48000
#define VIDEO_CLOCK_HRTZ 90000

class Session;
class SessionUserData;
class NoCtxTest;
class St30pHandler;
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
  std::vector<SessionUserData*> sessionUserDatas;

  void sleepUntilFailure(int sleepDuration=0);
};



class Handlers {
 public:
  Session session;
  st_tests_context* ctx = nullptr;
  uint clockHrtz;
  SessionUserData* sessionUserData;

  Handlers(st_tests_context* ctx, SessionUserData* sessionUserData, uint clockHrz)
    : ctx(ctx), clockHrtz(clockHrz), sessionUserData(sessionUserData) {}
  
  Handlers(st_tests_context* ctx, uint clockHrz)
    : ctx(ctx), clockHrtz(clockHrz), sessionUserData(nullptr) {}

  void setModifiers(SessionUserData* sessionUserData) {
    this->sessionUserData = sessionUserData;
  }

  void startSession(std::vector<std::function<void(std::atomic<bool>&)>> threadFunctions = {}) {
      for (auto& func : threadFunctions) {
        session.addThread(func);
      }
  }

  void stopSession() {
    session.stop();
  }

  ~Handlers() {
    session.stop();
  }
};

class SessionUserData {
  public:
    Handlers* parent;
    uint32_t idx_tx;
    uint32_t idx_rx;
    double expect_fps;
    bool enable_tx_modifier = false;
    bool enable_rx_modifier = false;

  virtual void txTestFrameModifier(void* frame, size_t frame_size) {};
  virtual void rxTestFrameModifier(void* frame, size_t frame_size) {};

  virtual ~SessionUserData() {
    parent = nullptr;
  }
};

class St30pHandler : public Handlers {
 private:
  uint msPerFramebuffer;

 public:
  uint nsPacketTime;
  St30pHandler(st_tests_context* ctx,
               SessionUserData* sessionUserData,
               st30p_tx_ops ops_tx = {},
               st30p_rx_ops ops_rx = {},
               uint msPerFramebuffer = 10);

  St30pHandler(st_tests_context* ctx,
               st30p_tx_ops ops_tx = {},
               st30p_rx_ops ops_rx = {},
               uint msPerFramebuffer = 10);
  ~St30pHandler();

  struct st30p_tx_ops sessionsOpsTx;
  struct st30p_rx_ops sessionsOpsRx;
  st30p_tx_handle sessionsHandleTx;
  st30p_rx_handle sessionsHandleRx;

  void fillSt30pOps(
        uint transmissionPort = 30000,
        uint framebufferQueueSize = 3,
        uint payloadType = 111,
        st30_fmt format = ST30_FMT_PCM16,
        st30_sampling sampling = ST30_SAMPLING_48K,
        uint8_t channelCount = 2,
        st30_ptime ptime = ST30_PTIME_1MS
  );

  void createSession(st30p_tx_ops ops_tx, st30p_rx_ops ops_rx, bool start = true);
  void createSession(bool start = true);
  void createTxSession();
  void createRxSession();

  void startSession(std::vector<std::function<void(std::atomic<bool>&)>> threadFunctions);
  void startSession();

  void st30pTxDefaultFunction(std::atomic<bool>& stopFlag);
  void st30pRxDefaultFunction(std::atomic<bool>& stopFlag);
};
