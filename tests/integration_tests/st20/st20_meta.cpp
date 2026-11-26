/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include "st20_common.h"

namespace {

struct St20MetaStream {
  enum st_fps fps;
  int width;
  int height;
};

int st20_tx_meta_build_rtp(tests_context* s, struct st20_rfc4175_rtp_hdr* rtp,
                           uint16_t* pkt_len) {
  struct st20_rfc4175_extra_rtp_hdr* e_rtp = NULL;
  int offset;
  int frame_size = s->frame_size;
  uint16_t row_number, row_offset;
  uint8_t* payload = (uint8_t*)rtp + sizeof(*rtp);
  int pkt_idx = s->pkt_idx;

  if (s->single_line) {
    row_number = pkt_idx / s->pkts_in_line;
    int pixels_in_pkt = s->pkt_data_len / s->st20_pg.size * s->st20_pg.coverage;
    row_offset = pixels_in_pkt * (pkt_idx % s->pkts_in_line);
    offset = (row_number * s->width + row_offset) / s->st20_pg.coverage * s->st20_pg.size;
  } else {
    offset = s->pkt_data_len * pkt_idx;
    row_number = offset / s->bytes_in_line;
    row_offset = (offset % s->bytes_in_line) * s->st20_pg.coverage / s->st20_pg.size;
    if ((offset + s->pkt_data_len > (row_number + 1) * s->bytes_in_line) &&
        (offset + s->pkt_data_len < frame_size)) {
      e_rtp = (struct st20_rfc4175_extra_rtp_hdr*)payload;
      payload += sizeof(*e_rtp);
    }
  }
  bool marker = false;

  /* update hdr */
  rtp->base.csrc_count = 0;
  rtp->base.extension = 0;
  rtp->base.padding = 0;
  rtp->base.version = 2;
  rtp->base.marker = 0;
  rtp->base.payload_type = ST20_TEST_PAYLOAD_TYPE;
  rtp->row_number = htons(row_number);
  rtp->row_offset = htons(row_offset);
  rtp->base.tmstamp = htonl(s->rtp_tmstamp);
  rtp->base.seq_number = htons(s->seq_id);
  rtp->seq_number_ext = htons((uint16_t)(s->seq_id >> 16));
  s->seq_id++;
  int temp = s->single_line
                 ? ((s->width - row_offset) / s->st20_pg.coverage * s->st20_pg.size)
                 : (frame_size - offset);
  uint16_t data_len = s->pkt_data_len > temp ? temp : s->pkt_data_len;
  rtp->row_length = htons(data_len);
  *pkt_len = data_len + sizeof(*rtp);
  if (e_rtp) {
    uint16_t row_length_0 = (row_number + 1) * s->bytes_in_line - offset;
    uint16_t row_length_1 = s->pkt_data_len - row_length_0;
    rtp->row_length = htons(row_length_0);
    e_rtp->row_length = htons(row_length_1);
    e_rtp->row_offset = htons(0);
    e_rtp->row_number = htons(row_number + 1);
    rtp->row_offset = htons(row_offset | ST20_SRD_OFFSET_CONTINUATION);
    *pkt_len += sizeof(*e_rtp);
  }

  s->pkt_idx++;

  /* build incomplete frame */
  if (s->pkt_idx >= s->total_pkts_in_frame) marker = true;
  if (__atomic_load_n(&s->fb_send, __ATOMIC_RELAXED) % 2) {
    if (s->pkt_idx >= (s->total_pkts_in_frame / 2)) marker = true;
  }
  if (marker) {
    /* end of current frame */
    rtp->base.marker = 1;

    s->pkt_idx = 0;
    s->rtp_tmstamp++;
    __atomic_fetch_add(&s->fb_send, 1, __ATOMIC_RELAXED);
  }

  return 0;
}

void st20_rx_meta_feed_packet(void* args) {
  auto ctx = (tests_context*)args;
  auto handle = (st20_tx_handle)st20_get_handle(ctx);
  void* mbuf;
  void* usrptr = NULL;
  uint16_t mbuf_len = 0;
  std::unique_lock<std::mutex> lck(ctx->mtx, std::defer_lock);
  while (!ctx->stop) {
    /* get available buffer*/
    mbuf = st20_tx_get_mbuf(handle, &usrptr);
    if (!mbuf) {
      lck.lock();
      /* try again */
      mbuf = st20_tx_get_mbuf(handle, &usrptr);
      if (mbuf) {
        lck.unlock();
      } else {
        if (!ctx->stop) ctx->cv.wait(lck);
        lck.unlock();
        continue;
      }
    }

    /* build the rtp pkt */
    st20_tx_meta_build_rtp(ctx, (struct st20_rfc4175_rtp_hdr*)usrptr, &mbuf_len);

    st20_tx_put_mbuf(handle, mbuf, mbuf_len);
  }
}

int st20_rx_meta_frame_ready(void* priv, void* frame, struct st20_rx_frame_meta* meta) {
  auto ctx = (tests_context*)priv;
  auto expect_meta = (struct st20_rx_frame_meta*)ctx->priv;

  auto handle = (st20_rx_handle)st20_get_handle(ctx);
  if (!handle) return -EIO;

  __atomic_fetch_add(&ctx->fb_rec, 1, __ATOMIC_RELAXED);
  if (!ctx->start_time) ctx->start_time = st_test_get_monotonic_time();
  if (expect_meta->width != meta->width) ctx->rx_meta_fail_cnt++;
  if (expect_meta->height != meta->height) ctx->rx_meta_fail_cnt++;
  if (expect_meta->fps != meta->fps) ctx->rx_meta_fail_cnt++;
  if (expect_meta->fmt != meta->fmt) ctx->rx_meta_fail_cnt++;
  if (expect_meta->timestamp == meta->timestamp) ctx->rx_meta_fail_cnt++;
  expect_meta->timestamp = meta->timestamp;
  if (!st_is_frame_complete(meta->status)) {
    ctx->incomplete_frame_cnt++;
    if (meta->frame_total_size <= meta->frame_recv_size) ctx->rx_meta_fail_cnt++;
  } else {
    if (meta->frame_total_size != meta->frame_recv_size) ctx->rx_meta_fail_cnt++;
  }
  st20_rx_put_framebuff(handle, frame);

  return 0;
}

void st20_rx_meta_test(const std::vector<St20MetaStream>& streams, enum st20_fmt fmt) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto m_handle = ctx->handle;
  const int sessions = static_cast<int>(streams.size());

