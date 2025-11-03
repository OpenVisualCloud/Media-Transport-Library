/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include <thread>

#include "log.h"
#include "tests.hpp"

#define ST40_TEST_PAYLOAD_TYPE (113)

static int tx_anc_next_frame(void* priv, uint16_t* next_frame_idx,
                             struct st40_tx_frame_meta* meta) {
  return tx_next_frame(priv, next_frame_idx);
}

static int tx_anc_next_frame_timestamp(void* priv, uint16_t* next_frame_idx,
                                       struct st40_tx_frame_meta* meta) {
  auto ctx = (tests_context*)priv;

  if (!ctx->handle) return -EIO; /* not ready */

  meta->tfmt = ST10_TIMESTAMP_FMT_TAI;
  meta->timestamp = mtl_ptp_read_time(ctx->ctx->handle) + 40 * 1000 * 1000;
  *next_frame_idx = ctx->fb_idx;
  dbg("%s, next_frame_idx %d\n", __func__, *next_frame_idx);
  ctx->fb_idx++;
  if (ctx->fb_idx >= ctx->fb_cnt) ctx->fb_idx = 0;
  ctx->fb_send++;
  if (!ctx->start_time) ctx->start_time = st_test_get_monotonic_time();
  return 0;
}

static int tx_anc_build_rtp_packet(tests_context* s, struct st40_rfc8331_rtp_hdr* rtp,
                                   uint16_t* pkt_len) {
  /* rtp hdr */
  memset(rtp, 0x0, sizeof(*rtp));
  rtp->base.marker = 1;
  rtp->first_hdr_chunk.anc_count = 0;
  rtp->base.payload_type = ST40_TEST_PAYLOAD_TYPE;
  rtp->base.version = 2;
  rtp->base.extension = 0;
  rtp->base.padding = 0;
  rtp->base.csrc_count = 0;
  rtp->first_hdr_chunk.f = 0b00;
  rtp->base.tmstamp = s->rtp_tmstamp;
  rtp->base.ssrc = htonl(0x88888888 + s->idx);
  /* update rtp seq*/
  rtp->base.seq_number = htons((uint16_t)s->seq_id);
  rtp->seq_number_ext = htons((uint16_t)(s->seq_id >> 16));
  s->rtp_tmstamp++;
  s->seq_id++;
  if (s->check_sha) {
    struct st40_rfc8331_payload_hdr* payload_hdr =
        (struct st40_rfc8331_payload_hdr*)(&rtp[1]);
    int total_size, payload_len, udw_size = s->frame_size;
    payload_hdr->first_hdr_chunk.c = 0;
    payload_hdr->first_hdr_chunk.line_number = 10;
    payload_hdr->first_hdr_chunk.horizontal_offset = 0;
    payload_hdr->first_hdr_chunk.s = 0;
    payload_hdr->first_hdr_chunk.stream_num = 0;
    payload_hdr->second_hdr_chunk.did = st40_add_parity_bits(0x43);
    payload_hdr->second_hdr_chunk.sdid = st40_add_parity_bits(0x02);
    payload_hdr->second_hdr_chunk.data_count = st40_add_parity_bits(udw_size);
    payload_hdr->swapped_first_hdr_chunk = htonl(payload_hdr->swapped_first_hdr_chunk);
    payload_hdr->swapped_second_hdr_chunk = htonl(payload_hdr->swapped_second_hdr_chunk);
    rtp->first_hdr_chunk.anc_count = 1;
    for (int i = 0; i < udw_size; i++) {
      st40_set_udw(i + 3,
                   st40_add_parity_bits(s->frame_buf[s->seq_id % TEST_SHA_HIST_NUM][i]),
                   (uint8_t*)&payload_hdr->second_hdr_chunk);
    }
    uint16_t check_sum =
        st40_calc_checksum(3 + udw_size, (uint8_t*)&payload_hdr->second_hdr_chunk);
    st40_set_udw(udw_size + 3, check_sum, (uint8_t*)&payload_hdr->second_hdr_chunk);
    total_size = ((3 + udw_size + 1) * 10) / 8;  // Calculate size of the
                                                 // 10-bit words: DID, SDID, DATA_COUNT
                                                 // + size of buffer with data + checksum
    total_size = (4 - total_size % 4) + total_size;  // Calculate word align to the 32-bit
                                                     // word of ANC data packet
    payload_len =
        sizeof(struct st40_rfc8331_payload_hdr) - 4 + total_size;  // Full size of one ANC
    rtp->length = htons(payload_len);
    *pkt_len = payload_len + sizeof(struct st40_rfc8331_rtp_hdr);
  } else {
    *pkt_len = sizeof(struct st40_rfc8331_rtp_hdr);
  }
  return 0;
}

static void tx_feed_packet(void* args) {
  auto ctx = (tests_context*)args;
  void* mbuf;
  void* usrptr = NULL;
  uint16_t mbuf_len = 0;
  std::unique_lock<std::mutex> lck(ctx->mtx, std::defer_lock);
  while (!ctx->stop) {
    /* get available buffer*/
    mbuf = st40_tx_get_mbuf((st40_tx_handle)ctx->handle, &usrptr);
    if (!mbuf) {
      lck.lock();
      /* try again */
      mbuf = st40_tx_get_mbuf((st40_tx_handle)ctx->handle, &usrptr);
      if (mbuf) {
        lck.unlock();
      } else {
        if (!ctx->stop) ctx->cv.wait(lck);
        lck.unlock();
        continue;
      }
    }

    /* build the rtp pkt */
    tx_anc_build_rtp_packet(ctx, (struct st40_rfc8331_rtp_hdr*)usrptr, &mbuf_len);
    st40_tx_put_mbuf((st40_tx_handle)ctx->handle, mbuf, mbuf_len);
  }
}

static int tx_rtp_done(void* args) {
  auto ctx = (tests_context*)args;

  if (!ctx->handle) return -EIO; /* not ready */

  std::unique_lock<std::mutex> lck(ctx->mtx);
  ctx->cv.notify_all();
  if (!ctx->start_time) ctx->start_time = st_test_get_monotonic_time();
  ctx->fb_send++;
  return 0;
}

