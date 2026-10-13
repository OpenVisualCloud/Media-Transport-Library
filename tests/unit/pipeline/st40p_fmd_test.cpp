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

#include <chrono>
#include <cstring>
#include <vector>

#include "pipeline/st40p_harness.h"
#include "pipeline/st40p_tx_harness.h"
#include "session/st40_harness.h"
#include "session/st40_tx_harness.h"

namespace {
constexpr uint32_t kDit = 0x2a5a5a;
constexpr uint32_t kMaxDataItem = 1436; /* the largest frame level data item */

void append_be32(std::vector<uint8_t>& pkt, uint32_t word) {
  for (int shift = 24; shift >= 0; shift -= 8)
    pkt.push_back(static_cast<uint8_t>(word >> shift));
}

/* ST 2110-41 RTP header with no item, payload type 0 and SSRC 0. */
std::vector<uint8_t> zero_item_packet(uint16_t seq, uint32_t ts) {
  std::vector<uint8_t> pkt = {0x80, 0x00, static_cast<uint8_t>(seq >> 8),
                              static_cast<uint8_t>(seq)};
  append_be32(pkt, ts);
  append_be32(pkt, 0);
  return pkt;
}

void append_item(std::vector<uint8_t>& pkt, uint32_t dit, uint8_t k, uint32_t words) {
  append_be32(pkt, (dit << 10) | (static_cast<uint32_t>(k) << 9) | words);
  for (uint32_t i = 0; i < words; i++) append_be32(pkt, 0xc0c1c2c3 + i);
}
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

/* The app may put frames in any order; get_frame must still give the oldest first. */
TEST_F(St40PipelineFmdRxTest, OutOfOrderPutKeepsDeliveryOrder) {
  static uint8_t slot[5];
  struct st40_frame_info* f[3];
  for (uint32_t i = 0; i < 3; i++) {
    struct st40_rx_frame_meta meta = item_meta();
    meta.fmd_dit = i;
    ASSERT_EQ(ut40p_inject_meta(ctx_, &slot[i], &meta), 0);
  }
  for (int i = 0; i < 3; i++) ASSERT_NE(f[i] = ut40p_get_frame(ctx_), nullptr);
  ASSERT_EQ(ut40p_put_frame(ctx_, f[2]), 0);
  struct st40_rx_frame_meta older = item_meta();
  older.fmd_dit = 3;
  ASSERT_EQ(ut40p_inject_meta(ctx_, &slot[3], &older), 0);
  ASSERT_EQ(ut40p_put_frame(ctx_, f[0]), 0);
  struct st40_rx_frame_meta newer = item_meta();
  newer.fmd_dit = 4;
  ASSERT_EQ(ut40p_inject_meta(ctx_, &slot[4], &newer), 0);

  struct st40_frame_info* next = ut40p_get_frame(ctx_);
  ASSERT_NE(next, nullptr);
  EXPECT_EQ(next->fmd_dit, 3u) << "the item delivered first must come out first";
}

/* ST40P_RX_FLAG_BLOCK_GET: with no frame ready, get_frame waits for the timeout. */
TEST_F(St40PipelineFmdRxTest, BlockGetWaitsAfterBurstIsTaken) {
  static uint8_t slot[2];
  constexpr uint64_t kTimeoutNs = 200ull * 1000 * 1000;
  ut40p_ctx_enable_block_get(ctx_, kTimeoutNs);
  struct st40_frame_info* f[2];
  for (int i = 0; i < 2; i++) {
    struct st40_rx_frame_meta meta = item_meta();
    ASSERT_EQ(ut40p_inject_meta(ctx_, &slot[i], &meta), 0);
  }
  for (int i = 0; i < 2; i++) ASSERT_NE(f[i] = ut40p_get_frame(ctx_), nullptr);

  auto start = std::chrono::steady_clock::now();
  EXPECT_EQ(ut40p_get_frame(ctx_), nullptr);
  auto waited = std::chrono::steady_clock::now() - start;

  EXPECT_GE(std::chrono::duration_cast<std::chrono::milliseconds>(waited).count(), 150)
      << "get_frame returned at once, with no frame and before the timeout";
  for (int i = 0; i < 2; i++) EXPECT_EQ(ut40p_put_frame(ctx_, f[i]), 0);
}

/* The real ST40 FMD transport feeds the pipeline, and put_frame returns its slot. */
class St40PipelineFmdE2eTest : public ::testing::Test {
 protected:
  static constexpr uint16_t kFrames = 2;
  static constexpr uint32_t kSize = 64;
  static constexpr uint32_t kTs = 0x1234;
  ut_test_ctx* sess_ = nullptr;
  ut40p_ctx* pipe_ = nullptr;