  MTL_TEST_REQUIRE_PORTS(ctx, 2);

  std::vector<tests_context*> test_ctx_tx(sessions);
  std::vector<tests_context*> test_ctx_rx(sessions);
  std::vector<st20_tx_handle> tx_handle(sessions);
  std::vector<st20_rx_handle> rx_handle(sessions);
  std::vector<double> expect_framerate(sessions);
  std::vector<std::thread> rtp_thread_tx(sessions);

  St20DeinitGuard guard(m_handle, test_ctx_tx, test_ctx_rx, tx_handle, rx_handle,
                        &rtp_thread_tx, nullptr);

  for (int i = 0; i < sessions; i++) {
    const auto& st = streams[i];
    expect_framerate[i] = st_frame_rate(st.fps);
    test_ctx_tx[i] = init_test_ctx(ctx, i, 3);
    ASSERT_TRUE(test_ctx_tx[i] != NULL);

    struct st20_tx_ops ops_tx;
    init_single_port_tx(ops_tx, test_ctx_tx[i], "st20_meta_test", udp_port_for_idx(i));
    ops_tx.pacing = ST21_PACING_NARROW;
    ops_tx.packing = ST20_PACKING_BPM;
    ops_tx.type = ST20_TYPE_RTP_LEVEL;
    ops_tx.width = st.width;
    ops_tx.height = st.height;
    ops_tx.fps = st.fps;
    ops_tx.fmt = fmt;
    ops_tx.payload_type = ST20_TEST_PAYLOAD_TYPE;
    ops_tx.framebuff_cnt = test_ctx_tx[i]->fb_cnt;
    ops_tx.get_next_frame = tx_next_video_frame;
    rtp_tx_specific_init(&ops_tx, test_ctx_tx[i]);
    tx_handle[i] = st20_tx_create(m_handle, &ops_tx);
    ASSERT_TRUE(tx_handle[i] != NULL);
    test_ctx_tx[i]->stop = false;
  }

