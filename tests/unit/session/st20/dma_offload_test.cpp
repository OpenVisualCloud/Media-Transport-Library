/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Which RX video sessions request a DMA device for ST20_RX_FLAG_DMA_OFFLOAD.
 * GPU VRAM framebuffers carry no iova, so a DMA copy into one would target
 * IOVA 0 + offset.
 */

#include <gtest/gtest.h>

#include "session/st20/st20_rx_test_base.h"

class St20RxDmaOffloadTest : public St20RxBaseTest {
 protected:
  int num_port() const override {
    return 1;
  }
};

TEST_F(St20RxDmaOffloadTest, HostFramesRequestDma) {
  EXPECT_EQ(ut20_init_sw_dma_requests(ctx_, false), 1);
}

TEST_F(St20RxDmaOffloadTest, GpuFramesDoNotRequestDma) {
  EXPECT_EQ(ut20_init_sw_dma_requests(ctx_, true), 0);
}
