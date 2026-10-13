/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * ST40p fast metadata: ST40P_*_FLAG_FAST_METADATA and the data item header map onto
 * the ST40 transport, and a pipeline frame carries one data item.
 *
 * Build: meson setup build_unit -Denable_unit_tests=true && ninja -C build_unit
 * Run:   ./build_unit/tests/unit/UnitTest --gtest_filter='St40PipelineFmd*'
 */

#include <gtest/gtest.h>

#include <cstring>

#include "pipeline/st40p_harness.h"
#include "pipeline/st40p_tx_harness.h"
#include "session/st40_tx_harness.h"

namespace {
constexpr uint32_t kDit = 0x2a5a5a;
constexpr uint32_t kMaxDataItem = 1436; /* the largest frame level data item */
}  // namespace

class St40PipelineFmdRxTest : public ::testing::Test {
 protected:
  ut40p_ctx* ctx_ = nullptr;

  void SetUp() override {
    ASSERT_EQ(ut40p_init(), 0) << "EAL init failed";
    ctx_ = ut40p_ctx_create(/*framebuff_cnt=*/3);
    ASSERT_NE(ctx_, nullptr);
  }

  void TearDown() override {
    ut40p_ctx_destroy(ctx_);
    ctx_ = nullptr;
  }

  static struct st40_rx_frame_meta item_meta() {
    struct st40_rx_frame_meta meta;
    memset(&meta, 0, sizeof(meta));
    meta.status = ST_FRAME_STATUS_COMPLETE;
    return meta;
  }
};

TEST_F(St40PipelineFmdRxTest, CreateTransportSetsFastMetadataFlag) {
  struct st40_rx_ops ops;

  ASSERT_EQ(ut40p_rx_create_transport(ctx_, ST40P_RX_FLAG_FAST_METADATA, &ops), 0);

  EXPECT_TRUE(ops.flags & ST40_RX_FLAG_FAST_METADATA);
  EXPECT_EQ(ops.type, ST40_TYPE_FRAME_LEVEL);
}

TEST_F(St40PipelineFmdRxTest, FrameInfoCarriesItemHeaderAndContents) {
  static uint8_t slot[64];
  struct st40_rx_frame_meta meta = item_meta();
  meta.udw_buffer_fill = 12;
  meta.fmd_dit = kDit;
  meta.fmd_k_bit = 1;

  ASSERT_EQ(ut40p_inject_meta(ctx_, slot, &meta), 0);
  struct st40_frame_info* f = ut40p_get_frame(ctx_);
  ASSERT_NE(f, nullptr);

  EXPECT_EQ(f->fmd_dit, kDit);
  EXPECT_EQ(f->fmd_k_bit, 1);
  EXPECT_EQ(f->udw_buff_addr, slot);
  EXPECT_EQ(f->udw_buffer_fill, 12u);
  EXPECT_EQ(f->meta_num, 0u);
  EXPECT_EQ(ut40p_put_frame(ctx_, f), 0);
}

/* The gap is in the stream around the item, not in the item: no corruption count. */
TEST_F(St40PipelineFmdRxTest, StreamGapReachesAppWithoutCorruptedCount) {
  static uint8_t slot[16];
  struct st40_rx_frame_meta meta = item_meta();
  meta.seq_discont = true;
  meta.seq_lost = 2;

  ASSERT_EQ(ut40p_inject_meta(ctx_, slot, &meta), 0);
  struct st40_frame_info* f = ut40p_get_frame(ctx_);
  ASSERT_NE(f, nullptr);

  EXPECT_TRUE(f->seq_discont);
  EXPECT_EQ(f->seq_lost, 2u);
  EXPECT_EQ(f->status, ST_FRAME_STATUS_COMPLETE);
  EXPECT_EQ(ut40p_stat_frames_corrupted(ctx_), 0u);
  EXPECT_EQ(ut40p_put_frame(ctx_, f), 0);
}

class St40PipelineFmdTxTest : public ::testing::Test {
 protected:
  ut40p_tx_ctx* ctx_ = nullptr;

  void SetUp() override {
    ASSERT_EQ(ut40p_tx_init(), 0) << "EAL init failed";
    ctx_ = ut40p_tx_ctx_create(/*framebuff_cnt=*/3);
    ASSERT_NE(ctx_, nullptr);
  }

