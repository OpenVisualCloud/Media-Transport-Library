/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include "st20_common.h"

namespace {

void verify_switch_phase(const std::vector<tests_context*>& test_ctx_rx,
                         const std::vector<st20_rx_handle>& rx_handle,
                         const std::vector<double>& expect_framerate,
                         const struct st_rx_source_info& src, enum st20_type type,
                         int max_rtp_delta, const char* label) {
  const int rx_sessions = static_cast<int>(rx_handle.size());
  std::vector<int> base_rec(rx_sessions, 0);
  for (int i = 0; i < rx_sessions; i++) {
    int ret = st20_rx_update_source(rx_handle[i], const_cast<st_rx_source_info*>(&src));
    EXPECT_GE(ret, 0);
    __atomic_store_n(&test_ctx_rx[i]->start_time, 0, __ATOMIC_RELEASE);
    base_rec[i] = mtl_test::rx_frames(test_ctx_rx[i]);
  }

  /*
   * Measure steady-state RX FPS relative to base_rec[i] without resetting fb_rec:
   * warmup=12 (~200 ms at 59.94 fps) absorbs the switch transition, then >=35 frames
   * over >=1500 ms (~90 frames at 59.94 fps, <=1.1% edge error vs +/-10% tolerance).
   */
  auto rel_rx_frames = [&base_rec](const tests_context* c) {
    return mtl_test::rx_frames(c) - base_rec[c->idx];
  };
  auto res = mtl_test::measure_rate_window(test_ctx_rx, rel_rx_frames, kSt20WarmupFrames,
                                           kSt20MinRateFrames, kSt20MinRateWindowMs,
                                           kSt20WarmupTimeoutMs, kSt20WindowTimeoutMs);
  EXPECT_TRUE(res.warmed_up);
  EXPECT_TRUE(res.complete);
  for (int i = 0; i < rx_sessions; i++) {
    int rec = mtl_test::rx_frames(test_ctx_rx[i]);
    EXPECT_GT(rec, base_rec[i]);
    info("%s, session %d fb_rec %d framerate %f for %s\n", __func__, i, rec - base_rec[i],
         res.rate[i], label);
    EXPECT_NEAR(res.rate[i], expect_framerate[i], expect_framerate[i] * 0.1);
    if (type == ST20_TYPE_FRAME_LEVEL) {
      /* Acquire load on start_time pairs with release store after rtp_delta update. */
      EXPECT_NE(__atomic_load_n(&test_ctx_rx[i]->start_time, __ATOMIC_ACQUIRE), 0u);
      EXPECT_LE(__atomic_load_n(&test_ctx_rx[i]->rtp_delta, __ATOMIC_RELAXED),
                max_rtp_delta);
    }
  }
}

void st20_rx_update_src_test(enum st20_type type, int tx_sessions) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto m_handle = ctx->handle;

  MTL_TEST_REQUIRE_PORTS(ctx, 2);
  ASSERT_TRUE(tx_sessions >= 1);

  const bool tx_update_dst = (tx_sessions == 1);
  const int rx_sessions = 1;
  /* 1501/1502 for one frame, max two frames. */
  constexpr int kMaxRtpDelta = 3003;

  std::vector<tests_context*> test_ctx_tx(tx_sessions);
  std::vector<tests_context*> test_ctx_rx(rx_sessions);
  std::vector<st20_tx_handle> tx_handle(tx_sessions);
  std::vector<st20_rx_handle> rx_handle(rx_sessions);
  std::vector<double> expect_framerate(rx_sessions, st_frame_rate(ST_FPS_P59_94));
  std::vector<std::thread> rtp_thread_tx(tx_sessions);
  std::vector<std::thread> rtp_thread_rx(rx_sessions);

  St20DeinitGuard guard(m_handle, test_ctx_tx, test_ctx_rx, tx_handle, rx_handle,
                        &rtp_thread_tx, &rtp_thread_rx);

  for (int i = 0; i < tx_sessions; i++) {
    test_ctx_tx[i] = init_test_ctx(ctx, i, 3);
    ASSERT_TRUE(test_ctx_tx[i] != NULL);
    test_ctx_tx[i]->stop = false;

    struct st20_tx_ops ops_tx;
    init_single_port_tx(ops_tx, test_ctx_tx[i], "st20_test", udp_port_for_idx(i));
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
    ops_tx.type = type;
    ops_tx.width = 1920;
    ops_tx.height = 1080;
    ops_tx.fps = ST_FPS_P59_94;
    ops_tx.fmt = ST20_FMT_YUV_422_10BIT;
    ops_tx.payload_type = ST20_TEST_PAYLOAD_TYPE;
    ops_tx.framebuff_cnt = test_ctx_tx[i]->fb_cnt;
    ops_tx.get_next_frame = tx_next_video_frame;
    if (type == ST20_TYPE_RTP_LEVEL) {
      rtp_tx_specific_init(&ops_tx, test_ctx_tx[i]);
    }

    tx_handle[i] = st20_tx_create(m_handle, &ops_tx);
    ASSERT_TRUE(tx_handle[i] != NULL);
    st20_set_handle(test_ctx_tx[i], tx_handle[i]);
    if (type == ST20_TYPE_RTP_LEVEL) {
      rtp_thread_tx[i] = std::thread(tx_feed_packet, test_ctx_tx[i]);
    }
  }

