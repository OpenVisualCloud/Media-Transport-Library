/* SPDX-License-Identifier: BSD-3-Clause */

#include "strategy.hpp"

#include <utility>

void StrategySharedState::setPacingParameters(double tr_offset_ns, double trs_ns,
                                              uint32_t vrx_pkts) {
  std::lock_guard<std::mutex> lock(pacing_mutex_);
  pacing_.tr_offset_ns = tr_offset_ns;
  pacing_.trs_ns = trs_ns;
  pacing_.vrx_pkts = vrx_pkts;
  pacing_.has_value = true;
}

StrategySharedState::PacingParameters StrategySharedState::getPacingParameters() const {
  std::lock_guard<std::mutex> lock(pacing_mutex_);
  return pacing_;
}

FrameTestStrategy::FrameTestStrategy(Handlers* parent, bool enable_tx_modifier,
                                     bool enable_rx_modifier)
    : parent(parent),
      idx_tx(0),
      idx_rx(0),
      expect_fps(0.0),
      enable_tx_modifier(enable_tx_modifier),
      enable_rx_modifier(enable_rx_modifier),
      shared_state_(nullptr) {
}

FrameTestStrategy::~FrameTestStrategy() {
  parent = nullptr;
}

void FrameTestStrategy::setSharedState(std::shared_ptr<StrategySharedState> state) {
  shared_state_ = std::move(state);
}

std::shared_ptr<StrategySharedState> FrameTestStrategy::sharedState() const {
  return shared_state_;
}