  void TearDown() override {
    ut40p_tx_ctx_destroy(ctx_);
    ctx_ = nullptr;
  }
};

TEST_F(St40PipelineFmdTxTest, CreateTransportPassesFlagAndItemHeader) {
  struct st40p_tx_ops ops;
  struct st40_tx_ops created;
  memset(&ops, 0, sizeof(ops));
  ops.flags = ST40P_TX_FLAG_FAST_METADATA;
  ops.framebuff_cnt = ut40p_tx_framebuff_cnt(ctx_);
  ops.fmd_dit = kDit;
  ops.fmd_k_bit = 1;

  ut40p_tx_transport_ops_of(ctx_, &ops, &created);

  EXPECT_TRUE(created.flags & ST40_TX_FLAG_FAST_METADATA);
  EXPECT_EQ(created.fmd_dit, kDit);
  EXPECT_EQ(created.fmd_k_bit, 1);
}

/* meta[] does not exist on the wire in this mode, so its count must not fail put. */
TEST_F(St40PipelineFmdTxTest, PutFrameIgnoresMetaNum) {
  ut40p_tx_ctx_set_flags(ctx_, ST40P_TX_FLAG_FAST_METADATA);
  struct st40_frame_info* f = ut40p_tx_get_frame(ctx_);
  ASSERT_NE(f, nullptr);
  f->meta_num = ST40_MAX_META + 1;
  f->udw_buffer_size = 1024;
  f->udw_buffer_fill = 100;

  ASSERT_EQ(ut40p_tx_put_frame(ctx_, f), 0);

  const struct st40_frame* anc = ut40p_tx_anc_frame(ctx_, ut40p_tx_frame_idx(f));
  EXPECT_EQ(anc->meta_num, 0u);
  EXPECT_EQ(anc->data_size, 100u);
}

/* The transport copies udw_buffer_fill bytes, so a fill past the buffer must fail put. */
TEST_F(St40PipelineFmdTxTest, PutFrameRejectsFillPastBuffer) {
  ut40p_tx_ctx_set_flags(ctx_, ST40P_TX_FLAG_FAST_METADATA);
  struct st40_frame_info* f = ut40p_tx_get_frame(ctx_);
  ASSERT_NE(f, nullptr);
  f->udw_buffer_size = 64;
  f->udw_buffer_fill = 65;

  EXPECT_EQ(ut40p_tx_put_frame(ctx_, f), -EIO);
}

TEST_F(St40PipelineFmdTxTest, PutFrameAcceptsFillOfWholeBuffer) {
  ut40p_tx_ctx_set_flags(ctx_, ST40P_TX_FLAG_FAST_METADATA);
  struct st40_frame_info* f = ut40p_tx_get_frame(ctx_);
  ASSERT_NE(f, nullptr);
  f->udw_buffer_size = 64;
  f->udw_buffer_fill = 64;

  EXPECT_EQ(ut40p_tx_put_frame(ctx_, f), 0);
}

/* A frame put back with no fill is a zero-item packet, not the previous item again. */
TEST_F(St40PipelineFmdTxTest, NextGetOfFrameStartsWithNoFill) {
  ut40p_tx_ctx_destroy(ctx_);
  ctx_ = ut40p_tx_ctx_create(/*framebuff_cnt=*/1);
  ASSERT_NE(ctx_, nullptr);
  ut40p_tx_ctx_set_flags(ctx_, ST40P_TX_FLAG_FAST_METADATA);
  struct st40_frame_info* f = ut40p_tx_get_frame(ctx_);
  ASSERT_NE(f, nullptr);
  f->udw_buffer_size = 1024;
  f->udw_buffer_fill = 100;
  ASSERT_EQ(ut40p_tx_put_frame(ctx_, f), 0);
  uint16_t idx = 0;
  ASSERT_EQ(ut40p_tx_next_frame(ctx_, &idx), 0);
  ASSERT_EQ(ut40p_tx_frame_done(ctx_, idx), 0);

  f = ut40p_tx_get_frame(ctx_);
  ASSERT_NE(f, nullptr);
  ASSERT_EQ(ut40p_tx_put_frame(ctx_, f), 0);

  EXPECT_EQ(ut40p_tx_anc_frame(ctx_, ut40p_tx_frame_idx(f))->data_size, 0u);
}

