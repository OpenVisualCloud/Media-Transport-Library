/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include <gtest/gtest.h>

#include <thread>
#include <vector>

#include "tests.hpp"

class SessionManager;
class NoCtxTest;
class St30pHandler;
class TransmissionThread;

/* Common structure accessible by rx and tx session thread responsibility of the handlers is to make it thread safe */
struct sessionInfo {
  uint32_t idx_tx;

  uint32_t idx_rx;
  double expect_fps;
};

/* Session class represents a single media session */
class TransmissionThread {
  public:
  int id;
  std::thread thread;
  std::atomic<bool> stop_flag{false};
  std::function<void(std::atomic<bool>&)> func;
  void* user_data;

  TransmissionThread(int session_id, std::function<void(std::atomic<bool>&)> f,
          void* data = nullptr)
      : id(session_id), func(f), user_data(data) {
  }
};

/* Creates a map of TransmissionThread vectors keyed by session id
   basically you can create as many connected TransmissionThreads as needed */
class SessionManager {
  std::unordered_map<int, std::vector<std::unique_ptr<TransmissionThread>>> sessionGroups;

 public:
  int startAsPartOfSession(int id, std::function<void(std::atomic<bool>&)> func,
                   void* user_data = nullptr);
  int stopSession(int id);
  void stopAll();
  void* getUserData(int id);
  bool isRunning(int id);
  size_t getSessionCount() const;

  ~SessionManager() {
    stopAll();
  }
};

class NoCtxTest : public ::testing::Test {
 protected:
  struct st_tests_context* ctx = nullptr;

  void SetUp() override;
  void TearDown() override;

  SessionManager session_manager;

 public:
  uint defaultTestDuration;
  static uint64_t TestPtpSourceSinceEpoch(void* priv);
  std::vector<std::unique_ptr<St30pHandler>> st30pHandlers;

  void txDefaultCheck(int session_idx, void* frame, size_t frame_size);
  void rxDefaultCheck(int session_idx, void* frame, size_t frame_size);

  void addSessions(int index, std::function<void(std::atomic<bool>&)> func);

  int startManagedSession(int index, std::function<void(std::atomic<bool>&)> func,
                          void* user_data = nullptr);

  int startManagedSession(int index, std::function<void(std::atomic<bool>&)> func_tx,
                          std::function<void(std::atomic<bool>&)> func_rx,
                          void* user_data = nullptr);

  int stopManagedSession(int index) {
    return session_manager.stopSession(index);
  }

  void stopAllManagedSessions() {
    session_manager.stopAll();
  }
};

class St30pHandler {
 private:
  uint sessionIdx;
  st_tests_context* ctx = nullptr;
  uint msPerFramebuffer;

 public:
  uint nsPacketTime;
  sessionInfo sessionsUserData;
  std::function<void(int session_idx, void* frame, size_t frame_size)>
      txTestFrameModifier;
  std::function<void(int session_idx, void* frame, size_t frame_size)>
      rxTestFrameModifier;
  St30pHandler(uint session_idx, st_tests_context* ctx);
  ~St30pHandler();
  uint transmissionPortDefault;
  uint payloadTypeDefault;
  uint framebufferSizeDefault;
  std::vector<enum st30_sampling> samplingModesDefault;
  std::vector<enum st30_ptime> ptimeModesDefault;
  std::vector<uint16_t> channelCountsDefault;
  std::vector<enum st30_fmt> fmtModesDefault;

  struct st30p_tx_ops sessionsOpsTx;
  struct st30p_rx_ops sessionsOpsRx;
  st30p_tx_handle sessionsHandleTx;
  st30p_rx_handle sessionsHandleRx;

  st30p_tx_ops* fillDefaultSt30pTxOps(int defaultValuesIdx = 0);
  st30p_rx_ops* fillDefaultSt30pRxOps(int defaultValuesIdx = 0);

  void createSession(st30p_tx_ops ops_tx = {}, st30p_rx_ops ops_rx = {});
  void startDefaultSession();
  void createTxSession();
  void createRxSession();

  void st30pTxDefaultFunction(int session_idx, std::atomic<bool>& stop_flag);
  void st30pRxDefaultFunction(int session_idx, std::atomic<bool>& stop_flag);

  /* test modifier functions */
  void rxSt30DefaultTimestampsCheck(int session_idx, void* frame, size_t frame_size);

  void txSt30DefaultUserPacing(int session_idx, void* frame, size_t frame_size);
  void rxSt30DefaultUserPacingCheck(int session_idx, void* frame, size_t frame_size);
};
