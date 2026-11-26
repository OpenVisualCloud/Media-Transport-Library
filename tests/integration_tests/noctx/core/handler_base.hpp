/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#pragma once

#include <cstdio>
#include <stdexcept>

#include "session.hpp"
#include "strategy.hpp"
#include "tests.hpp"

class Handlers {
 public:
  explicit Handlers(st_tests_context* ctx,
                    FrameTestStrategy* frameTestStrategy = nullptr);
  virtual ~Handlers() = default;

  void stopSession();

  template <typename PortT>
  void setSessionPortsImpl(PortT* port, int portIdx, int redundantIdx) {
    if (!ctx) {
      throw std::runtime_error("setSessionPorts: ctx is null");
    }
    if (portIdx >= static_cast<int>(ctx->para.num_ports) ||
        redundantIdx >= static_cast<int>(ctx->para.num_ports)) {
      throw std::runtime_error("setSessionPorts: port index out of range");
    }
    if (portIdx >= 0) {
      snprintf(port->port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
               ctx->para.port[portIdx]);
      port->num_port = 1;
      if (redundantIdx >= 0) {
        snprintf(port->port[MTL_SESSION_PORT_R], MTL_PORT_MAX_LEN, "%s",
                 ctx->para.port[redundantIdx]);
        port->num_port = 2;
      }
    }
  }

  Session session;
  st_tests_context* ctx;
  FrameTestStrategy* frameTestStrategy;
};
