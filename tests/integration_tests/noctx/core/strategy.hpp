/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#pragma once

#include <cstddef>
#include <cstdint>

class Handlers;

class FrameTestStrategy {
 public:
  explicit FrameTestStrategy(Handlers* parent = nullptr, bool enable_tx_modifier = false,
                             bool enable_rx_modifier = false)
      : parent(parent),
        enable_tx_modifier(enable_tx_modifier),
        enable_rx_modifier(enable_rx_modifier) {
  }
  virtual ~FrameTestStrategy() = default;

  virtual void txTestFrameModifier(void* /*frame*/, size_t /*frame_size*/) {
  }
  virtual void rxTestFrameModifier(void* /*frame*/, size_t /*frame_size*/) {
  }

  Handlers* parent;
  uint32_t idx_tx = 0;
  uint32_t idx_rx = 0;
  bool enable_tx_modifier;
  bool enable_rx_modifier;
};