static void rx_handle_rtp(tests_context* s, struct st40_rfc8331_rtp_hdr* hdr) {
  struct st40_rfc8331_payload_hdr* payload_hdr =
      (struct st40_rfc8331_payload_hdr*)(&hdr[1]);
  int anc_count = hdr->first_hdr_chunk.anc_count;
  int idx, total_size, payload_len;

  for (idx = 0; idx < anc_count; idx++) {
    payload_hdr->swapped_first_hdr_chunk = ntohl(payload_hdr->swapped_first_hdr_chunk);
    payload_hdr->swapped_second_hdr_chunk = ntohl(payload_hdr->swapped_second_hdr_chunk);
    if (!st40_check_parity_bits(payload_hdr->second_hdr_chunk.did) ||
        !st40_check_parity_bits(payload_hdr->second_hdr_chunk.sdid) ||
        !st40_check_parity_bits(payload_hdr->second_hdr_chunk.data_count)) {
      err("anc RTP checkParityBits for payload hdr error\n");
      s->rx_meta_fail_cnt++;
      return;
    }
    int udw_size = payload_hdr->second_hdr_chunk.data_count & 0xff;

    // verify checksum
    uint16_t checksum = 0;
    checksum = st40_get_udw(udw_size + 3, (uint8_t*)&payload_hdr->second_hdr_chunk);
    payload_hdr->swapped_second_hdr_chunk = htonl(payload_hdr->swapped_second_hdr_chunk);
    if (checksum !=
        st40_calc_checksum(3 + udw_size, (uint8_t*)&payload_hdr->second_hdr_chunk)) {
      s->sha_fail_cnt++;
      return;
    }
    // get payload
    uint16_t data;
    uint8_t* udw = (uint8_t*)st_test_zmalloc(udw_size);
    ASSERT_TRUE(udw != NULL);
    for (int i = 0; i < udw_size; i++) {
      data = st40_get_udw(i + 3, (uint8_t*)&payload_hdr->second_hdr_chunk);
      if (!st40_check_parity_bits(data)) {
        err("anc RTP checkParityBits for udw error\n");
        s->rx_meta_fail_cnt++;
      }
      udw[i] = data & 0xff;
    }
    {
      std::unique_lock<std::mutex> lck(s->mtx);
      s->buf_q.push(udw);
      s->cv.notify_all();
    }

    total_size = ((3 + udw_size + 1) * 10) / 8;  // Calculate size of the
                                                 // 10-bit words: DID, SDID, DATA_COUNT
                                                 // + size of buffer with data + checksum
    total_size = (4 - total_size % 4) + total_size;  // Calculate word align to the 32-bit
                                                     // word of ANC data packet
    payload_len =
        sizeof(struct st40_rfc8331_payload_hdr) - 4 + total_size;  // Full size of one ANC
    payload_hdr = (struct st40_rfc8331_payload_hdr*)((uint8_t*)payload_hdr + payload_len);
  }
}

static int rx_rtp_ready(void* priv) {
  auto ctx = (tests_context*)priv;
  void* useptr;
  void* mbuf;
  uint16_t len;

  if (!ctx->handle) return -EIO;

  while (1) {
    mbuf = st40_rx_get_mbuf((st40_rx_handle)ctx->handle, &useptr, &len);
    if (!mbuf) break; /* no mbuf */
    if (ctx->check_sha) {
      rx_handle_rtp(ctx, (struct st40_rfc8331_rtp_hdr*)useptr);
    }
    st40_rx_put_mbuf((st40_rx_handle)ctx->handle, mbuf);
    ctx->fb_rec++;
  }

  if (!ctx->start_time) ctx->start_time = st_test_get_monotonic_time();

  return 0;
}

static void st40_rx_ops_init(tests_context* st40, struct st40_rx_ops* ops) {
  auto ctx = st40->ctx;

  memset(ops, 0, sizeof(*ops));
  ops->name = "st40_test";
  ops->priv = st40;
  ops->num_port = ctx->para.num_ports;
  if (ctx->same_dual_port) ops->num_port = 1;
  memcpy(ops->ip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
         MTL_IP_ADDR_LEN);
  snprintf(ops->port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
           ctx->para.port[MTL_PORT_P]);
  ops->udp_port[MTL_SESSION_PORT_P] = 30000 + st40->idx * 2;
  if (ops->num_port == 2) {
    memcpy(ops->ip_addr[MTL_SESSION_PORT_R], ctx->mcast_ip_addr[MTL_PORT_R],
           MTL_IP_ADDR_LEN);
    snprintf(ops->port[MTL_SESSION_PORT_R], MTL_PORT_MAX_LEN, "%s",
             ctx->para.port[MTL_PORT_R]);
    ops->udp_port[MTL_SESSION_PORT_R] = 30000 + st40->idx * 2;
  }
  ops->notify_rtp_ready = rx_rtp_ready;
  ops->rtp_ring_size = 1024;
  ops->payload_type = ST40_TEST_PAYLOAD_TYPE;
}

static void st40_tx_ops_init(tests_context* st40, struct st40_tx_ops* ops) {
  auto ctx = st40->ctx;

  memset(ops, 0, sizeof(*ops));
  ops->name = "st40_test";
  ops->priv = st40;
  ops->num_port = ctx->para.num_ports;
  if (ctx->same_dual_port) ops->num_port = 1;
  memcpy(ops->dip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
         MTL_IP_ADDR_LEN);
  snprintf(ops->port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
           ctx->para.port[MTL_PORT_P]);
  ops->udp_port[MTL_SESSION_PORT_P] = 30000 + st40->idx * 2;
  if (ops->num_port == 2) {
    memcpy(ops->dip_addr[MTL_SESSION_PORT_R], ctx->mcast_ip_addr[MTL_PORT_R],
           MTL_IP_ADDR_LEN);
    snprintf(ops->port[MTL_SESSION_PORT_R], MTL_PORT_MAX_LEN, "%s",
             ctx->para.port[MTL_PORT_R]);
    ops->udp_port[MTL_SESSION_PORT_R] = 30000 + st40->idx * 2;
  }
  ops->type = ST40_TYPE_FRAME_LEVEL;
  ops->fps = ST_FPS_P59_94;
  ops->payload_type = ST40_TEST_PAYLOAD_TYPE;

  ops->framebuff_cnt = st40->fb_cnt;
  ops->get_next_frame = tx_anc_next_frame;
  ops->rtp_ring_size = 1024;
  ops->notify_rtp_done = tx_rtp_done;
}

static void st40_tx_assert_cnt(int expect_s40_tx_cnt) {
  auto ctx = st_test_ctx();
  auto handle = ctx->handle;
  struct st_var_info var;
  int ret;

  ret = st_get_var_info(handle, &var);
  EXPECT_GE(ret, 0);
  EXPECT_EQ(var.st40_tx_sessions_cnt, expect_s40_tx_cnt);
}

static void st40_rx_assert_cnt(int expect_s40_rx_cnt) {
  auto ctx = st_test_ctx();
  auto handle = ctx->handle;
  struct st_var_info var;
  int ret;

  ret = st_get_var_info(handle, &var);
  EXPECT_GE(ret, 0);
  EXPECT_EQ(var.st40_rx_sessions_cnt, expect_s40_rx_cnt);
}