/* st40_pipeline_api.h: a fill past 1436 bytes fails put, even in a larger buffer. */
TEST_F(St40PipelineFmdTxTest, PutFrameRejectsItemPastMaxDataItem) {
  ut40p_tx_ctx_set_flags(ctx_, ST40P_TX_FLAG_FAST_METADATA);
  struct st40_frame_info* f = ut40p_tx_get_frame(ctx_);
  ASSERT_NE(f, nullptr);
  f->udw_buffer_size = 2048;
  f->udw_buffer_fill = kMaxDataItem + 1;

  EXPECT_LT(ut40p_tx_put_frame(ctx_, f), 0);
}

/* The pipeline frame runs through the real ST40 TX frame tasklet. */
class St40PipelineFmdTxSessionTest : public ::testing::Test {
 protected:
  ut40p_tx_ctx* p_ = nullptr;
  ut_txa_ctx* s_ = nullptr;
  int done_calls_ = 0;
  enum st_frame_status done_status_ = ST_FRAME_STATUS_COMPLETE;

  static int on_done(void* priv, struct st40_frame_info* frame) {
    auto* self = static_cast<St40PipelineFmdTxSessionTest*>(priv);
    self->done_calls_++;
    self->done_status_ = frame->status;
    return 0;
  }

  void SetUp() override {
    ASSERT_EQ(ut40p_tx_init(), 0) << "EAL init failed";
    p_ = ut40p_tx_ctx_create(/*framebuff_cnt=*/1);
    ASSERT_NE(p_, nullptr);
    s_ = ut_txa_create();
    ASSERT_NE(s_, nullptr);

    ut40p_tx_ctx_set_flags(p_, ST40P_TX_FLAG_FAST_METADATA);
    ut40p_tx_set_notify_frame_done(p_, on_done, this);
    struct st40p_tx_ops ops;
    struct st40_tx_ops transport;
    memset(&ops, 0, sizeof(ops));
    ops.flags = ST40P_TX_FLAG_FAST_METADATA;
    ops.framebuff_cnt = 1;
    ut40p_tx_transport_ops_of(p_, &ops, &transport);

    ut_txa_set_cur_epochs(s_, 10);
    ut_txa_set_mock_ptp_time(s_, 10ull * 1000 * 1000);
    ut_txa_set_mock_tsc_time(s_, 0);
    ASSERT_EQ(ut_txa_prepare_frame_tasklet(s_, ST10_TIMESTAMP_FMT_TAI, 0, 1), 0);
    ut_txa_set_fmd(s_, kDit, 1);
    ut_txa_set_frame_source(s_, &transport,
                            const_cast<struct st40_frame*>(ut40p_tx_anc_frame(p_, 0)));
  }

  void TearDown() override {
    ut_txa_destroy(s_);
    ut40p_tx_ctx_destroy(p_);
  }

  void step_frame() {
    ut_txa_step_frame_tasklet(s_);
    ut_txa_set_mock_tsc_time(s_, ut_txa_tsc_time_cursor(s_));
    ut_txa_step_frame_tasklet(s_);
  }
};

/* An item past 1436 bytes is not sent: it must not reach the app as complete or sent. */
TEST_F(St40PipelineFmdTxSessionTest, OversizedItemIsNotReportedSent) {
  struct st40_frame_info* f = ut40p_tx_get_frame(p_);
  ASSERT_NE(f, nullptr);
  f->udw_buffer_size = 2048;
  f->udw_buffer_fill = kMaxDataItem + 1;
  (void)ut40p_tx_put_frame(p_, f); /* rejected up front, or aborted by the transport */

  step_frame();

  EXPECT_EQ(ut_txa_queued_packets(s_), 0u);
  EXPECT_EQ(ut40p_tx_stat_frames_sent(p_), 0u);
  EXPECT_FALSE(done_calls_ && done_status_ == ST_FRAME_STATUS_COMPLETE)
      << "notify_frame_done reported the unsent item complete";
}
