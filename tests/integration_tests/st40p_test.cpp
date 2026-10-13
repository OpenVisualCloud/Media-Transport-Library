/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include <algorithm>
#include <thread>
#include <vector>

#include "log.h"
#include "tests.hpp"

/* st40_pipeline_api.h is not included by tests.hpp, pull it in for st40p + is_late */
#include <mtl/st40_pipeline_api.h>

#define ABORT_TEST_UDP_PORT (40000)

TEST(St40p, tx_put_frame_abort) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto st = ctx->handle;
  int ret;
  struct st40p_tx_ops ops_tx;

  auto test_ctx = new tests_context();
  ASSERT_TRUE(test_ctx != NULL);
  test_ctx->idx = 0;
  test_ctx->ctx = ctx;
  test_ctx->fb_cnt = 3;

  memset(&ops_tx, 0, sizeof(ops_tx));
  ops_tx.name = "st40p_abort_test";
  ops_tx.priv = test_ctx;
  ops_tx.port.num_port = 1;
  memcpy(ops_tx.port.dip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
         MTL_IP_ADDR_LEN);
  snprintf(ops_tx.port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
           ctx->para.port[MTL_PORT_P]);
  ops_tx.port.udp_port[MTL_SESSION_PORT_P] = ABORT_TEST_UDP_PORT + 12;
  ops_tx.port.payload_type = 113;
  ops_tx.fps = ST_FPS_P59_94;
  ops_tx.interlaced = false;
  ops_tx.framebuff_cnt = test_ctx->fb_cnt;
  ops_tx.max_udw_buff_size = 1024;
  ops_tx.flags |= ST40P_TX_FLAG_BLOCK_GET;

  auto tx_handle = st40p_tx_create(st, &ops_tx);
  ASSERT_TRUE(tx_handle != NULL);
  ret = st40p_tx_set_block_timeout(tx_handle, NS_PER_S);
  EXPECT_EQ(ret, 0);

  struct st40_frame_info* frame_info = st40p_tx_get_frame(tx_handle);
  if (frame_info) {
    ret = st40p_tx_put_frame_abort(tx_handle, frame_info);
    EXPECT_GE(ret, 0);
    info("%s, st40p_tx_put_frame_abort succeeded\n", __func__);
  } else {
    info("%s, no frame available for st40p TX abort test\n", __func__);
  }

  ret = st40p_tx_free(tx_handle);
  EXPECT_GE(ret, 0);
  delete test_ctx;
}

