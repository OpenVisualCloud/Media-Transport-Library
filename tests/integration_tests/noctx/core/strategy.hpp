/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>

class Handlers;

class FrameTestStrategy {
 public:
  FrameTestStrategy(Handlers* parent = nullptr, bool enable_tx_modifier = false,
                    bool enable_rx_modifier = false);
  virtual ~FrameTestStrategy();

  virtual void txTestFrameModifier(void* frame, size_t frame_size) {
  }
  virtual void rxTestFrameModifier(void* frame, size_t frame_size) {
  }

  Handlers* parent;
  uint32_t idx_tx;
  uint32_t idx_rx;
  double expect_fps;
  bool enable_tx_modifier;
  bool enable_rx_modifier;
};