  for (int i = 0; i < sessions; i++) {
    const auto& st = streams[i];
    test_ctx_rx[i] = init_test_ctx(ctx, i, 3);
    ASSERT_TRUE(test_ctx_rx[i] != NULL);

    struct st20_rx_ops ops_rx;
    init_single_port_rx(ops_rx, test_ctx_rx[i], "st20_meta_test", udp_port_for_idx(i));
    ops_rx.type = ST20_TYPE_FRAME_LEVEL;
    ops_rx.width = st.width;
    ops_rx.height = st.height;
    ops_rx.fps = st.fps;
    ops_rx.fmt = fmt;
    ops_rx.payload_type = ST20_TEST_PAYLOAD_TYPE;
    ops_rx.flags |= ST20_RX_FLAG_RECEIVE_INCOMPLETE_FRAME;
    ops_rx.framebuff_cnt = test_ctx_rx[i]->fb_cnt;
    ops_rx.notify_frame_ready = st20_rx_meta_frame_ready;
    ops_rx.notify_rtp_ready = rx_rtp_ready;
    ops_rx.rtp_ring_size = 1024;
    rx_handle[i] = st20_rx_create(m_handle, &ops_rx);
    ASSERT_TRUE(rx_handle[i] != NULL);
    test_ctx_rx[i]->frame_size = test_ctx_tx[i]->frame_size;
    test_ctx_rx[i]->stop = false;

    /* set expect meta data to private */
    auto meta =
        (struct st20_rx_frame_meta*)st_test_zmalloc(sizeof(struct st20_rx_frame_meta));
    ASSERT_TRUE(meta != NULL);
    meta->width = ops_rx.width;
    meta->height = ops_rx.height;
    meta->fps = ops_rx.fps;
    meta->fmt = ops_rx.fmt;
    test_ctx_rx[i]->priv = meta;
    st20_set_handle(test_ctx_rx[i], rx_handle[i]);
  }

  /* Publish TX handles and start TX feeds only after every RX session is ready. */
  for (int i = 0; i < sessions; i++) {
    st20_set_handle(test_ctx_tx[i], tx_handle[i]);
    rtp_thread_tx[i] = std::thread(st20_rx_meta_feed_packet, test_ctx_tx[i]);
  }

  int ret = mtl_start(m_handle);
  EXPECT_GE(ret, 0);
  guard.set_started(ret >= 0);

  /*
   * Measure steady-state RX FPS: warmup=12, then >=35 frames over >=1500 ms
   * (~90 frames at 59.94 fps): <=1.1% FPS edge error and >=102 total frames
   * for the 50% incomplete ratio.
   */
  auto res = st20_wait_rx_window(test_ctx_rx, true, false);
  EXPECT_TRUE(res.warmed_up);
  EXPECT_TRUE(res.complete);
  guard.release_sessions();

  for (int i = 0; i < sessions; i++) {
    EXPECT_GT(test_ctx_rx[i]->fb_rec, 0);
    float expect_incomplete_frame_cnt = (float)test_ctx_rx[i]->fb_rec / 2;
    EXPECT_NEAR(test_ctx_rx[i]->incomplete_frame_cnt, expect_incomplete_frame_cnt,
                expect_incomplete_frame_cnt * 0.1);
    EXPECT_EQ(test_ctx_rx[i]->sha_fail_cnt, 0);
    EXPECT_EQ(test_ctx_rx[i]->rx_meta_fail_cnt, 0);
    info("%s, session %d fb_rec %d fb_incomplete %d framerate %f\n", __func__, i,
         test_ctx_rx[i]->fb_rec, test_ctx_rx[i]->incomplete_frame_cnt, res.rate[i]);
    EXPECT_NEAR(res.rate[i], expect_framerate[i], expect_framerate[i] * 0.1);
  }
}

}  // namespace

TEST(St20_rx, frame_meta_1080p_fps59_94_s1) {
  st20_rx_meta_test({{ST_FPS_P59_94, 1920, 1080}}, ST20_FMT_YUV_422_10BIT);
}