TEST(St40_tx, create_free_single) {
  create_free_test(st40_tx, 0, 1, 1);
}
TEST(St40_tx, create_free_multi) {
  create_free_test(st40_tx, 0, 1, 6);
}
TEST(St40_tx, create_free_mix) {
  create_free_test(st40_tx, 2, 3, 4);
}
TEST(St40_tx, create_free_max) {
  create_free_max(st40_tx, TEST_CREATE_FREE_MAX);
}
TEST(St40_tx, create_expect_fail) {
  expect_fail_test(st40_tx);
}
TEST(St40_tx, create_expect_fail_ring_sz) {
  uint16_t ring_size = 0;
  expect_fail_test_rtp_ring(st40_tx, ST40_TYPE_RTP_LEVEL, ring_size);
  ring_size = 128 + 1;
  expect_fail_test_rtp_ring(st40_tx, ST40_TYPE_RTP_LEVEL, ring_size);
}
TEST(St40_tx, get_framebuffer) {
  uint16_t fbcnt = 3;
  test_get_framebuffer(st40_tx, fbcnt);
  fbcnt = 1000;
  test_get_framebuffer(st40_tx, fbcnt);
}
TEST(St40_tx, get_framebuffer_expect_fail) {
  uint16_t fbcnt = 3;
  expect_fail_test_get_framebuffer(st40_tx, fbcnt);
  fbcnt = 1000;
  expect_fail_test_get_framebuffer(st40_tx, fbcnt);
}

TEST(St40_rx, create_free_single) {
  create_free_test(st40_rx, 0, 1, 1);
}
TEST(St40_rx, create_free_multi) {
  create_free_test(st40_rx, 0, 1, 6);
}
TEST(St40_rx, create_free_mix) {
  create_free_test(st40_rx, 2, 3, 4);
}
TEST(St40_rx, create_free_max) {
  create_free_max(st40_rx, TEST_CREATE_FREE_MAX);
}
TEST(St40_rx, create_expect_fail) {
  expect_fail_test(st40_rx);
}
TEST(St40_rx, create_expect_fail_ring_sz) {
  uint16_t ring_size = 0;
  expect_fail_test_rtp_ring_2(st40_rx, ring_size);
  ring_size = 128 + 1;
  expect_fail_test_rtp_ring_2(st40_rx, ring_size);
}

static void st40_tx_frame_init(tests_context* st40, st40_tx_handle handle,
                               enum st40_type type) {
  size_t frame_size = 240;
  if (st40->st40_empty_frame) frame_size = 0;

  st40->pkt_data_len = frame_size;
  st40->frame_size = frame_size;

  for (int frame = 0; frame < st40->fb_cnt; frame++) {
    st40->frame_buf[frame] = (uint8_t*)st_test_zmalloc(frame_size);
    ASSERT_TRUE(st40->frame_buf[frame] != NULL);

    if (ST40_TYPE_FRAME_LEVEL == type) {
      struct st40_frame* dst = (struct st40_frame*)st40_tx_get_framebuffer(handle, frame);
      ASSERT_TRUE(dst != NULL);

      dst->data_size = dst->meta[0].udw_size = frame_size;
      dst->meta[0].udw_offset = 0;
      dst->meta[0].c = 0;
      dst->meta[0].line_number = 10;
      dst->meta[0].hori_offset = 0;
      dst->meta[0].s = 0;
      dst->meta[0].stream_num = 0;
      dst->meta[0].did = 0x43;
      dst->meta[0].sdid = 0x02;
      if (st40->st40_empty_frame)
        dst->meta_num = 0;
      else
        dst->meta_num = 1;
      dst->data = st40->frame_buf[frame];
    }
  }
}

static void st40_tx_frame_uinit(tests_context* st40) {
  for (int frame = 0; frame < st40->fb_cnt; frame++) {
    if (st40->frame_buf[frame]) {
      st_test_free(st40->frame_buf[frame]);
      st40->frame_buf[frame] = NULL;
    }
  }
}

static void st40_tx_fps_test(enum st40_type type[], enum st_fps fps[],
                             enum st_test_level level, int sessions = 1) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto m_handle = ctx->handle;
  int ret;
  struct st40_tx_ops ops;

  std::vector<tests_context*> test_ctx;
  std::vector<st40_tx_handle> handle;
  std::vector<double> expect_framerate;
  std::vector<double> framerate;
  std::vector<std::thread> rtp_thread;

  /* return if level small than global */
  if (level < ctx->level) return;

  test_ctx.resize(sessions);
  handle.resize(sessions);
  expect_framerate.resize(sessions);
  framerate.resize(sessions);
  rtp_thread.resize(sessions);

  for (int i = 0; i < sessions; i++) {
    expect_framerate[i] = st_frame_rate(fps[i]);
    test_ctx[i] = new tests_context();
    ASSERT_TRUE(test_ctx[i] != NULL);

    test_ctx[i]->idx = i;
    test_ctx[i]->ctx = ctx;
    test_ctx[i]->fb_cnt = 3;
    test_ctx[i]->fb_idx = 0;
    st40_tx_ops_init(test_ctx[i], &ops);
    ops.type = type[i];
    ops.fps = fps[i];

    handle[i] = st40_tx_create(m_handle, &ops);
    ASSERT_TRUE(handle[i] != NULL);

    st40_tx_frame_init(test_ctx[i], handle[i], type[i]);

    test_ctx[i]->handle = handle[i];

    if (type[i] == ST40_TYPE_RTP_LEVEL) {
      test_ctx[i]->stop = false;
      rtp_thread[i] = std::thread(tx_feed_packet, test_ctx[i]);
    }
  }

  ret = mtl_start(m_handle);
  EXPECT_GE(ret, 0);
  sleep(5);

  for (int i = 0; i < sessions; i++) {
    uint64_t cur_time_ns = st_test_get_monotonic_time();
    double time_sec = (double)(cur_time_ns - test_ctx[i]->start_time) / NS_PER_S;
    framerate[i] = test_ctx[i]->fb_send / time_sec;
    if (type[i] == ST40_TYPE_RTP_LEVEL) {
      test_ctx[i]->stop = true;
      {
        std::unique_lock<std::mutex> lck(test_ctx[i]->mtx);
        test_ctx[i]->cv.notify_all();
      }
      rtp_thread[i].join();
    }
  }

  ret = mtl_stop(m_handle);
  EXPECT_GE(ret, 0);

  for (int i = 0; i < sessions; i++) {
    EXPECT_GT(test_ctx[i]->fb_send, 0);
    info("%s, session %d fb_send %d framerate %f\n", __func__, i, test_ctx[i]->fb_send,
         framerate[i]);
    EXPECT_NEAR(framerate[i], expect_framerate[i], expect_framerate[i] * 0.1);
    ret = st40_tx_free(handle[i]);
    EXPECT_GE(ret, 0);
    st40_tx_frame_uinit(test_ctx[i]);
    delete test_ctx[i];
  }
}