TEST(St40p, rx_put_frame_abort) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto st = ctx->handle;
  int ret;
  struct st40p_tx_ops ops_tx;
  struct st40p_rx_ops ops_rx;

  if (ctx->para.num_ports < 2) {
    info("%s, dual port should be enabled\n", __func__);
    return;
  }

  /* create TX to feed data */
  auto test_ctx_tx = new tests_context();
  ASSERT_TRUE(test_ctx_tx != NULL);
  test_ctx_tx->idx = 0;
  test_ctx_tx->ctx = ctx;
  test_ctx_tx->fb_cnt = 3;

  memset(&ops_tx, 0, sizeof(ops_tx));
  ops_tx.name = "st40p_abort_tx";
  ops_tx.priv = test_ctx_tx;
  ops_tx.port.num_port = 1;
  memcpy(ops_tx.port.dip_addr[MTL_SESSION_PORT_P], ctx->para.sip_addr[MTL_PORT_R],
         MTL_IP_ADDR_LEN);
  snprintf(ops_tx.port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
           ctx->para.port[MTL_PORT_P]);
  ops_tx.port.udp_port[MTL_SESSION_PORT_P] = ABORT_TEST_UDP_PORT + 14;
  ops_tx.port.payload_type = 113;
  ops_tx.fps = ST_FPS_P59_94;
  ops_tx.interlaced = false;
  ops_tx.framebuff_cnt = test_ctx_tx->fb_cnt;
  ops_tx.max_udw_buff_size = 1024;
  ops_tx.flags |= ST40P_TX_FLAG_BLOCK_GET;

  auto tx_handle = st40p_tx_create(st, &ops_tx);
  ASSERT_TRUE(tx_handle != NULL);

  test_ctx_tx->handle = tx_handle;
  test_ctx_tx->stop = false;
  auto tx_thread = std::thread(
      [](tests_context* s) {
        auto handle = (st40p_tx_handle)s->handle;
        while (!s->stop) {
          auto frame_info = st40p_tx_get_frame(handle);
          if (!frame_info) continue;
          /* fill minimal ANC metadata */
          frame_info->meta_num = 0;
          frame_info->udw_buffer_fill = 0;
          st40p_tx_put_frame(handle, frame_info);
          s->fb_send++;
        }
      },
      test_ctx_tx);

  /* create RX */
  auto test_ctx_rx = new tests_context();
  ASSERT_TRUE(test_ctx_rx != NULL);
  test_ctx_rx->idx = 0;
  test_ctx_rx->ctx = ctx;
  test_ctx_rx->fb_cnt = 3;

  memset(&ops_rx, 0, sizeof(ops_rx));
  ops_rx.name = "st40p_abort_rx";
  ops_rx.priv = test_ctx_rx;
  ops_rx.port.num_port = 1;
  memcpy(ops_rx.port.ip_addr[MTL_SESSION_PORT_P], ctx->para.sip_addr[MTL_PORT_P],
         MTL_IP_ADDR_LEN);
  snprintf(ops_rx.port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
           ctx->para.port[MTL_PORT_R]);
  ops_rx.port.udp_port[MTL_SESSION_PORT_P] = ABORT_TEST_UDP_PORT + 14;
  ops_rx.port.payload_type = 113;
  ops_rx.interlaced = false;
  ops_rx.framebuff_cnt = test_ctx_rx->fb_cnt;
  ops_rx.max_udw_buff_size = 1024;
  ops_rx.rtp_ring_size = 1024;
  ops_rx.flags |= ST40P_RX_FLAG_BLOCK_GET;

  auto rx_handle = st40p_rx_create(st, &ops_rx);
  ASSERT_TRUE(rx_handle != NULL);
  ret = st40p_rx_set_block_timeout(rx_handle, NS_PER_S);
  EXPECT_EQ(ret, 0);

  ret = mtl_start(st);
  EXPECT_GE(ret, 0);
  sleep(2);

  struct st40_frame_info* frame_info = st40p_rx_get_frame(rx_handle);
  if (frame_info) {
    ret = st40p_rx_put_frame_abort(rx_handle, frame_info);
    EXPECT_GE(ret, 0);
    info("%s, st40p_rx_put_frame_abort succeeded\n", __func__);
  } else {
    info("%s, no rx frame available for st40p abort test\n", __func__);
  }

  test_ctx_tx->stop = true;
  st40p_tx_wake_block(tx_handle);
  tx_thread.join();

  ret = st40p_tx_free(tx_handle);
  EXPECT_GE(ret, 0);
  ret = st40p_rx_free(rx_handle);
  EXPECT_GE(ret, 0);

  delete test_ctx_tx;
  delete test_ctx_rx;
}

TEST(St40p, frame_is_late) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto st = ctx->handle;

  struct st40_frame_info frame_info;
  memset(&frame_info, 0, sizeof(frame_info));
  frame_info.tfmt = ST10_TIMESTAMP_FMT_TAI;
  uint64_t now = mtl_ptp_read_time(st);
  frame_info.timestamp = now - NS_PER_S;
  EXPECT_TRUE(st40_frame_is_late(st, &frame_info));

  frame_info.timestamp = now + (uint64_t)10 * NS_PER_S;
  EXPECT_FALSE(st40_frame_is_late(st, &frame_info));

  frame_info.tfmt = ST10_TIMESTAMP_FMT_MEDIA_CLK;
  frame_info.timestamp = 0;
  EXPECT_FALSE(st40_frame_is_late(st, &frame_info));

  info("%s, st40_frame_is_late tests passed\n", __func__);
}

/*
 * Verify redundant TX and RX port stats for st40p ancillary sessions.
 * TX frames must be counted on both ports.
 * RX bytes must be approximately equal on both ports.
 * Requires 4 ports: TX on ports 0+1, RX on ports 2+3.
 */