  for (int i = 0; i < rx_sessions; i++) {
    test_ctx_rx[i] = init_test_ctx(ctx, i, 3);
    ASSERT_TRUE(test_ctx_rx[i] != NULL);
    test_ctx_rx[i]->stop = false;

    struct st20_rx_ops ops_rx;
    init_single_port_rx(ops_rx, test_ctx_rx[i], "st20_test", udp_port_for_idx(i));
    if (ctx->mcast_only)
      memcpy(ops_rx.ip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_2],
             MTL_IP_ADDR_LEN);
    else
      memcpy(ops_rx.ip_addr[MTL_SESSION_PORT_P], ctx->para.sip_addr[MTL_PORT_P],
             MTL_IP_ADDR_LEN);
    ops_rx.type = type;
    ops_rx.width = 1920;
    ops_rx.height = 1080;
    ops_rx.fps = ST_FPS_P59_94;
    ops_rx.fmt = ST20_FMT_YUV_422_10BIT;
    ops_rx.payload_type = ST20_TEST_PAYLOAD_TYPE;
    ops_rx.framebuff_cnt = test_ctx_rx[i]->fb_cnt;
    ops_rx.notify_frame_ready = st20_rx_frame_ready;
    ops_rx.notify_rtp_ready = rx_rtp_ready;
    ops_rx.flags |= ST20_RX_FLAG_RECEIVE_INCOMPLETE_FRAME;
    rx_handle[i] = st20_rx_create(m_handle, &ops_rx);
    ASSERT_TRUE(rx_handle[i] != NULL);

    test_ctx_rx[i]->total_pkts_in_frame = test_ctx_tx[i]->total_pkts_in_frame;
    st20_set_handle(test_ctx_rx[i], rx_handle[i]);
    if (type == ST20_TYPE_RTP_LEVEL) {
      rtp_thread_rx[i] = std::thread(rx_get_packet, test_ctx_rx[i]);
    }
  }

  int ret = mtl_start(m_handle);
  EXPECT_GE(ret, 0);
  guard.set_started(ret >= 0);

  /* Wait until all TX sessions finish pacing startup and RX receives initial frames. */
  EXPECT_TRUE(
      mtl_test::wait_for_all(test_ctx_tx, mtl_test::tx_frames, 5, kSt20WarmupTimeoutMs));
  EXPECT_TRUE(
      mtl_test::wait_for_all(test_ctx_rx, mtl_test::rx_frames, 5, kSt20WarmupTimeoutMs));

  struct st_rx_source_info src;
  /* switch to mcast port p(tx_session:1) */
  memset(&src, 0, sizeof(src));
  src.udp_port[MTL_SESSION_PORT_P] = udp_port_for_idx(1);
  memcpy(src.ip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
         MTL_IP_ADDR_LEN);
  if (tx_update_dst) {
    __atomic_store_n(&test_ctx_tx[0]->seq_id, 0, __ATOMIC_RELAXED); /* reset seq id */
    struct st_tx_dest_info dst;
    memset(&dst, 0, sizeof(dst));
    dst.udp_port[MTL_SESSION_PORT_P] = udp_port_for_idx(1);
    memcpy(dst.dip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
           MTL_IP_ADDR_LEN);
    EXPECT_GE(st20_tx_update_destination(tx_handle[0], &dst), 0);
  } else {
    __atomic_store_n(&test_ctx_tx[1]->seq_id, 0, __ATOMIC_RELAXED); /* reset seq id */
  }
  verify_switch_phase(test_ctx_rx, rx_handle, expect_framerate, src, type, kMaxRtpDelta,
                      "mcast 1");

  if (tx_sessions > 2) {
    /* switch to mcast port r(tx_session:2) */
    memset(&src, 0, sizeof(src));
    src.udp_port[MTL_SESSION_PORT_P] = udp_port_for_idx(2);
    memcpy(src.ip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_R],
           MTL_IP_ADDR_LEN);
    __atomic_store_n(&test_ctx_tx[2]->seq_id, rand(),
                     __ATOMIC_RELAXED); /* random seq id */
    verify_switch_phase(test_ctx_rx, rx_handle, expect_framerate, src, type, kMaxRtpDelta,
                        "mcast 2");
  }

  /* switch to unicast(tx_session:0) */
  memset(&src, 0, sizeof(src));
  src.udp_port[MTL_SESSION_PORT_P] = udp_port_for_idx(0);
  memcpy(
      src.ip_addr[MTL_SESSION_PORT_P],
      ctx->mcast_only ? ctx->mcast_ip_addr[MTL_PORT_2] : ctx->para.sip_addr[MTL_PORT_P],
      MTL_IP_ADDR_LEN);
  __atomic_store_n(&test_ctx_tx[0]->seq_id, rand(), __ATOMIC_RELAXED); /* random seq id */
  if (tx_update_dst) {
    struct st_tx_dest_info dst;
    memset(&dst, 0, sizeof(dst));
    dst.udp_port[MTL_SESSION_PORT_P] = udp_port_for_idx(0);
    memcpy(
        dst.dip_addr[MTL_SESSION_PORT_P],
        ctx->mcast_only ? ctx->mcast_ip_addr[MTL_PORT_2] : ctx->para.sip_addr[MTL_PORT_R],
        MTL_IP_ADDR_LEN);
    EXPECT_GE(st20_tx_update_destination(tx_handle[0], &dst), 0);
  }
  verify_switch_phase(test_ctx_rx, rx_handle, expect_framerate, src, type, kMaxRtpDelta,
                      "unicast 0");

  guard.release_sessions();
}

}  // namespace

TEST(St20_rx, update_source_frame) {
  st20_rx_update_src_test(ST20_TYPE_FRAME_LEVEL, 3);
}
TEST(St20_rx, update_source_rtp) {
  st20_rx_update_src_test(ST20_TYPE_RTP_LEVEL, 2);
}
TEST(St20_tx, update_dest_frame) {
  st20_rx_update_src_test(ST20_TYPE_FRAME_LEVEL, 1);
}
TEST(St20_tx, update_dest_rtp) {
  st20_rx_update_src_test(ST20_TYPE_RTP_LEVEL, 1);
}