static void st40_rx_fps_test(enum st40_type type[], enum st_fps fps[],
                             enum st_test_level level, int sessions = 1,
                             bool check_sha = false, bool user_timestamp = false,
                             bool empty_frame = false, bool interlaced = false,
                             bool dedicate_tx_queue = false) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto m_handle = ctx->handle;
  int ret;
  struct st40_tx_ops ops_tx;
  struct st40_rx_ops ops_rx;

  /* return if level small than global */
  if (level < ctx->level) return;

  if (ctx->para.num_ports < 2) {
    info(
        "%s, dual port should be enabled for tx test, one for tx and one for "
        "rx\n",
        __func__);
    throw std::runtime_error("Dual port not enabled");
  }

  std::vector<tests_context*> test_ctx_tx;
  std::vector<tests_context*> test_ctx_rx;
  std::vector<st40_tx_handle> tx_handle;
  std::vector<st40_rx_handle> rx_handle;
  std::vector<double> expect_framerate;
  std::vector<double> framerate;
  std::vector<std::thread> rtp_thread_tx;
  std::vector<std::thread> sha_check;

  test_ctx_tx.resize(sessions);
  test_ctx_rx.resize(sessions);
  tx_handle.resize(sessions);
  rx_handle.resize(sessions);
  expect_framerate.resize(sessions);
  framerate.resize(sessions);
  rtp_thread_tx.resize(sessions);
  sha_check.resize(sessions);

  for (int i = 0; i < sessions; i++) {
    test_ctx_tx[i] = new tests_context();
    ASSERT_TRUE(test_ctx_tx[i] != NULL);
    expect_framerate[i] = st_frame_rate(fps[i]);
    if (user_timestamp) expect_framerate[i] /= 2;

    test_ctx_tx[i]->idx = i;
    test_ctx_tx[i]->ctx = ctx;
    test_ctx_tx[i]->fb_cnt = TEST_SHA_HIST_NUM;
    test_ctx_tx[i]->fb_idx = 0;
    test_ctx_tx[i]->st40_empty_frame = empty_frame;
    memset(&ops_tx, 0, sizeof(ops_tx));
    ops_tx.name = "st40_test";
    ops_tx.priv = test_ctx_tx[i];
    ops_tx.num_port = 1;
    if (ctx->mcast_only)
      memcpy(ops_tx.dip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
             MTL_IP_ADDR_LEN);
    else
      memcpy(ops_tx.dip_addr[MTL_SESSION_PORT_P], ctx->para.sip_addr[MTL_PORT_R],
             MTL_IP_ADDR_LEN);
    snprintf(ops_tx.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
             ctx->para.port[MTL_PORT_P]);
    ops_tx.udp_port[MTL_SESSION_PORT_P] = 30000 + i * 2;
    ops_tx.type = type[i];
    ops_tx.fps = fps[i];
    ops_tx.payload_type = ST40_TEST_PAYLOAD_TYPE;
    ops_tx.interlaced = interlaced;
    ops_tx.ssrc = i ? i + 0x88888888 : 0;
    ops_tx.framebuff_cnt = test_ctx_tx[i]->fb_cnt;
    if (user_timestamp) {
      ops_tx.get_next_frame = tx_anc_next_frame_timestamp;
      ops_tx.flags |= ST40_TX_FLAG_USER_PACING;
    } else {
      ops_tx.get_next_frame = tx_anc_next_frame;
    }
    if (dedicate_tx_queue) ops_tx.flags |= ST40_TX_FLAG_DEDICATE_QUEUE;
    ops_tx.rtp_ring_size = 1024;
    ops_tx.notify_rtp_done = tx_rtp_done;

    tx_handle[i] = st40_tx_create(m_handle, &ops_tx);
    ASSERT_TRUE(tx_handle[i] != NULL);

    test_ctx_tx[i]->check_sha = check_sha;
    st40_tx_frame_init(test_ctx_tx[i], tx_handle[i], type[i]);
    if (check_sha) {
      uint8_t* fb;
      for (int frame = 0; frame < test_ctx_tx[i]->fb_cnt; frame++) {
        fb = test_ctx_tx[i]->frame_buf[frame];
        st_test_rand_data(fb, test_ctx_tx[i]->frame_size, frame);
        unsigned char* result = test_ctx_tx[i]->shas[frame];
        SHA256((unsigned char*)fb, test_ctx_tx[i]->frame_size, result);
        test_sha_dump("st40_rx", result);
      }
    }

    test_ctx_tx[i]->handle = tx_handle[i];

    if (type[i] == ST40_TYPE_RTP_LEVEL) {
      test_ctx_tx[i]->stop = false;
      rtp_thread_tx[i] = std::thread(tx_feed_packet, test_ctx_tx[i]);
    }
  }

  for (int i = 0; i < sessions; i++) {
    test_ctx_rx[i] = new tests_context();
    ASSERT_TRUE(test_ctx_rx[i] != NULL);

    test_ctx_rx[i]->idx = i;
    test_ctx_rx[i]->ctx = ctx;
    test_ctx_rx[i]->fb_cnt = 3;
    test_ctx_rx[i]->fb_idx = 0;
    memset(&ops_rx, 0, sizeof(ops_rx));
    ops_rx.name = "st40_test";
    ops_rx.priv = test_ctx_rx[i];
    ops_rx.num_port = 1;
    if (ctx->mcast_only)
      memcpy(ops_rx.ip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
             MTL_IP_ADDR_LEN);
    else
      memcpy(ops_rx.ip_addr[MTL_SESSION_PORT_P], ctx->para.sip_addr[MTL_PORT_P],
             MTL_IP_ADDR_LEN);
    snprintf(ops_rx.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
             ctx->para.port[MTL_PORT_R]);
    ops_rx.udp_port[MTL_SESSION_PORT_P] = 30000 + i * 2;
    ops_rx.notify_rtp_ready = rx_rtp_ready;
    ops_rx.rtp_ring_size = 1024;
    ops_rx.payload_type = ST40_TEST_PAYLOAD_TYPE;
    ops_rx.interlaced = interlaced;
    ops_rx.ssrc = i ? i + 0x88888888 : 0;
    rx_handle[i] = st40_rx_create(m_handle, &ops_rx);
    ASSERT_TRUE(rx_handle[i] != NULL);

    test_ctx_rx[i]->check_sha = check_sha;
    if (check_sha) {
      test_ctx_rx[i]->pkt_data_len = test_ctx_tx[i]->pkt_data_len;
      test_ctx_rx[i]->frame_size = test_ctx_rx[i]->pkt_data_len;
      memcpy(test_ctx_rx[i]->shas, test_ctx_tx[i]->shas,
             TEST_SHA_HIST_NUM * SHA256_DIGEST_LENGTH);
      sha_check[i] = std::thread(sha_frame_check, test_ctx_rx[i]);
    }

    test_ctx_rx[i]->handle = rx_handle[i];

    struct st_queue_meta meta;
    ret = st40_rx_get_queue_meta(rx_handle[i], &meta);
    EXPECT_GE(ret, 0);
  }

  ret = mtl_start(m_handle);
  EXPECT_GE(ret, 0);
  sleep(10);

  for (int i = 0; i < sessions; i++) {
    uint64_t cur_time_ns = st_test_get_monotonic_time();
    double time_sec = (double)(cur_time_ns - test_ctx_rx[i]->start_time) / NS_PER_S;
    framerate[i] = test_ctx_rx[i]->fb_rec / time_sec;
    if (type[i] == ST40_TYPE_RTP_LEVEL) {
      test_ctx_tx[i]->stop = true;
      {
        std::unique_lock<std::mutex> lck(test_ctx_tx[i]->mtx);
        test_ctx_tx[i]->cv.notify_all();
      }
      rtp_thread_tx[i].join();
    }
    if (check_sha) {
      test_ctx_rx[i]->stop = true;
      {
        std::unique_lock<std::mutex> lck(test_ctx_rx[i]->mtx);
        test_ctx_rx[i]->cv.notify_all();
      }
      sha_check[i].join();
      while (!test_ctx_rx[i]->buf_q.empty()) {
        void* frame = test_ctx_rx[i]->buf_q.front();
        st_test_free(frame);
        test_ctx_rx[i]->buf_q.pop();
      }
    }
  }

  ret = mtl_stop(m_handle);
  EXPECT_GE(ret, 0);
  for (int i = 0; i < sessions; i++) {
    EXPECT_GT(test_ctx_rx[i]->fb_rec, 0);
    info("%s, session %d fb_rec %d framerate %f\n", __func__, i, test_ctx_rx[i]->fb_rec,
         framerate[i]);
    EXPECT_NEAR(framerate[i], expect_framerate[i], expect_framerate[i] * 0.1);
    EXPECT_LE(test_ctx_rx[i]->sha_fail_cnt, 2);
    EXPECT_LE(test_ctx_rx[i]->rx_meta_fail_cnt, 2);
    ret = st40_tx_free(tx_handle[i]);
    EXPECT_GE(ret, 0);
    ret = st40_rx_free(rx_handle[i]);
    EXPECT_GE(ret, 0);
    if (check_sha) {
      EXPECT_GT(test_ctx_rx[i]->check_sha_frame_cnt, 0);
    }
    /* free all payload in buf_q */
    while (!test_ctx_rx[i]->buf_q.empty()) {
      void* frame = test_ctx_rx[i]->buf_q.front();
      st_test_free(frame);
      test_ctx_rx[i]->buf_q.pop();
    }
    st40_tx_frame_uinit(test_ctx_tx[i]);
    delete test_ctx_tx[i];
    delete test_ctx_rx[i];
  }
}