TEST(St40p, redundant_stats) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto st = ctx->handle;
  int ret;

  if (ctx->para.num_ports < 4) {
    info("%s, need 4 ports for redundant test, skipping\n", __func__);
    return;
  }

  struct st40p_tx_ops ops_tx;
  struct st40p_rx_ops ops_rx;
  int udp_port = ABORT_TEST_UDP_PORT + 100;

  /* TX context */
  auto test_ctx_tx = new tests_context();
  ASSERT_TRUE(test_ctx_tx != NULL);
  test_ctx_tx->idx = 0;
  test_ctx_tx->ctx = ctx;
  test_ctx_tx->fb_cnt = 3;

  memset(&ops_tx, 0, sizeof(ops_tx));
  ops_tx.name = "st40p_redundant_tx";
  ops_tx.priv = test_ctx_tx;
  ops_tx.port.num_port = 2;
  /* primary TX port */
  memcpy(ops_tx.port.dip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
         MTL_IP_ADDR_LEN);
  snprintf(ops_tx.port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
           ctx->para.port[MTL_PORT_P]);
  ops_tx.port.udp_port[MTL_SESSION_PORT_P] = udp_port;
  /* redundant TX port */
  memcpy(ops_tx.port.dip_addr[MTL_SESSION_PORT_R], ctx->mcast_ip_addr[MTL_PORT_R],
         MTL_IP_ADDR_LEN);
  snprintf(ops_tx.port.port[MTL_SESSION_PORT_R], MTL_PORT_MAX_LEN, "%s",
           ctx->para.port[MTL_PORT_R]);
  ops_tx.port.udp_port[MTL_SESSION_PORT_R] = udp_port;
  ops_tx.port.payload_type = 113;
  ops_tx.fps = ST_FPS_P59_94;
  ops_tx.interlaced = false;
  ops_tx.framebuff_cnt = test_ctx_tx->fb_cnt;
  ops_tx.max_udw_buff_size = 1024;
  ops_tx.flags |= ST40P_TX_FLAG_BLOCK_GET;

  auto tx_handle = st40p_tx_create(st, &ops_tx);
  ASSERT_TRUE(tx_handle != NULL);
  ret = st40p_tx_set_block_timeout(tx_handle, NS_PER_S);
  EXPECT_EQ(ret, 0);

  test_ctx_tx->handle = tx_handle;
  test_ctx_tx->stop = false;
  auto tx_thread = std::thread(
      [](tests_context* s) {
        auto handle = (st40p_tx_handle)s->handle;
        while (!s->stop) {
          auto fi = st40p_tx_get_frame(handle);
          if (!fi) continue;
          fi->meta_num = 0;
          fi->udw_buffer_fill = 0;
          st40p_tx_put_frame(handle, fi);
          s->fb_send++;
        }
      },
      test_ctx_tx);

  /* RX context */
  auto test_ctx_rx = new tests_context();
  ASSERT_TRUE(test_ctx_rx != NULL);
  test_ctx_rx->idx = 0;
  test_ctx_rx->ctx = ctx;
  test_ctx_rx->fb_cnt = 3;

  memset(&ops_rx, 0, sizeof(ops_rx));
  ops_rx.name = "st40p_redundant_rx";
  ops_rx.priv = test_ctx_rx;
  ops_rx.port.num_port = 2;
  /* primary RX port */
  memcpy(ops_rx.port.ip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
         MTL_IP_ADDR_LEN);
  snprintf(ops_rx.port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
           ctx->para.port[MTL_PORT_2]);
  ops_rx.port.udp_port[MTL_SESSION_PORT_P] = udp_port;
  /* redundant RX port */
  memcpy(ops_rx.port.ip_addr[MTL_SESSION_PORT_R], ctx->mcast_ip_addr[MTL_PORT_R],
         MTL_IP_ADDR_LEN);
  snprintf(ops_rx.port.port[MTL_SESSION_PORT_R], MTL_PORT_MAX_LEN, "%s",
           ctx->para.port[MTL_PORT_3]);
  ops_rx.port.udp_port[MTL_SESSION_PORT_R] = udp_port;
  ops_rx.port.payload_type = 113;
  ops_rx.interlaced = false;
  ops_rx.framebuff_cnt = test_ctx_rx->fb_cnt;
  ops_rx.max_udw_buff_size = 1024;
  ops_rx.rtp_ring_size = 1024;
  ops_rx.flags |= ST40P_RX_FLAG_BLOCK_GET;

  auto rx_handle = st40p_rx_create(st, &ops_rx);
  ASSERT_TRUE(rx_handle != NULL);
  ret = st40p_rx_set_block_timeout(rx_handle, NS_PER_S);
  EXPECT_EQ(ret, 0);

  test_ctx_rx->handle = rx_handle;
  test_ctx_rx->stop = false;
  auto rx_thread = std::thread(
      [](tests_context* s) {
        auto handle = (st40p_rx_handle)s->handle;
        while (!s->stop) {
          auto fi = st40p_rx_get_frame(handle);
          if (!fi) continue;
          st40p_rx_put_frame(handle, fi);
          s->fb_rec++;
        }
      },
      test_ctx_rx);

  ret = mtl_start(st);
  EXPECT_GE(ret, 0);
  sleep(10);

  /* stop threads */
  test_ctx_tx->stop = true;
  st40p_tx_wake_block(tx_handle);
  tx_thread.join();

  test_ctx_rx->stop = true;
  st40p_rx_wake_block(rx_handle);
  rx_thread.join();

  /* query stats */
  struct st40_tx_user_stats tx_stats;
  ret = st40p_tx_get_session_stats(tx_handle, &tx_stats);
  EXPECT_GE(ret, 0);

  struct st40_rx_user_stats rx_stats;
  ret = st40p_rx_get_session_stats(rx_handle, &rx_stats);
  EXPECT_GE(ret, 0);

  /* verify TX frames on both ports */
  uint64_t tx_frames_p = tx_stats.common.port[MTL_SESSION_PORT_P].frames;
  uint64_t tx_frames_r = tx_stats.common.port[MTL_SESSION_PORT_R].frames;
  EXPECT_GT(tx_frames_p, (uint64_t)0) << "TX port 0 frames must be > 0";
  EXPECT_GT(tx_frames_r, (uint64_t)0) << "TX port 1 frames must be > 0";
  EXPECT_EQ(tx_frames_p, tx_frames_r)
      << "TX port frames should be equal (port0=" << tx_frames_p
      << " port1=" << tx_frames_r << ")";
  info("%s, TX frames port0=%" PRIu64 " port1=%" PRIu64 "\n", __func__, tx_frames_p,
       tx_frames_r);

  /* verify RX bytes approximately equal on both ports */
  uint64_t rx_bytes_p = rx_stats.common.port[MTL_SESSION_PORT_P].bytes;
  uint64_t rx_bytes_r = rx_stats.common.port[MTL_SESSION_PORT_R].bytes;
  EXPECT_GT(rx_bytes_p, (uint64_t)0) << "RX port 0 bytes must be > 0";
  EXPECT_GT(rx_bytes_r, (uint64_t)0) << "RX port 1 bytes must be > 0";
  uint64_t rx_max_bytes = std::max(rx_bytes_p, rx_bytes_r);
  uint64_t rx_min_bytes = std::min(rx_bytes_p, rx_bytes_r);
  EXPECT_GT(rx_min_bytes, rx_max_bytes * 80 / 100)
      << "RX port bytes should be approximately equal (port0=" << rx_bytes_p
      << " port1=" << rx_bytes_r << ")";
  info("%s, RX bytes port0=%" PRIu64 " port1=%" PRIu64 "\n", __func__, rx_bytes_p,
       rx_bytes_r);

  /* verify RX received frames */
  EXPECT_GT(test_ctx_rx->fb_rec, 0) << "Must have received frames";
  info("%s, TX sent %d, RX received %d frames\n", __func__, test_ctx_tx->fb_send,
       test_ctx_rx->fb_rec);

  ret = st40p_tx_free(tx_handle);
  EXPECT_GE(ret, 0);
  ret = st40p_rx_free(rx_handle);
  EXPECT_GE(ret, 0);

  delete test_ctx_tx;
  delete test_ctx_rx;
}

