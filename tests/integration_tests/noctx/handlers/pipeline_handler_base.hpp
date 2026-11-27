/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright(c) 2025 Intel Corporation */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
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
  using TxWakeFn = int (*)(TxHandle);
  using RxWakeFn = int (*)(RxHandle);

  PipelineHandlerBase(st_tests_context* ctx, FrameTestStrategy* frameTestStrategy,
                      TxCreateFn txCreate, RxCreateFn rxCreate, TxFreeFn txFree,
                      RxFreeFn rxFree, TxWakeFn txWake = nullptr,
                      RxWakeFn rxWake = nullptr)
      : Handlers(ctx, frameTestStrategy),
        txCreateFn_(txCreate),
        rxCreateFn_(rxCreate),
        txFreeFn_(txFree),
        rxFreeFn_(rxFree),
        txWakeFn_(txWake),
        rxWakeFn_(rxWake) {
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
    setSessionPortsImpl(&(this->sessionsOpsTx.port), txPortIdx, txPortRedundantIdx);
    setSessionPortsImpl(&(this->sessionsOpsRx.port), rxPortIdx, rxPortRedundantIdx);
  }

  void createSessionTx() {
    ASSERT_TRUE(ctx && ctx->handle != nullptr);
    releaseTxHandle();

    auto ops = sessionsOpsTx;
    TxHandle handle = txCreateFn_(ctx->handle, &ops);
    EXPECT_TRUE(handle != nullptr);
    sessionsHandleTx = handle;
    if (handle && txWakeFn_) {
      session.setTxWakeCallback([fn = txWakeFn_, handle]() { (void)fn(handle); });
    }
  }

  void createSessionRx() {
    ASSERT_TRUE(ctx && ctx->handle != nullptr);
    releaseRxHandle();

    auto ops = sessionsOpsRx;
    RxHandle handle = rxCreateFn_(ctx->handle, &ops);
    EXPECT_TRUE(handle != nullptr);
    sessionsHandleRx = handle;
    if (handle && rxWakeFn_) {
      session.setRxWakeCallback([fn = rxWakeFn_, handle]() { (void)fn(handle); });
    }
  }

  void startSession() {
    startSessionRx();
    startSessionTx();
  }

  virtual void startSessionTx() = 0;
  virtual void startSessionRx() = 0;

 protected:
  void initDefaultPorts(const char* txName, const char* rxName, uint transmissionPort,
                        uint payloadType) {
    std::memset(&sessionsOpsTx, 0, sizeof(sessionsOpsTx));
    sessionsOpsTx.name = txName;
    sessionsOpsTx.priv = ctx;
    sessionsOpsTx.port.num_port = 1;
    std::memcpy(sessionsOpsTx.port.dip_addr[MTL_SESSION_PORT_P],
                ctx->mcast_ip_addr[MTL_PORT_P], MTL_IP_ADDR_LEN);
    std::memcpy(sessionsOpsTx.port.dip_addr[MTL_SESSION_PORT_R],
                ctx->mcast_ip_addr[MTL_PORT_R], MTL_IP_ADDR_LEN);
    snprintf(sessionsOpsTx.port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
             ctx->para.port[MTL_PORT_P]);
    sessionsOpsTx.port.udp_port[MTL_SESSION_PORT_P] = transmissionPort;
    sessionsOpsTx.port.udp_port[MTL_SESSION_PORT_R] = transmissionPort + 1;
    sessionsOpsTx.port.payload_type = payloadType;

    std::memset(&sessionsOpsRx, 0, sizeof(sessionsOpsRx));
    sessionsOpsRx.name = rxName;
    sessionsOpsRx.priv = ctx;
    sessionsOpsRx.port.num_port = 1;
    std::memcpy(sessionsOpsRx.port.ip_addr[MTL_SESSION_PORT_P],
                ctx->mcast_ip_addr[MTL_PORT_P], MTL_IP_ADDR_LEN);
    std::memcpy(sessionsOpsRx.port.ip_addr[MTL_SESSION_PORT_R],
                ctx->mcast_ip_addr[MTL_PORT_R], MTL_IP_ADDR_LEN);
    snprintf(sessionsOpsRx.port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
             ctx->para.port[MTL_PORT_R]);
    sessionsOpsRx.port.udp_port[MTL_SESSION_PORT_P] = transmissionPort;
    sessionsOpsRx.port.udp_port[MTL_SESSION_PORT_R] = transmissionPort + 1;
    sessionsOpsRx.port.payload_type = payloadType;
  }

  void startTxThread(std::function<void(std::atomic<bool>&)> threadFn) {
    session.addThread(std::move(threadFn), /*isRx=*/false);
  }

  void startRxThread(std::function<void(std::atomic<bool>&)> threadFn) {
    session.addThread(std::move(threadFn), /*isRx=*/true);
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
    session.setTxWakeCallback(nullptr);
    if (sessionsHandleTx) {
      txFreeFn_(sessionsHandleTx);
      sessionsHandleTx = nullptr;
    }
  }

  void releaseRxHandle() {
    session.setRxWakeCallback(nullptr);
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

 private:
  TxCreateFn txCreateFn_;
  RxCreateFn rxCreateFn_;
  TxFreeFn txFreeFn_;
  RxFreeFn rxFreeFn_;
  TxWakeFn txWakeFn_;
  RxWakeFn rxWakeFn_;

  std::atomic<uint32_t> txFrameCount_{0};
  std::atomic<uint32_t> rxFrameCount_{0};
};