TEST(St40_tx, frame_fps59_94_s1) {
  enum st40_type type[1] = {ST40_TYPE_FRAME_LEVEL};
  enum st_fps fps[1] = {ST_FPS_P59_94};
  st40_tx_fps_test(type, fps, ST_TEST_LEVEL_ALL);
}
TEST(St40_tx, rtp_fps29_97_s1) {
  enum st40_type type[1] = {ST40_TYPE_RTP_LEVEL};
  enum st_fps fps[1] = {ST_FPS_P29_97};
  st40_tx_fps_test(type, fps, ST_TEST_LEVEL_ALL);
}
TEST(St40_tx, frame_fps50_s1) {
  enum st40_type type[1] = {ST40_TYPE_FRAME_LEVEL};
  enum st_fps fps[1] = {ST_FPS_P50};
  st40_tx_fps_test(type, fps, ST_TEST_LEVEL_ALL);
}
TEST(St40_tx, mix_fps59_94_s3) {
  enum st40_type type[3] = {ST40_TYPE_FRAME_LEVEL, ST40_TYPE_RTP_LEVEL,
                            ST40_TYPE_RTP_LEVEL};
  enum st_fps fps[3] = {ST_FPS_P59_94, ST_FPS_P59_94, ST_FPS_P59_94};
  st40_tx_fps_test(type, fps, ST_TEST_LEVEL_ALL, 3);
}
TEST(St40_tx, mix_fps29_97_s3) {
  enum st40_type type[3] = {ST40_TYPE_FRAME_LEVEL, ST40_TYPE_RTP_LEVEL,
                            ST40_TYPE_RTP_LEVEL};
  enum st_fps fps[3] = {ST_FPS_P29_97, ST_FPS_P29_97, ST_FPS_P29_97};
  st40_tx_fps_test(type, fps, ST_TEST_LEVEL_ALL, 3);
}
TEST(St40_tx, rtp_fps50_s3) {
  enum st40_type type[3] = {ST40_TYPE_RTP_LEVEL, ST40_TYPE_RTP_LEVEL,
                            ST40_TYPE_RTP_LEVEL};
  enum st_fps fps[3] = {ST_FPS_P50, ST_FPS_P50, ST_FPS_P50};
  st40_tx_fps_test(type, fps, ST_TEST_LEVEL_ALL, 3);
}