#define EPOCH_DROP_TEST_UDP_PORT (40100)

static void st40p_tx_epoch_drop_test(bool user_pacing) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto st = ctx->handle;
  int ret;
  struct st40p_tx_ops ops_tx;

  auto test_ctx = new tests_context();
  ASSERT_TRUE(test_ctx != NULL);
  test_ctx->idx = 0;
  test_ctx->ctx = ctx;
  test_ctx->fb_cnt = 3;

  memset(&ops_tx, 0, sizeof(ops_tx));
  ops_tx.name = "st40p_epoch_drop_test";
  ops_tx.priv = test_ctx;
  ops_tx.port.num_port = 1;
  memcpy(ops_tx.port.dip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
         MTL_IP_ADDR_LEN);
  snprintf(ops_tx.port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
           ctx->para.port[MTL_PORT_P]);
  ops_tx.port.udp_port[MTL_SESSION_PORT_P] = EPOCH_DROP_TEST_UDP_PORT;
  ops_tx.port.payload_type = 113;
  ops_tx.fps = ST_FPS_P59_94;
  ops_tx.interlaced = false;
  ops_tx.framebuff_cnt = test_ctx->fb_cnt;
  ops_tx.max_udw_buff_size = 1024;
  ops_tx.flags |= ST40P_TX_FLAG_BLOCK_GET;
  if (user_pacing) {
    ops_tx.flags |= ST40P_TX_FLAG_USER_PACING;
    ops_tx.flags |= ST40P_TX_FLAG_USER_TIMESTAMP;
  }

  /* create after start, the path that reported bogus drops */
  ret = mtl_start(st);
  EXPECT_GE(ret, 0);
  auto tx_handle = st40p_tx_create(st, &ops_tx);
  ASSERT_TRUE(tx_handle != NULL);
  ret = st40p_tx_set_block_timeout(tx_handle, NS_PER_S);
  EXPECT_EQ(ret, 0);

  test_ctx->handle = tx_handle;
  test_ctx->stop = false;

  auto tx_thread = std::thread(
      [user_pacing](tests_context* s) {
        auto handle = (st40p_tx_handle)s->handle;
        auto st = s->ctx->handle;
        while (!s->stop) {
          auto frame_info = st40p_tx_get_frame(handle);
          if (!frame_info) continue;
          frame_info->meta_num = 0;
          frame_info->udw_buffer_fill = 0;
          if (user_pacing) {
            frame_info->tfmt = ST10_TIMESTAMP_FMT_TAI;
            frame_info->timestamp = mtl_ptp_read_time(st) + 20 * 1000 * 1000; /* +20ms */
          }
          st40p_tx_put_frame(handle, frame_info);
          s->fb_send++;
        }
      },
      test_ctx);
  sleep(5);

  test_ctx->stop = true;
  st40p_tx_wake_block(tx_handle);
  tx_thread.join();

  struct st40_tx_user_stats stats;
  ret = st40p_tx_get_session_stats(tx_handle, &stats);
  EXPECT_GE(ret, 0);
  EXPECT_GT(test_ctx->fb_send, 0);
  info("%s, user_pacing %d, fb_send %d, epoch_drop %" PRIu64 ", epoch_mismatch %" PRIu64
       "\n",
       __func__, user_pacing, test_ctx->fb_send, stats.common.stat_epoch_drop,
       stats.common.stat_epoch_mismatch);
  EXPECT_EQ(stats.common.stat_epoch_drop, 0);

  ret = st40p_tx_free(tx_handle);
  EXPECT_GE(ret, 0);
  delete test_ctx;
}