  static void release(void* priv, void* addr) {
    ut40_release_frame(static_cast<ut_test_ctx*>(priv), addr);
  }

  void SetUp() override {
    ASSERT_EQ(ut40_init(), 0) << "EAL init failed";
    ASSERT_EQ(ut40p_init(), 0) << "EAL init failed";
    sess_ = ut40_ctx_create(1);
    ASSERT_NE(sess_, nullptr);
    pipe_ = ut40p_ctx_create(kFrames);
    ASSERT_NE(pipe_, nullptr);
    ut40_ctx_set_fmd(sess_);
    /* the transport pool the pipeline asks for */
    struct st40_rx_ops transport;
    ASSERT_EQ(ut40p_rx_create_transport(pipe_, ST40P_RX_FLAG_FAST_METADATA, &transport),
              0);
    ut40_setup_frame_pool_cb(sess_, transport.framebuff_cnt, kSize,
                             ut40p_frame_ready_cb(), ut40p_frame_ready_priv(pipe_));
    ut40p_set_put_framebuff_hook(release, sess_);
  }

  void TearDown() override {
    ut40p_put_framebuff_reset_spy();
    if (sess_) ut40_teardown_frame_pool(sess_);
    ut40_ctx_destroy(sess_);
    ut40p_ctx_destroy(pipe_);
  }

  int feed(const std::vector<uint8_t>& pkt) {
    return ut40_feed_rtp_bytes(sess_, pkt.data(), static_cast<uint16_t>(pkt.size()),
                               MTL_SESSION_PORT_P);
  }
};

TEST_F(St40PipelineFmdE2eTest, ItemReachesAppWithHeaderContentsAndTimestamps) {
  auto pkt = zero_item_packet(5, kTs);
  append_item(pkt, kDit, 1, 2);
  ASSERT_EQ(feed(pkt), 0);

  struct st40_frame_info* f = ut40p_get_frame(pipe_);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->fmd_dit, kDit);
  EXPECT_EQ(f->fmd_k_bit, 1);
  ASSERT_EQ(f->udw_buffer_fill, 8u);
  EXPECT_EQ(f->udw_buff_addr[0], 0xc0);
  EXPECT_EQ(f->udw_buff_addr[7], 0xc4);
  EXPECT_EQ(f->meta_num, 0u);
  EXPECT_EQ(f->rtp_timestamp, kTs);
  EXPECT_EQ(f->timestamp, kTs);
  EXPECT_EQ(f->tfmt, ST10_TIMESTAMP_FMT_MEDIA_CLK);
  EXPECT_NE(f->receive_timestamp, 0u);
  EXPECT_EQ(f->status, ST_FRAME_STATUS_COMPLETE);
  EXPECT_EQ(f->pkts_total, 1u);
  EXPECT_EQ(f->pkts_recv[MTL_SESSION_PORT_P], 1u);
  EXPECT_FALSE(f->seq_discont);
  EXPECT_EQ(ut40p_put_frame(pipe_, f), 0);
}

/* stat_frames_dropped: frames the pipeline could not deliver for no free user slot. */
TEST_F(St40PipelineFmdE2eTest, ItemDroppedForBusyFramesCountsFramesDropped) {
  auto pkt = zero_item_packet(0, kTs);
  for (uint32_t i = 0; i < kFrames + 1; i++) append_item(pkt, i, 0, 1);
  ASSERT_EQ(feed(pkt), 0);

  struct st40_rx_user_stats stats;
  ASSERT_EQ(ut40p_get_session_stats(pipe_, &stats), 0);
  EXPECT_EQ(stats.common.stat_frames_received, kFrames);
  EXPECT_EQ(ut40_stat_anc_frames_dropped(sess_), 0u)
      << "the transport has a slot left and hands the item to the pipeline";
  EXPECT_EQ(stats.common.stat_frames_dropped, 1u)
      << "the app never sees the last item, and no stat counts it";
}

