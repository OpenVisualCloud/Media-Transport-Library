/* SPDX-License-Identifier: BSD-3-Clause */
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>

class Handlers;

class StrategySharedState {
 public:
  struct PacingParameters {
    double tr_offset_ns = 0.0;
    double trs_ns = 0.0;
    uint32_t vrx_pkts = 0;
    bool has_value = false;
  };

  void setPacingParameters(double tr_offset_ns, double trs_ns, uint32_t vrx_pkts);
  PacingParameters getPacingParameters() const;

 private:
  mutable std::mutex pacing_mutex_;
  PacingParameters pacing_;
};

class FrameTestStrategy {
 public:
  FrameTestStrategy(Handlers* parent = nullptr, bool enable_tx_modifier = false,
                    bool enable_rx_modifier = false);
  virtual ~FrameTestStrategy();

  void setSharedState(std::shared_ptr<StrategySharedState> state);
  std::shared_ptr<StrategySharedState> sharedState() const;

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

 private:
  std::shared_ptr<StrategySharedState> shared_state_;
};