TEST(St40p, tx_no_epoch_drop) {
  st40p_tx_epoch_drop_test(false);
}

TEST(St40p, tx_user_pacing_no_epoch_drop) {
  st40p_tx_epoch_drop_test(true);
}

#define FMD_TEST_UDP_PORT (40200)
#define FMD_TEST_PAYLOAD_TYPE (115)
#define FMD_TEST_DIT (0x2a5a5a)
#define FMD_TEST_PADDED(size) (((size) + 3) & ~3u)
/* 5 s at 59.94 fps is ~300 items, enough for a 10% rate check */
#define FMD_TEST_DURATION_S (5)

struct st40p_fmd_case {
  uint32_t dit;
  uint8_t k_bit;
  uint32_t item_size; /* at least 4: the first 4 bytes carry the item seq */
  bool redundant;
};

struct st40p_fmd_rx_check {
  st40p_fmd_case c;
  std::vector<uint8_t> want;
  int frames = 0;
  int bad_frames = 0;   /* header, fill, contents or zero padding wrong */
  int out_of_order = 0; /* seq not after the previous one: a duplicate or reorder */
  uint64_t seq_lost = 0;
  bool have_seq = false;
  uint32_t last_seq = 0;
  int discont_frames = 0;
  /* seq_discont set, or not, against whether the item follows the one before it */
  int discont_mismatch = 0;
  int marker_frames = 0;
  bool have_prev = false;
  uint32_t prev_seq = 0;
  uint64_t first_ns = 0;
  uint64_t last_ns = 0;
};

