/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include "st20_common.h"

static int st20_tx_meta_build_rtp(tests_context* s, struct st20_rfc4175_rtp_hdr* rtp,
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
  if (s->fb_send % 2) {
    if (s->pkt_idx >= (s->total_pkts_in_frame / 2)) marker = true;
  }
  if (marker) {
    /* end of current frame */
    rtp->base.marker = 1;

    s->pkt_idx = 0;
    s->rtp_tmstamp++;
    s->fb_send++;
  }

  return 0;
}

static void st20_rx_meta_feed_packet(void* args) {
  auto ctx = (tests_context*)args;
  void* mbuf;
  void* usrptr = NULL;
  uint16_t mbuf_len = 0;
  std::unique_lock<std::mutex> lck(ctx->mtx, std::defer_lock);
  while (!ctx->stop) {
    /* get available buffer*/
    mbuf = st20_tx_get_mbuf((st20_tx_handle)ctx->handle, &usrptr);
    if (!mbuf) {
      lck.lock();
      /* try again */
      mbuf = st20_tx_get_mbuf((st20_tx_handle)ctx->handle, &usrptr);
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

    st20_tx_put_mbuf((st20_tx_handle)ctx->handle, mbuf, mbuf_len);
  }
}

static int st20_rx_meta_frame_ready(void* priv, void* frame,
                                    struct st20_rx_frame_meta* meta) {
  auto ctx = (tests_context*)priv;
  auto expect_meta = (struct st20_rx_frame_meta*)ctx->priv;

  if (!ctx->handle) return -EIO;

  ctx->fb_rec++;
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
  st20_rx_put_framebuff((st20_rx_handle)ctx->handle, frame);

  return 0;
}

static void st20_rx_meta_test(enum st_fps fps[], int width[], int height[],
                              enum st20_fmt fmt, int sessions = 1) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto m_handle = ctx->handle;
  int ret;
  struct st20_tx_ops ops_tx;
  struct st20_rx_ops ops_rx;
  if (ctx->para.num_ports < 2) {
    info(
        "%s, dual port should be enabled for tx test, one for tx and one for "
        "rx\n",
        __func__);
    throw std::runtime_error("Dual port not enabled");
  }

  std::vector<tests_context*> test_ctx_tx;
  std::vector<tests_context*> test_ctx_rx;
  std::vector<st20_tx_handle> tx_handle;
  std::vector<st20_rx_handle> rx_handle;
  std::vector<double> expect_framerate;
  std::vector<double> framerate;
  std::vector<std::thread> rtp_thread_tx;

  test_ctx_tx.resize(sessions);
  test_ctx_rx.resize(sessions);
  tx_handle.resize(sessions);
  rx_handle.resize(sessions);
  expect_framerate.resize(sessions);
  framerate.resize(sessions);
  rtp_thread_tx.resize(sessions);

  St20DeinitGuard guard(m_handle, test_ctx_tx, test_ctx_rx, tx_handle, rx_handle,
                        &rtp_thread_tx, nullptr);

  for (int i = 0; i < sessions; i++) {
    expect_framerate[i] = st_frame_rate(fps[i]);
    test_ctx_tx[i] = init_test_ctx(ctx, i, 3);
    ASSERT_TRUE(test_ctx_tx[i] != NULL);

    init_single_port_tx(ops_tx, test_ctx_tx[i], "st20_meta_test", udp_port_for_idx(i));
    ops_tx.pacing = ST21_PACING_NARROW;
    ops_tx.packing = ST20_PACKING_BPM;
    ops_tx.type = ST20_TYPE_RTP_LEVEL;
    ops_tx.width = width[i];
    ops_tx.height = height[i];
    ops_tx.fps = fps[i];
    ops_tx.fmt = fmt;
    ops_tx.payload_type = ST20_TEST_PAYLOAD_TYPE;
    ops_tx.framebuff_cnt = test_ctx_tx[i]->fb_cnt;
    ops_tx.get_next_frame = tx_next_video_frame;
    rtp_tx_specific_init(&ops_tx, test_ctx_tx[i]);
    tx_handle[i] = st20_tx_create(m_handle, &ops_tx);
    ASSERT_TRUE(tx_handle[i] != NULL);
    test_ctx_tx[i]->handle = tx_handle[i];
    test_ctx_tx[i]->stop = false;
    rtp_thread_tx[i] = std::thread(st20_rx_meta_feed_packet, test_ctx_tx[i]);
  }

  for (int i = 0; i < sessions; i++) {
    test_ctx_rx[i] = init_test_ctx(ctx, i, 3);
    ASSERT_TRUE(test_ctx_rx[i] != NULL);

    init_single_port_rx(ops_rx, test_ctx_rx[i], "st20_meta_test", udp_port_for_idx(i));
    ops_rx.type = ST20_TYPE_FRAME_LEVEL;
    ops_rx.width = width[i];
    ops_rx.height = height[i];
    ops_rx.fps = fps[i];
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

    test_ctx_rx[i]->handle = rx_handle[i];
  }

  ret = mtl_start(m_handle);
  EXPECT_GE(ret, 0);
  guard.set_started(ret >= 0);
  sleep(ST20_TRAIN_TIME_S * sessions); /* time for train_pacing */
  sleep(10);

  for (int i = 0; i < sessions; i++) {
    uint64_t cur_time_ns = st_test_get_monotonic_time();
    double time_sec = (double)(cur_time_ns - test_ctx_rx[i]->start_time) / NS_PER_S;
    framerate[i] = test_ctx_rx[i]->fb_rec / time_sec;
  }

  /* freeze counters before assertions */
  guard.stop();
  for (int i = 0; i < sessions; i++) {
    EXPECT_GT(test_ctx_rx[i]->fb_rec, 0);
    float expect_incomplete_frame_cnt = (float)test_ctx_rx[i]->fb_rec / 2;
    EXPECT_NEAR(test_ctx_rx[i]->incomplete_frame_cnt, expect_incomplete_frame_cnt,
                expect_incomplete_frame_cnt * 0.1);
    EXPECT_EQ(test_ctx_rx[i]->sha_fail_cnt, 0);
    EXPECT_EQ(test_ctx_rx[i]->rx_meta_fail_cnt, 0);
    info("%s, session %d fb_rec %d fb_incomplete %d framerate %f\n", __func__, i,
         test_ctx_rx[i]->fb_rec, test_ctx_rx[i]->incomplete_frame_cnt, framerate[i]);
    EXPECT_NEAR(framerate[i], expect_framerate[i], expect_framerate[i] * 0.1);
  }
}

TEST(St20_rx, frame_meta_1080p_fps59_94_s1) {
  enum st_fps fps[1] = {ST_FPS_P59_94};
  int width[1] = {1920};
  int height[1] = {1080};
  st20_rx_meta_test(fps, width, height, ST20_FMT_YUV_422_10BIT);
}