TEST(St40_tx, mix_fps50_fps29_97) {
  enum st40_type type[2] = {ST40_TYPE_FRAME_LEVEL, ST40_TYPE_RTP_LEVEL};
  enum st_fps fps[2] = {ST_FPS_P50, ST_FPS_P29_97};
  st40_tx_fps_test(type, fps, ST_TEST_LEVEL_ALL, 2);
}
TEST(St40_tx, mix_fps50_fps59_94) {
  enum st40_type type[2] = {ST40_TYPE_FRAME_LEVEL, ST40_TYPE_RTP_LEVEL};
  enum st_fps fps[2] = {ST_FPS_P50, ST_FPS_P59_94};
  st40_tx_fps_test(type, fps, ST_TEST_LEVEL_ALL, 2);
}
TEST(St40_tx, frame_fps29_97_fps59_94) {
  enum st40_type type[2] = {ST40_TYPE_FRAME_LEVEL, ST40_TYPE_FRAME_LEVEL};
  enum st_fps fps[2] = {ST_FPS_P29_97, ST_FPS_P59_94};
  st40_tx_fps_test(type, fps, ST_TEST_LEVEL_ALL, 2);
}
TEST(St40_rx, frame_fps29_97_fps59_94) {
  enum st40_type type[2] = {ST40_TYPE_RTP_LEVEL, ST40_TYPE_RTP_LEVEL};
  enum st_fps fps[2] = {ST_FPS_P29_97, ST_FPS_P59_94};
  st40_rx_fps_test(type, fps, ST_TEST_LEVEL_ALL, 2);
}
TEST(St40_rx, mix_s2) {
  enum st40_type type[2] = {ST40_TYPE_RTP_LEVEL, ST40_TYPE_FRAME_LEVEL};
  enum st_fps fps[2] = {ST_FPS_P50, ST_FPS_P59_94};
  st40_rx_fps_test(type, fps, ST_TEST_LEVEL_MANDATORY, 2, true, false, false, false,
                   true);
}
TEST(St40_rx, frame_fps50_fps59_94_digest) {
  enum st40_type type[2] = {ST40_TYPE_FRAME_LEVEL, ST40_TYPE_FRAME_LEVEL};
  enum st_fps fps[2] = {ST_FPS_P50, ST_FPS_P59_94};
  st40_rx_fps_test(type, fps, ST_TEST_LEVEL_ALL, 2, true);
}
TEST(St40_rx, rtp_fps50_fps59_94_digest) {
  enum st40_type type[2] = {ST40_TYPE_RTP_LEVEL, ST40_TYPE_RTP_LEVEL};
  enum st_fps fps[2] = {ST_FPS_P50, ST_FPS_P59_94};
  st40_rx_fps_test(type, fps, ST_TEST_LEVEL_ALL, 2, true);
}
TEST(St40_rx, frame_user_timestamp) {
  enum st40_type type[1] = {ST40_TYPE_FRAME_LEVEL};
  enum st_fps fps[1] = {ST_FPS_P59_94};
  st40_rx_fps_test(type, fps, ST_TEST_LEVEL_MANDATORY, 1, true, true);
}
TEST(St40_rx, frame_interlaced_empty) {
  enum st40_type type[1] = {ST40_TYPE_FRAME_LEVEL};
  enum st_fps fps[1] = {ST_FPS_P50};
  /* no sha check */
  st40_rx_fps_test(type, fps, ST_TEST_LEVEL_MANDATORY, 1, false, false, true, true);
}