TEST_F(St40PipelineFmdE2eTest, ItemAfterBusyFramesDropIsFlagged) {
  auto pkt = zero_item_packet(0, kTs);
  for (uint32_t i = 0; i < kFrames + 1; i++) append_item(pkt, i, 0, 1);
  ASSERT_EQ(feed(pkt), 0);
  for (int i = 0; i < kFrames; i++) {
    struct st40_frame_info* f = ut40p_get_frame(pipe_);
    ASSERT_NE(f, nullptr);
    EXPECT_FALSE(f->seq_discont) << "item " << i;
    EXPECT_EQ(ut40p_put_frame(pipe_, f), 0);
  }
  auto next = zero_item_packet(1, kTs + 1);
  append_item(next, kDit, 0, 1);
  ASSERT_EQ(feed(next), 0);

  struct st40_frame_info* f = ut40p_get_frame(pipe_);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->fmd_dit, kDit);
  EXPECT_TRUE(f->seq_discont);
  EXPECT_EQ(f->seq_lost, 0u);
  EXPECT_EQ(ut40p_put_frame(pipe_, f), 0);
}

TEST_F(St40PipelineFmdE2eTest, PutFrameAndAbortReturnTransportSlots) {
  auto pkt = zero_item_packet(0, kTs);
  for (uint32_t i = 0; i < kFrames; i++) append_item(pkt, i, 0, 1);
  for (uint16_t seq = 0; seq < 3; seq++) {
    pkt[2] = static_cast<uint8_t>(seq >> 8);
    pkt[3] = static_cast<uint8_t>(seq);
    ASSERT_EQ(feed(pkt), 0);
    struct st40_frame_info* f0 = ut40p_get_frame(pipe_);
    struct st40_frame_info* f1 = ut40p_get_frame(pipe_);
    ASSERT_NE(f0, nullptr) << "seq " << seq;
    ASSERT_NE(f1, nullptr) << "seq " << seq;
    EXPECT_EQ(f0->fmd_dit, 0u);
    EXPECT_EQ(f1->fmd_dit, 1u);
    EXPECT_FALSE(f0->seq_discont) << "seq " << seq;
    EXPECT_EQ(ut40p_put_frame(pipe_, f0), 0);
    EXPECT_EQ(ut40p_put_frame_abort(pipe_, f1), 0);
  }
}

/* RFC 8331 through the same seam: a seq gap inside a frame is CORRUPTED and counted. */
TEST_F(St40PipelineFmdE2eTest, Rfc8331SeqGapStillCountsCorrupted) {
  ut40_teardown_frame_pool(sess_);
  ut40_ctx_destroy(sess_);
  sess_ = ut40_ctx_create(1);
  ASSERT_NE(sess_, nullptr);
  ut40_setup_frame_pool_cb(sess_, kFrames, kSize, ut40p_frame_ready_cb(),
                           ut40p_frame_ready_priv(pipe_));
  ut40p_set_put_framebuff_hook(release, sess_);

  ASSERT_EQ(ut40_feed_pkt_anc0(sess_, 0, kTs, 0, MTL_SESSION_PORT_P), 0);
  ASSERT_EQ(ut40_feed_pkt_anc0(sess_, 2, kTs, 1, MTL_SESSION_PORT_P), 0);

  struct st40_frame_info* f = ut40p_get_frame(pipe_);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->status, ST_FRAME_STATUS_CORRUPTED);
  EXPECT_TRUE(f->seq_discont);
  EXPECT_EQ(f->fmd_dit, 0u);
  EXPECT_EQ(ut40p_stat_frames_corrupted(pipe_), 1u);
  EXPECT_EQ(ut40p_put_frame(pipe_, f), 0);
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

/* USER_TIMESTAMP alone: the frame timestamp is the one the transport puts in RTP. */
TEST_F(St40PipelineFmdTxTest, UserTimestampReachesTransport) {
  ut40p_tx_ctx_set_flags(ctx_,
                         ST40P_TX_FLAG_FAST_METADATA | ST40P_TX_FLAG_USER_TIMESTAMP);
  struct st40_frame_info* f = ut40p_tx_get_frame(ctx_);
  ASSERT_NE(f, nullptr);
  f->udw_buffer_size = 64;
  f->udw_buffer_fill = 8;
  f->tfmt = ST10_TIMESTAMP_FMT_MEDIA_CLK;
  f->timestamp = 0x12345678;
  ASSERT_EQ(ut40p_tx_put_frame(ctx_, f), 0);

  struct st40_tx_frame_meta meta;
  memset(&meta, 0, sizeof(meta));
  meta.tfmt = ST10_TIMESTAMP_FMT_TAI; /* the transport's epoch default */
  meta.timestamp = 999;
  uint16_t idx = 0;
  ASSERT_EQ(ut40p_tx_next_frame_meta(ctx_, &idx, &meta), 0);

  EXPECT_EQ(meta.tfmt, ST10_TIMESTAMP_FMT_MEDIA_CLK);
  EXPECT_EQ(meta.timestamp, 0x12345678u);
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
