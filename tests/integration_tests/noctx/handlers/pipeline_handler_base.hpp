/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright(c) 2025 Intel Corporation */

#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <utility>

#include "core/constants.hpp"
#include "core/handler_base.hpp"

/**
 * @brief Generic helper for handlers that manage paired TX/RX pipeline sessions.
 */
template <typename TxOps, typename RxOps, typename TxHandle, typename RxHandle>
class PipelineHandlerBase : public Handlers {
 public:
  using TxCreateFn = TxHandle (*)(mtl_handle, TxOps*);
  using RxCreateFn = RxHandle (*)(mtl_handle, RxOps*);
  using TxFreeFn = int (*)(TxHandle);
  using RxFreeFn = int (*)(RxHandle);

  PipelineHandlerBase(st_tests_context* ctx, FrameTestStrategy* frameTestStrategy,
                      TxCreateFn txCreate, RxCreateFn rxCreate, TxFreeFn txFree,
                      RxFreeFn rxFree)
      : Handlers(ctx, frameTestStrategy),
        txCreateFn_(txCreate),
        rxCreateFn_(rxCreate),
        txFreeFn_(txFree),
        rxFreeFn_(rxFree) {
  }

  virtual ~PipelineHandlerBase() {
    Handlers::stopSession();  // ensure worker threads exit before freeing handles
    releaseTxHandle();
    releaseRxHandle();
  }

  TxOps sessionsOpsTx{};
  RxOps sessionsOpsRx{};
  TxHandle sessionsHandleTx = nullptr;
  RxHandle sessionsHandleRx = nullptr;

  void setFrameTestStrategy(FrameTestStrategy* newStrategy) {
    frameTestStrategy = newStrategy;
    if (frameTestStrategy) {
      frameTestStrategy->parent = this;
    }
  }

  uint32_t txFrames() const {
    return txFrameCount_.load(std::memory_order_relaxed);
  }

  uint32_t rxFrames() const {
    return rxFrameCount_.load(std::memory_order_relaxed);
  }

  void setSessionPorts(int txPortIdx = SESSION_SKIP_PORT,
                       int rxPortIdx = SESSION_SKIP_PORT,
                       int txPortRedundantIdx = SESSION_SKIP_PORT,
                       int rxPortRedundantIdx = SESSION_SKIP_PORT) {
    setSessionPortsTx(&(this->sessionsOpsTx.port), txPortIdx, txPortRedundantIdx);
    setSessionPortsRx(&(this->sessionsOpsRx.port), rxPortIdx, rxPortRedundantIdx);
  }

  void createSession(TxOps ops_tx, RxOps ops_rx, bool start = true) {
    sessionsOpsTx = ops_tx;
    sessionsOpsRx = ops_rx;

    resetFrameCounters();
    createSessionTx();
    createSessionRx();

    if (start) {
      startSession();
    }
  }

  void createSession(bool start = true) {
    resetFrameCounters();
    createSessionTx();
    createSessionRx();

    if (start) {
      startSession();
    }
  }

  void createSessionTx() {
    ASSERT_TRUE(ctx && ctx->handle != nullptr);
    releaseTxHandle();

    auto ops = sessionsOpsTx;
    TxHandle handle = txCreateFn_(ctx->handle, &ops);
    EXPECT_TRUE(handle != nullptr);
    sessionsHandleTx = handle;
  }

  void createSessionRx() {
    ASSERT_TRUE(ctx && ctx->handle != nullptr);
    releaseRxHandle();

    auto ops = sessionsOpsRx;
    RxHandle handle = rxCreateFn_(ctx->handle, &ops);
    EXPECT_TRUE(handle != nullptr);
    sessionsHandleRx = handle;
  }

  void startSession() {
    startSessionRx();
    startSessionTx();
  }

  virtual void startSessionTx() = 0;
  virtual void startSessionRx() = 0;

 protected:
  void startTxThread(std::function<void(std::atomic<bool>&)> threadFn) {
    Handlers::startSession({std::move(threadFn)}, /*isRx=*/false);
  }

  void startRxThread(std::function<void(std::atomic<bool>&)> threadFn) {
    Handlers::startSession({std::move(threadFn)}, /*isRx=*/true);
  }

  void applyTxModifier(void* frame, size_t size_bytes) {
    if (frameTestStrategy && frameTestStrategy->enable_tx_modifier) {
      frameTestStrategy->txTestFrameModifier(frame, size_bytes);
    }
  }

  void applyRxModifier(void* frame, size_t size_bytes) {
    if (frameTestStrategy && frameTestStrategy->enable_rx_modifier) {
      frameTestStrategy->rxTestFrameModifier(frame, size_bytes);
    }
  }

  void releaseTxHandle() {
    if (sessionsHandleTx) {
      txFreeFn_(sessionsHandleTx);
      sessionsHandleTx = nullptr;
    }
  }

  void releaseRxHandle() {
    if (sessionsHandleRx) {
      rxFreeFn_(sessionsHandleRx);
      sessionsHandleRx = nullptr;
    }
  }

  void recordTxFrame() {
    txFrameCount_.fetch_add(1, std::memory_order_relaxed);
  }

  void recordRxFrame() {
    rxFrameCount_.fetch_add(1, std::memory_order_relaxed);
  }

  void resetFrameCounters() {
    txFrameCount_.store(0, std::memory_order_relaxed);
    rxFrameCount_.store(0, std::memory_order_relaxed);
  }

 private:
  TxCreateFn txCreateFn_;
  RxCreateFn rxCreateFn_;
  TxFreeFn txFreeFn_;
  RxFreeFn rxFreeFn_;

  std::atomic<uint32_t> txFrameCount_{0};
  std::atomic<uint32_t> rxFrameCount_{0};
};
