/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include "handler_base.hpp"

#include <cstdio>
#include <stdexcept>

Handlers::Handlers(st_tests_context* ctx, FrameTestStrategy* frameTestStrategy)
    : ctx(ctx), frameTestStrategy(frameTestStrategy) {
}

Handlers::~Handlers() {
  session.stop();
}

void Handlers::startSession(
    std::vector<std::function<void(std::atomic<bool>&)>> threadFunctions, bool isRx) {
  for (auto& func : threadFunctions) {
    session.addThread(func, isRx);
  }
}

void Handlers::stopSession() {
  session.stop();
}

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

void Handlers::setSessionPortsRx(struct st_rx_port* port, int rxPortIdx,
                                 int rxPortRedundantIdx) {
  if (!ctx) {
    throw std::runtime_error("setSessionPortsRx no ctx (ctx is null)");
  } else if (rxPortIdx >= (int)ctx->para.num_ports) {
    throw std::runtime_error("setSessionPortsRx rxPortIdx out of range");
  } else if (rxPortRedundantIdx >= (int)ctx->para.num_ports) {
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