static void st40_rx_update_src_test(enum st40_type type, int tx_sessions,
                                    enum st_test_level level) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto m_handle = ctx->handle;
  int ret;
  struct st40_tx_ops ops_tx;
  struct st40_rx_ops ops_rx;

  if (ctx->para.num_ports < 2) {
    info(
        "%s, dual port should be enabled for tx test, one for tx and one for "
        "rx\n",
        __func__);
    throw std::runtime_error("Dual port not enabled");
  }
  /* return if level lower than global */
  if (level < ctx->level) return;

  ASSERT_TRUE(tx_sessions >= 1);
  bool tx_update_dst = (tx_sessions == 1);

  int rx_sessions = 1;

  std::vector<tests_context*> test_ctx_tx;
  std::vector<tests_context*> test_ctx_rx;
  std::vector<st40_tx_handle> tx_handle;
  std::vector<st40_rx_handle> rx_handle;
  std::vector<double> expect_framerate;
  std::vector<double> framerate;
  std::vector<std::thread> rtp_thread_tx;

  test_ctx_tx.resize(tx_sessions);
  test_ctx_rx.resize(rx_sessions);
  tx_handle.resize(tx_sessions);
  rx_handle.resize(rx_sessions);
  expect_framerate.resize(rx_sessions);
  framerate.resize(rx_sessions);
  rtp_thread_tx.resize(tx_sessions);

  for (int i = 0; i < rx_sessions; i++)
    expect_framerate[i] = st_frame_rate(ST_FPS_P59_94);

  for (int i = 0; i < tx_sessions; i++) {
    test_ctx_tx[i] = new tests_context();
    ASSERT_TRUE(test_ctx_tx[i] != NULL);

    test_ctx_tx[i]->idx = i;
    test_ctx_tx[i]->ctx = ctx;
    test_ctx_tx[i]->fb_cnt = 3;
    test_ctx_tx[i]->fb_idx = 0;
    memset(&ops_tx, 0, sizeof(ops_tx));
    ops_tx.name = "st40_test";
    ops_tx.priv = test_ctx_tx[i];
    ops_tx.num_port = 1;
    if (2 == i)
      memcpy(ops_tx.dip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_R],
             MTL_IP_ADDR_LEN);
    else if (1 == i)
      memcpy(ops_tx.dip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
             MTL_IP_ADDR_LEN);
    else if (ctx->mcast_only)
      memcpy(ops_tx.dip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_2],
             MTL_IP_ADDR_LEN);
    else
      memcpy(ops_tx.dip_addr[MTL_SESSION_PORT_P], ctx->para.sip_addr[MTL_PORT_R],
             MTL_IP_ADDR_LEN);
    snprintf(ops_tx.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
             ctx->para.port[MTL_PORT_P]);
    ops_tx.udp_port[MTL_SESSION_PORT_P] = 30000 + i * 2;
    ops_tx.type = type;
    ops_tx.fps = ST_FPS_P59_94;
    ops_tx.payload_type = ST40_TEST_PAYLOAD_TYPE;
    ops_tx.framebuff_cnt = test_ctx_tx[i]->fb_cnt;
    ops_tx.get_next_frame = tx_anc_next_frame;
    ops_tx.notify_rtp_done = tx_rtp_done;
    ops_tx.rtp_ring_size = 1024;

    tx_handle[i] = st40_tx_create(m_handle, &ops_tx);
    ASSERT_TRUE(tx_handle[i] != NULL);

    st40_tx_frame_init(test_ctx_tx[i], tx_handle[i], type);

    test_ctx_tx[i]->handle = tx_handle[i];

    if (type == ST40_TYPE_RTP_LEVEL) {
      test_ctx_tx[i]->stop = false;
      rtp_thread_tx[i] = std::thread(tx_feed_packet, test_ctx_tx[i]);
    }
  }

  for (int i = 0; i < rx_sessions; i++) {
    test_ctx_rx[i] = new tests_context();
    ASSERT_TRUE(test_ctx_rx[i] != NULL);

    test_ctx_rx[i]->idx = i;
    test_ctx_rx[i]->ctx = ctx;
    test_ctx_rx[i]->fb_cnt = 3;
    test_ctx_rx[i]->fb_idx = 0;
    memset(&ops_rx, 0, sizeof(ops_rx));
    ops_rx.name = "st40_test";
    ops_rx.priv = test_ctx_rx[i];
    ops_rx.num_port = 1;
    if (ctx->mcast_only)
      memcpy(ops_rx.ip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_2],
             MTL_IP_ADDR_LEN);
    else
      memcpy(ops_rx.ip_addr[MTL_SESSION_PORT_P], ctx->para.sip_addr[MTL_PORT_P],
             MTL_IP_ADDR_LEN);
    snprintf(ops_rx.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
             ctx->para.port[MTL_PORT_R]);
    ops_rx.udp_port[MTL_SESSION_PORT_P] = 30000 + i * 2;
    ops_rx.notify_rtp_ready = rx_rtp_ready;
    ops_rx.rtp_ring_size = 1024;
    ops_rx.payload_type = ST40_TEST_PAYLOAD_TYPE;

    rx_handle[i] = st40_rx_create(m_handle, &ops_rx);
    test_ctx_rx[i]->handle = rx_handle[i];
    ASSERT_TRUE(rx_handle[i] != NULL);
  }

  ret = mtl_start(m_handle);
  EXPECT_GE(ret, 0);
  sleep(10);

  struct st_rx_source_info src;
  /* switch to mcast port p(tx_session:1) */
  memset(&src, 0, sizeof(src));
  src.udp_port[MTL_SESSION_PORT_P] = 30000 + 2;
  memcpy(src.ip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
         MTL_IP_ADDR_LEN);
  if (tx_update_dst) {
    test_ctx_tx[0]->seq_id = 0; /* reset seq id */
    struct st_tx_dest_info dst;
    memset(&dst, 0, sizeof(dst));
    dst.udp_port[MTL_SESSION_PORT_P] = 30000 + 2;
    memcpy(dst.dip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
           MTL_IP_ADDR_LEN);
    ret = st40_tx_update_destination(tx_handle[0], &dst);
    EXPECT_GE(ret, 0);
  } else {
    test_ctx_tx[1]->seq_id = 0; /* reset seq id */
  }
  for (int i = 0; i < rx_sessions; i++) {
    ret = st40_rx_update_source(rx_handle[i], &src);
    EXPECT_GE(ret, 0);
    test_ctx_rx[i]->start_time = 0;
    test_ctx_rx[i]->fb_rec = 0;
  }
  sleep(10);
  /* check rx fps */
  for (int i = 0; i < rx_sessions; i++) {
    uint64_t cur_time_ns = st_test_get_monotonic_time();
    double time_sec = (double)(cur_time_ns - test_ctx_rx[i]->start_time) / NS_PER_S;
    framerate[i] = test_ctx_rx[i]->fb_rec / time_sec;

    EXPECT_GT(test_ctx_rx[i]->fb_rec, 0);
    info("%s, session %d fb_rec %d framerate %f for mcast 1\n", __func__, i,
         test_ctx_rx[i]->fb_rec, framerate[i]);
    EXPECT_NEAR(framerate[i], expect_framerate[i], expect_framerate[i] * 0.1);
  }

  if (tx_sessions > 2) {
    /* switch to mcast port r(tx_session:2) */
    memset(&src, 0, sizeof(src));
    src.udp_port[MTL_SESSION_PORT_P] = 30000 + 4;
    memcpy(src.ip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_R],
           MTL_IP_ADDR_LEN);
    for (int i = 0; i < rx_sessions; i++) {
      ret = st40_rx_update_source(rx_handle[i], &src);
      EXPECT_GE(ret, 0);
      test_ctx_tx[2]->seq_id = rand(); /* random seq id */
      test_ctx_rx[i]->start_time = 0;
      test_ctx_rx[i]->fb_rec = 0;
    }
    sleep(10);
    /* check rx fps */
    for (int i = 0; i < rx_sessions; i++) {
      uint64_t cur_time_ns = st_test_get_monotonic_time();
      double time_sec = (double)(cur_time_ns - test_ctx_rx[i]->start_time) / NS_PER_S;
      framerate[i] = test_ctx_rx[i]->fb_rec / time_sec;

      EXPECT_GT(test_ctx_rx[i]->fb_rec, 0);
      info("%s, session %d fb_rec %d framerate %f for mcast 2\n", __func__, i,
           test_ctx_rx[i]->fb_rec, framerate[i]);
      EXPECT_NEAR(framerate[i], expect_framerate[i], expect_framerate[i] * 0.1);
    }
  }

  /* switch to unicast(tx_session:0) */
  memset(&src, 0, sizeof(src));
  src.udp_port[MTL_SESSION_PORT_P] = 30000 + 0;
  memcpy(src.ip_addr[MTL_SESSION_PORT_P], ctx->para.sip_addr[MTL_PORT_P],
         MTL_IP_ADDR_LEN);
  test_ctx_tx[0]->seq_id = rand(); /* random seq id */
  if (tx_update_dst) {
    struct st_tx_dest_info dst;
    memset(&dst, 0, sizeof(dst));
    dst.udp_port[MTL_SESSION_PORT_P] = 30000 + 0;
    memcpy(dst.dip_addr[MTL_SESSION_PORT_P], ctx->para.sip_addr[MTL_PORT_R],
           MTL_IP_ADDR_LEN);
    ret = st40_tx_update_destination(tx_handle[0], &dst);
    EXPECT_GE(ret, 0);
  }
  for (int i = 0; i < rx_sessions; i++) {
    ret = st40_rx_update_source(rx_handle[i], &src);
    EXPECT_GE(ret, 0);
    test_ctx_rx[i]->start_time = 0;
    test_ctx_rx[i]->fb_rec = 0;
  }
  sleep(10);
  /* check rx fps */
  for (int i = 0; i < rx_sessions; i++) {
    uint64_t cur_time_ns = st_test_get_monotonic_time();
    double time_sec = (double)(cur_time_ns - test_ctx_rx[i]->start_time) / NS_PER_S;
    framerate[i] = test_ctx_rx[i]->fb_rec / time_sec;

    EXPECT_GT(test_ctx_rx[i]->fb_rec, 0);
    info("%s, session %d fb_rec %d framerate %f for unicast 0\n", __func__, i,
         test_ctx_rx[i]->fb_rec, framerate[i]);
    EXPECT_NEAR(framerate[i], expect_framerate[i], expect_framerate[i] * 0.1);
  }

  /* stop rtp thread */
  for (int i = 0; i < tx_sessions; i++) {
    if (type == ST40_TYPE_RTP_LEVEL) {
      test_ctx_tx[i]->stop = true;
      {
        std::unique_lock<std::mutex> lck(test_ctx_tx[i]->mtx);
        test_ctx_tx[i]->cv.notify_all();
      }
      rtp_thread_tx[i].join();
    }
  }

  ret = mtl_stop(m_handle);
  EXPECT_GE(ret, 0);

  /* free all tx and rx */
  for (int i = 0; i < rx_sessions; i++) {
    ret = st40_rx_free(rx_handle[i]);
    EXPECT_GE(ret, 0);
    delete test_ctx_rx[i];
  }
  for (int i = 0; i < tx_sessions; i++) {
    ret = st40_tx_free(tx_handle[i]);
    EXPECT_GE(ret, 0);
    st40_tx_frame_uinit(test_ctx_tx[i]);
    delete test_ctx_tx[i];
  }
}

