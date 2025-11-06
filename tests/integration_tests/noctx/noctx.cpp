/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include "noctx.hpp"

void NoCtxTest::SetUp() {
  ctx = new struct st_tests_context;

  /* NOCTX test: always operate on a copy of the global ctx.
     Do not use the global ctx directly for anything except copying its values.
   */
  if (!ctx) {
    throw std::runtime_error("NoCtxTest::SetUp no ctx");
  }

  memcpy(ctx, st_test_ctx(), sizeof(*ctx));

  if (ctx->handle) {
    throw std::runtime_error(
        "NoCtxTest::SetUp: ctx->handle is already initialized!\n"
        "This likely means the global context was not properly reset between tests.\n"
        "To run NOCTX tests, please use the '--no_ctx_tests' option to ensure a clean "
        "context.");
  }

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
  mtl_stop(ctx->handle);
  for (auto handler : st30pHandlers) {
    if (handler) {
      delete handler;
      handler = nullptr;
    }
  }
  st30pHandlers.clear();

  for (auto handler : st20pHandlers) {
    if (handler) {
      delete handler;
      handler = nullptr;
    }
  }
  st20pHandlers.clear();

  for (auto data : sessionUserDatas) {
    if (data) {
      delete data;
      data = nullptr;
    }
  }
  sessionUserDatas.clear();

  if (ctx) {
    if (ctx->handle) {
      mtl_uninit(ctx->handle);
      /* make sure the tests cannot be reinitialized */
      ctx->handle = (mtl_handle)0x1;
    }

    delete ctx;
    ctx = nullptr;
  }
}

/*
 * PTP time source that provides timestamps starting from 0.
 * Reset behavior: Pass nullptr as priv to reset the epoch to current time.
 * This allows tests to synchronize timing by calling TestPtpSourceSinceEpoch(nullptr)
 * before starting timed operations, ensuring all subsequent timestamps start from 0.
 */
uint64_t NoCtxTest::TestPtpSourceSinceEpoch(void* priv) {
  struct timespec spec;
  static std::atomic<uint64_t> adjustment_ns{0};

  if (adjustment_ns.load() == 0 || priv == nullptr) {
    struct timespec spec_adjustment_to_epoch;
    clock_gettime(CLOCK_MONOTONIC, &spec_adjustment_to_epoch);
    uint64_t temp_adjustment = (uint64_t)spec_adjustment_to_epoch.tv_sec * NS_PER_S +
                               spec_adjustment_to_epoch.tv_nsec;

    adjustment_ns.store(temp_adjustment);
  }

  clock_gettime(CLOCK_MONOTONIC, &spec);
  uint64_t result =
      ((uint64_t)spec.tv_sec * NS_PER_S + spec.tv_nsec) - adjustment_ns.load();

  return result;
}

void NoCtxTest::sleepUntilFailure(int sleep_duration) {
  if (!sleep_duration) {
    sleep_duration = defaultTestDuration;
  }

  for (int i = 0; i < sleep_duration; ++i) {
    if (HasFailure()) break;
    sleep(1);
  }
}

/**
 * @brief Set the TX session port names, including redundant port if specified.
 *
 * @param port Pointer to the st_tx_port struct to update.
 * @param txPortIdx Index for the primary TX port in ctx->para.port, or SESSION_SKIP_PORT
 * to skip.
 * @param txPortRedundantIdx Index for the redundant TX port in ctx->para.port, or
 * SESSION_SKIP_PORT to skip.
 */
void Handlers::setSessionPortsTx(struct st_tx_port* port, int txPortIdx,
                                 int txPortRedundantIdx) {
  if (!ctx) {
    throw std::runtime_error("setSessionPortsTx no ctx (ctx is null)");
  } else if (txPortIdx >= (int)ctx->para.num_ports) {
    throw std::runtime_error("setSessionPortsTx txPortIdx out of range");
  } else if (txPortRedundantIdx >= (int)ctx->para.num_ports) {
    throw std::runtime_error("setSessionPortsTx txPortRedundantIdx out of range");
  }

  if (txPortIdx >= 0) {
    snprintf(port->port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
             ctx->para.port[txPortIdx]);
    int num_ports = 1;

    if (txPortRedundantIdx >= 0) {
      snprintf(port->port[MTL_SESSION_PORT_R], MTL_PORT_MAX_LEN, "%s",
               ctx->para.port[txPortRedundantIdx]);
      num_ports = 2;
    }

    port->num_port = num_ports;
  }
}

/**
 * @brief Set the RX session port names, including redundant port if specified.
 *
 * @param port Pointer to the st_rx_port struct to update.
 * @param rxPortIdx Index for the primary RX port in ctx->para.port, or SESSION_SKIP_PORT
 * to skip.
 * @param rxPortRedundantIdx Index for the redundant RX port in ctx->para.port, or
 * SESSION_SKIP_PORT to skip.
 */
void Handlers::setSessionPortsRx(struct st_rx_port* port, int rxPortIdx,
                                 int rxPortRedundantIdx) {
  if (!ctx) {
    throw std::runtime_error("setSessionPortsRx no ctx (ctx is null)");
  } else if (rxPortIdx > (int)ctx->para.num_ports) {
    throw std::runtime_error("setSessionPortsRx rxPortIdx out of range");
  } else if (rxPortRedundantIdx > (int)ctx->para.num_ports) {
    throw std::runtime_error("setSessionPortsRx rxPortRedundantIdx out of range");
  }

  if (rxPortIdx >= 0) {
    snprintf(port->port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
             ctx->para.port[rxPortIdx]);
    int num_ports = 1;

    if (rxPortRedundantIdx >= 0) {
      snprintf(port->port[MTL_SESSION_PORT_R], MTL_PORT_MAX_LEN, "%s",
               ctx->para.port[rxPortRedundantIdx]);
      num_ports = 2;
    }
    port->num_port = num_ports;
  }
}