static void st40p_fmd_fill_item(uint8_t* item, uint32_t size, uint32_t seq) {
  for (uint32_t i = 0; i < size; i++)
    item[i] = i < 4 ? (uint8_t)(seq >> (24 - 8 * i)) : (uint8_t)(seq + i);
}

static void st40p_fmd_rx_check_frame(st40p_fmd_rx_check* chk,
                                     const struct st40_frame_info* f) {
  uint32_t padded = FMD_TEST_PADDED(chk->c.item_size);
  bool ok = f->fmd_dit == chk->c.dit && f->fmd_k_bit == chk->c.k_bit &&
            f->meta_num == 0 && f->udw_buffer_fill == padded && f->udw_buff_addr;
  uint32_t seq = 0;

  if (ok) {
    const uint8_t* b = f->udw_buff_addr;
    seq = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
    std::fill(chk->want.begin(), chk->want.end(), 0);
    st40p_fmd_fill_item(chk->want.data(), chk->c.item_size, seq);
    ok = memcmp(b, chk->want.data(), padded) == 0;
  }

  uint64_t now = st_test_get_monotonic_time();
  if (!chk->frames) chk->first_ns = now;
  chk->last_ns = now;
  chk->frames++;
  if (f->rtp_marker) chk->marker_frames++;
  if (f->seq_discont) chk->discont_frames++;
  if (!ok) {
    chk->bad_frames++;
    chk->have_prev = false;
    return;
  }
  bool broken = chk->have_prev && seq != chk->prev_seq + 1;
  if (f->seq_discont != broken) chk->discont_mismatch++;
  chk->have_prev = true;
  chk->prev_seq = seq;
  if (chk->have_seq) {
    int32_t delta = (int32_t)(seq - chk->last_seq);
    if (delta <= 0) {
      chk->out_of_order++;
      return;
    }
    chk->seq_lost += delta - 1;
  }
  chk->have_seq = true;
  chk->last_seq = seq;
}

static void st40p_fmd_tx_ops_init(struct st40p_tx_ops* ops, tests_context* test_ctx,
                                  uint32_t dit, uint8_t k_bit, uint32_t item_size) {
  auto ctx = test_ctx->ctx;

  memset(ops, 0, sizeof(*ops));
  ops->name = "st40p_fmd_tx";
  ops->priv = test_ctx;
  ops->port.num_port = 1;
  if (ctx->mcast_only)
    memcpy(ops->port.dip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
           MTL_IP_ADDR_LEN);
  else
    memcpy(ops->port.dip_addr[MTL_SESSION_PORT_P], ctx->para.sip_addr[MTL_PORT_R],
           MTL_IP_ADDR_LEN);
  (void)snprintf(ops->port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
                 ctx->para.port[MTL_PORT_P]);
  ops->port.udp_port[MTL_SESSION_PORT_P] = FMD_TEST_UDP_PORT;
  ops->port.payload_type = FMD_TEST_PAYLOAD_TYPE;
  ops->fps = ST_FPS_P59_94;
  ops->framebuff_cnt = test_ctx->fb_cnt;
  ops->max_udw_buff_size = item_size;
  ops->flags = ST40P_TX_FLAG_FAST_METADATA | ST40P_TX_FLAG_BLOCK_GET;
  ops->fmd_dit = dit;
  ops->fmd_k_bit = k_bit;
}

