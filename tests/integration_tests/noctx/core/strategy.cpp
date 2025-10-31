/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include "strategy.hpp"

FrameTestStrategy::FrameTestStrategy(Handlers* parent, bool enable_tx_modifier,
                                     bool enable_rx_modifier)
    : parent(parent),
      idx_tx(0),
      idx_rx(0),
      expect_fps(0.0),
      enable_tx_modifier(enable_tx_modifier),
      enable_rx_modifier(enable_rx_modifier) {
}

FrameTestStrategy::~FrameTestStrategy() {
  parent = nullptr;
}
