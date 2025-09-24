/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include <gtest/gtest.h>

#include <thread>
#include <vector>
#include <map>
#include <memory>

#include "tests.hpp"

class Session;
class SessionManager;
class NoCtxTest;
class St30pHandler;


/* Common structure accessible by rx and tx session thread responsibility of the handlers is to make it thread safe */
struct sessionInfo {
  uint32_t idx_tx;

  uint32_t idx_rx;
  double expect_fps;
};

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
  std::vector<std::unique_ptr<St30pHandler>> st30pHandlers;

  void sleepUntilFailure(int sleepDuration=0);
};



class Handlers {
 private:
  Session session;

 public:
  st_tests_context* ctx = nullptr;
  sessionInfo sessionsUserData;
  std::function<void(void* frame, size_t frame_size)>
      txTestFrameModifier;
  std::function<void(void* frame, size_t frame_size)>
      rxTestFrameModifier;
  uint clockHrtz;

  Handlers(st_tests_context* ctx, std::function<void(void* frame, size_t frame_size)> txTestFrameModifier, std::function<void(void* frame, size_t frame_size)> rxTestFrameModifier, uint clockHrz) : ctx(ctx), sessionsUserData({}), txTestFrameModifier(txTestFrameModifier), rxTestFrameModifier(rxTestFrameModifier), clockHrtz(clockHrz) {}
  Handlers(st_tests_context* ctx, uint clockHrz) : ctx(ctx), sessionsUserData({}), txTestFrameModifier(nullptr), rxTestFrameModifier(nullptr), clockHrtz(clockHrz) {}
  void setModifiers(std::function<void(void* frame, size_t frame_size)> txTestFrameModifier, std::function<void(void* frame, size_t frame_size)> rxTestFrameModifier) {
    this->txTestFrameModifier = txTestFrameModifier;
    this->rxTestFrameModifier = rxTestFrameModifier;
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

class St30pHandler : public Handlers {
 private:
  uint msPerFramebuffer;

 public:
  uint nsPacketTime;
  St30pHandler(st_tests_context* ctx,
               std::function<void(void* frame, size_t frame_size)> txTestFrameModifier,
               std::function<void(void* frame, size_t frame_size)> rxTestFrameModifier,
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

  /* test modifier functions */
  void rxSt30DefaultTimestampsCheck(void* frame, size_t frame_size);

  void txSt30DefaultUserPacing(void* frame, size_t frame_size);
  void rxSt30DefaultUserPacingCheck(void* frame, size_t frame_size);
};