/* st40p FMD TX -> st40p FMD RX; every frame is one data item, checked byte by byte */
static void st40p_fmd_loopback_test(const st40p_fmd_case& c) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto st = ctx->handle;
  int ret;
  struct st40p_tx_ops ops_tx;
  struct st40p_rx_ops ops_rx;

  ASSERT_GE(c.item_size, 4u);
  if (ctx->para.num_ports < (c.redundant ? 4 : 2)) {
    info("%s, need %d ports, skipping\n", __func__, c.redundant ? 4 : 2);
    return;
  }

  auto test_ctx_tx = new tests_context();
  ASSERT_TRUE(test_ctx_tx != nullptr);
  test_ctx_tx->ctx = ctx;
  test_ctx_tx->fb_cnt = 3;
  st40p_fmd_tx_ops_init(&ops_tx, test_ctx_tx, c.dit, c.k_bit, c.item_size);
  if (c.redundant) {
    ops_tx.port.num_port = 2;
    memcpy(ops_tx.port.dip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
           MTL_IP_ADDR_LEN);
    memcpy(ops_tx.port.dip_addr[MTL_SESSION_PORT_R], ctx->mcast_ip_addr[MTL_PORT_R],
           MTL_IP_ADDR_LEN);
    (void)snprintf(ops_tx.port.port[MTL_SESSION_PORT_R], MTL_PORT_MAX_LEN, "%s",
                   ctx->para.port[MTL_PORT_R]);
    ops_tx.port.udp_port[MTL_SESSION_PORT_R] = FMD_TEST_UDP_PORT;
  }
  auto tx_handle = st40p_tx_create(st, &ops_tx);
  ASSERT_TRUE(tx_handle != nullptr);

  auto test_ctx_rx = new tests_context();
  ASSERT_TRUE(test_ctx_rx != nullptr);
  test_ctx_rx->ctx = ctx;
  test_ctx_rx->fb_cnt = 3;
  st40p_fmd_rx_check chk;
  chk.c = c;
  chk.want.resize(FMD_TEST_PADDED(c.item_size));
  test_ctx_rx->priv = &chk;

  memset(&ops_rx, 0, sizeof(ops_rx));
  ops_rx.name = "st40p_fmd_rx";
  ops_rx.priv = test_ctx_rx;
  ops_rx.port.num_port = c.redundant ? 2 : 1;
  if (c.redundant) {
    memcpy(ops_rx.port.ip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
           MTL_IP_ADDR_LEN);
    memcpy(ops_rx.port.ip_addr[MTL_SESSION_PORT_R], ctx->mcast_ip_addr[MTL_PORT_R],
           MTL_IP_ADDR_LEN);
    (void)snprintf(ops_rx.port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
                   ctx->para.port[MTL_PORT_2]);
    (void)snprintf(ops_rx.port.port[MTL_SESSION_PORT_R], MTL_PORT_MAX_LEN, "%s",
                   ctx->para.port[MTL_PORT_3]);
    ops_rx.port.udp_port[MTL_SESSION_PORT_R] = FMD_TEST_UDP_PORT;
  } else {
    if (ctx->mcast_only)
      memcpy(ops_rx.port.ip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
             MTL_IP_ADDR_LEN);
    else
      memcpy(ops_rx.port.ip_addr[MTL_SESSION_PORT_P], ctx->para.sip_addr[MTL_PORT_P],
             MTL_IP_ADDR_LEN);
    (void)snprintf(ops_rx.port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
                   ctx->para.port[MTL_PORT_R]);
  }
  ops_rx.port.udp_port[MTL_SESSION_PORT_P] = FMD_TEST_UDP_PORT;
  ops_rx.port.payload_type = FMD_TEST_PAYLOAD_TYPE;
  ops_rx.framebuff_cnt = test_ctx_rx->fb_cnt;
  /* exactly one padded item: an item that fills framebuff_size must still arrive */
  ops_rx.max_udw_buff_size = FMD_TEST_PADDED(c.item_size);
  ops_rx.rtp_ring_size = 1024;
  ops_rx.flags = ST40P_RX_FLAG_FAST_METADATA | ST40P_RX_FLAG_BLOCK_GET;
  auto rx_handle = st40p_rx_create(st, &ops_rx);
  ASSERT_TRUE(rx_handle != nullptr);

  test_ctx_tx->handle = tx_handle;
  auto tx_thread = std::thread(
      [](tests_context* s, uint32_t item_size) {
        auto handle = (st40p_tx_handle)s->handle;
        uint32_t seq = 0;
        while (!s->stop) {
          auto fi = st40p_tx_get_frame(handle);
          if (!fi) continue;
          st40p_fmd_fill_item(fi->udw_buff_addr, item_size, seq++);
          fi->udw_buffer_fill = item_size;
          st40p_tx_put_frame(handle, fi);
          s->fb_send++;
        }
      },
      test_ctx_tx, c.item_size);

  test_ctx_rx->handle = rx_handle;
  auto rx_thread = std::thread(
      [](tests_context* s) {
        auto handle = (st40p_rx_handle)s->handle;
        auto chk = (st40p_fmd_rx_check*)s->priv;
        while (!s->stop) {
          auto fi = st40p_rx_get_frame(handle);
          if (!fi) continue;
          st40p_fmd_rx_check_frame(chk, fi);
          st40p_rx_put_frame(handle, fi);
        }
      },
      test_ctx_rx);

  ret = mtl_start(st);
  EXPECT_GE(ret, 0);
  sleep(FMD_TEST_DURATION_S);

  test_ctx_tx->stop = true;
  st40p_tx_wake_block(tx_handle);
  tx_thread.join();
  test_ctx_rx->stop = true;
  st40p_rx_wake_block(rx_handle);
  rx_thread.join();

  double expect_rate = st_frame_rate(ST_FPS_P59_94);
  double rate = 0;
  if (chk.frames > 1 && chk.last_ns > chk.first_ns)
    rate = (double)(chk.frames - 1) * NS_PER_S / (double)(chk.last_ns - chk.first_ns);
  info(
      "%s, dit 0x%x k %u size %u redundant %d: sent %d, received %d, discont %d, rate "
      "%f\n",
      __func__, c.dit, c.k_bit, c.item_size, c.redundant, test_ctx_tx->fb_send,
      chk.frames, chk.discont_frames, rate);
  EXPECT_GT(chk.frames, 0);
  EXPECT_NEAR(rate, expect_rate, expect_rate * 0.1);
  EXPECT_EQ(chk.bad_frames, 0);
  EXPECT_EQ(chk.out_of_order, 0);
  EXPECT_LE(chk.seq_lost, 2u);
  EXPECT_EQ(chk.discont_mismatch, 0);
  EXPECT_LE(chk.discont_frames, 2);
  EXPECT_EQ(chk.marker_frames, 0);

  ret = st40p_rx_free(rx_handle);
  EXPECT_GE(ret, 0);
  ret = st40p_tx_free(tx_handle);
  EXPECT_GE(ret, 0);
  delete test_ctx_rx;
  delete test_ctx_tx;
}