TEST(St40_rx, update_source_rtp) {
  st40_rx_update_src_test(ST40_TYPE_RTP_LEVEL, 3, ST_TEST_LEVEL_ALL);
}
TEST(St40_tx, update_dest_rtp) {
  st40_rx_update_src_test(ST40_TYPE_RTP_LEVEL, 1, ST_TEST_LEVEL_ALL);
}

static void st40_after_start_test(enum st40_type type[], enum st_fps fps[], int sessions,
                                  int repeat) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto m_handle = ctx->handle;
  int ret;
  struct st40_tx_ops ops_tx;
  struct st40_rx_ops ops_rx;

  if (ctx->para.num_ports < 2) {
    info(
        "%s, dual port should be enabled for tx test, one for tx and one for "
        "rx\n",
        __func__);
    throw std::runtime_error("Dual port not enabled");
  }

  std::vector<tests_context*> test_ctx_tx;
  std::vector<tests_context*> test_ctx_rx;
  std::vector<st40_tx_handle> tx_handle;
  std::vector<st40_rx_handle> rx_handle;
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

  ret = mtl_start(m_handle);
  EXPECT_GE(ret, 0);

  for (int r = 0; r < repeat; r++) {
    for (int i = 0; i < sessions; i++) {
      test_ctx_tx[i] = new tests_context();
      ASSERT_TRUE(test_ctx_tx[i] != NULL);
      expect_framerate[i] = st_frame_rate(fps[i]);

      test_ctx_tx[i]->idx = i;
      test_ctx_tx[i]->ctx = ctx;
      test_ctx_tx[i]->fb_cnt = 3;
      test_ctx_tx[i]->fb_idx = 0;
      memset(&ops_tx, 0, sizeof(ops_tx));
      ops_tx.name = "st40_test";
      ops_tx.priv = test_ctx_tx[i];
      ops_tx.num_port = 1;
      if (ctx->mcast_only)
        memcpy(ops_tx.dip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
               MTL_IP_ADDR_LEN);
      else
        memcpy(ops_tx.dip_addr[MTL_SESSION_PORT_P], ctx->para.sip_addr[MTL_PORT_R],
               MTL_IP_ADDR_LEN);
      snprintf(ops_tx.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
               ctx->para.port[MTL_PORT_P]);
      ops_tx.udp_port[MTL_SESSION_PORT_P] = 30000 + i * 2;
      ops_tx.type = type[i];
      ops_tx.fps = fps[i];
      ops_tx.payload_type = ST40_TEST_PAYLOAD_TYPE;
      ops_tx.framebuff_cnt = test_ctx_tx[i]->fb_cnt;
      ops_tx.get_next_frame = tx_anc_next_frame;
      ops_tx.rtp_ring_size = 1024;
      ops_tx.notify_rtp_done = tx_rtp_done;

      tx_handle[i] = st40_tx_create(m_handle, &ops_tx);
      ASSERT_TRUE(tx_handle[i] != NULL);

      st40_tx_frame_init(test_ctx_tx[i], tx_handle[i], type[i]);

      test_ctx_tx[i]->handle = tx_handle[i];

      if (type[i] == ST40_TYPE_RTP_LEVEL) {
        test_ctx_tx[i]->stop = false;
        rtp_thread_tx[i] = std::thread(tx_feed_packet, test_ctx_tx[i]);
      }
    }

    for (int i = 0; i < sessions; i++) {
      test_ctx_rx[i] = new tests_context();
      ASSERT_TRUE(test_ctx_rx[i] != NULL);

      test_ctx_rx[i]->idx = i;
      test_ctx_rx[i]->ctx = ctx;
      test_ctx_rx[i]->fb_cnt = 3;
      test_ctx_rx[i]->fb_idx = 0;
      memset(&ops_rx, 0, sizeof(ops_rx));
      ops_rx.name = "st40_test";
      ops_rx.priv = test_ctx_rx[i];
      ops_rx.num_port = 1;
      if (ctx->mcast_only)
        memcpy(ops_rx.ip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
               MTL_IP_ADDR_LEN);
      else
        memcpy(ops_rx.ip_addr[MTL_SESSION_PORT_P], ctx->para.sip_addr[MTL_PORT_P],
               MTL_IP_ADDR_LEN);
      snprintf(ops_rx.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
               ctx->para.port[MTL_PORT_R]);
      ops_rx.udp_port[MTL_SESSION_PORT_P] = 30000 + i * 2;
      ops_rx.notify_rtp_ready = rx_rtp_ready;
      ops_rx.rtp_ring_size = 1024;
      ops_rx.payload_type = ST40_TEST_PAYLOAD_TYPE;
      rx_handle[i] = st40_rx_create(m_handle, &ops_rx);
      ASSERT_TRUE(rx_handle[i] != NULL);

      test_ctx_rx[i]->handle = rx_handle[i];
    }

    sleep(10);

    for (int i = 0; i < sessions; i++) {
      uint64_t cur_time_ns = st_test_get_monotonic_time();
      double time_sec = (double)(cur_time_ns - test_ctx_rx[i]->start_time) / NS_PER_S;
      framerate[i] = test_ctx_rx[i]->fb_rec / time_sec;
      if (type[i] == ST40_TYPE_RTP_LEVEL) {
        test_ctx_tx[i]->stop = true;
        {
          std::unique_lock<std::mutex> lck(test_ctx_tx[i]->mtx);
          test_ctx_tx[i]->cv.notify_all();
        }
        rtp_thread_tx[i].join();
      }
    }

    /* check fps */
    for (int i = 0; i < sessions; i++) {
      EXPECT_GT(test_ctx_rx[i]->fb_rec, 0);
      info("%s, session %d fb_rec %d framerate %f\n", __func__, i, test_ctx_rx[i]->fb_rec,
           framerate[i]);
      EXPECT_NEAR(framerate[i], expect_framerate[i], expect_framerate[i] * 0.1);
      ret = st40_tx_free(tx_handle[i]);
      EXPECT_GE(ret, 0);
      st40_tx_frame_uinit(test_ctx_tx[i]);
      delete test_ctx_tx[i];
      ret = st40_rx_free(rx_handle[i]);
      EXPECT_GE(ret, 0);
      delete test_ctx_rx[i];
    }
  }

  ret = mtl_stop(m_handle);
  EXPECT_GE(ret, 0);
}

TEST(St40_rx, after_start_mix_s2_r2) {
  enum st40_type type[2] = {ST40_TYPE_RTP_LEVEL, ST40_TYPE_FRAME_LEVEL};
  enum st_fps fps[2] = {ST_FPS_P50, ST_FPS_P59_94};
  st40_after_start_test(type, fps, 2, 2);
}
