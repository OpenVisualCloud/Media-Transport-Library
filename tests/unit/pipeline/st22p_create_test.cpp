/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * st22p_tx_create() and st22p_rx_create() must release everything they
 * allocated when they reject the ops.
 */

#include <gtest/gtest.h>

#include <cstring>

#include "common/ut_common.h"
#include "pipeline/st22p_harness.h"
#include "pipeline/st22p_tx_harness.h"

TEST(St22PipelineTxCreate, ZeroSizeInputFrameFailsWithoutLeak) {
  ASSERT_EQ(ut22p_tx_init(), 0) << "EAL init failed";

  struct st22p_tx_ops ops;
  memset(&ops, 0, sizeof(ops));
  ops.port.num_port = 1;
  ops.codec = ST22_CODEC_JPEGXS;
  ops.fps = ST_FPS_P59_94;
  ops.input_fmt = ST_FRAME_FMT_UYVY; /* not the codestream fmt: not derive mode */
  ops.width = 0;                     /* input frame size 0 */
  ops.height = 0;
  ops.framebuff_cnt = 3;

  const uint64_t before = ut_rte_alloc_count();
  EXPECT_EQ(ut22p_tx_create(&ops), nullptr);
  EXPECT_EQ(ut_rte_alloc_count(), before) << "the pipeline ctx leaked";
}

TEST(St22PipelineRxCreate, ZeroSizeOutputFrameFailsWithoutLeak) {
  ASSERT_EQ(ut22p_init(), 0) << "EAL init failed";

  struct st22p_rx_ops ops;
  memset(&ops, 0, sizeof(ops));
  ops.port.num_port = 1;
  ops.codec = ST22_CODEC_JPEGXS;
  ops.fps = ST_FPS_P59_94;
  ops.output_fmt = ST_FRAME_FMT_UYVY; /* not the codestream fmt: not derive mode */
  ops.width = 0;                      /* output frame size 0 */
  ops.height = 0;
  ops.framebuff_cnt = 3;

  const uint64_t before = ut_rte_alloc_count();
  EXPECT_EQ(ut22p_rx_create(&ops), nullptr);
  EXPECT_EQ(ut_rte_alloc_count(), before) << "the pipeline ctx leaked";
}
