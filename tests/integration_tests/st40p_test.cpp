/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include <thread>

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

  auto tx_handle = st40p_tx_create(st, &ops_tx);
  ASSERT_TRUE(tx_handle != NULL);
  ret = st40p_tx_set_block_timeout(tx_handle, NS_PER_S);
  EXPECT_EQ(ret, 0);

  test_ctx->handle = tx_handle;
  test_ctx->stop = false;

  ret = mtl_start(st);
  EXPECT_GE(ret, 0);

  auto tx_thread = std::thread(
      [user_pacing](tests_context* s) {
        auto handle = (st40p_tx_handle)s->handle;
        auto st = s->ctx->handle;
        while (!s->stop) {
          auto frame_info = st40p_tx_get_frame(handle);
          if (!frame_info) continue;
          /* fill minimal ANC metadata */
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

  /* check stats before free */
  struct st40_tx_user_stats stats;
  ret = st40p_tx_get_session_stats(tx_handle, &stats);
  EXPECT_GE(ret, 0);
  EXPECT_GT(test_ctx->fb_send, 0);
  info("%s, user_pacing %d, fb_send %d, epoch_drop %" PRIu64 ", epoch_mismatch %" PRIu64
       "\n",
       __func__, user_pacing, test_ctx->fb_send, stats.common.stat_epoch_drop,
       stats.stat_epoch_mismatch);
  EXPECT_EQ(stats.common.stat_epoch_drop, 0);

  ret = mtl_stop(st);
  EXPECT_GE(ret, 0);
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