static bool st40p_fmd_tx_create_ok(uint32_t extra_flags, uint32_t dit) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  tests_context test_ctx;
  struct st40p_tx_ops ops;

  test_ctx.ctx = ctx;
  test_ctx.fb_cnt = 3;
  st40p_fmd_tx_ops_init(&ops, &test_ctx, dit, 0, 64);
  ops.flags |= extra_flags;
  auto handle = st40p_tx_create(ctx->handle, &ops);
  if (!handle) return false;
  EXPECT_GE(st40p_tx_free(handle), 0);
  return true;
}

TEST(St40p, fmd_loopback) {
  st40p_fmd_loopback_test({FMD_TEST_DIT, 1, 61, false});
}

/* 0x3fffff is the top experimental DIT (§8.6); 1436 bytes fill the packet */
TEST(St40p, fmd_loopback_max_dit_max_item) {
  st40p_fmd_loopback_test({0x3fffff, 0, 1436, false});
}

TEST(St40p, fmd_redundant_loopback) {
  st40p_fmd_loopback_test({0x000000, 1, 13, true});
}

TEST(St40p, fmd_create_expect_fail_split_anc) {
  EXPECT_TRUE(st40p_fmd_tx_create_ok(0, FMD_TEST_DIT));
  EXPECT_FALSE(st40p_fmd_tx_create_ok(ST40P_TX_FLAG_SPLIT_ANC_BY_PKT, FMD_TEST_DIT));
}

TEST(St40p, fmd_create_expect_fail_reserved_dit) {
  EXPECT_TRUE(st40p_fmd_tx_create_ok(0, 0x2fffff));
  EXPECT_FALSE(st40p_fmd_tx_create_ok(0, 0x300000));
  EXPECT_FALSE(st40p_fmd_tx_create_ok(0, 0x3fefff));
  EXPECT_TRUE(st40p_fmd_tx_create_ok(0, 0x3ff000));
}
