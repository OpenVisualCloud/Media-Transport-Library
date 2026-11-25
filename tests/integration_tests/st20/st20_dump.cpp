/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include "st20_common.h"

namespace {

struct St20DumpStream {
  enum st20_type type;
  enum st_fps fps;
  int width;
  int height;
};

void st20_rx_dump_test(const std::vector<St20DumpStream>& streams, enum st20_fmt fmt) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto m_handle = ctx->handle;
  const int sessions = static_cast<int>(streams.size());

  MTL_TEST_REQUIRE_PORTS(ctx, 2);
  if (!mtl_pmd_is_dpdk_based(m_handle, MTL_PORT_R)) {
    GTEST_SKIP() << "MTL_PORT_R is not a DPDK-based PMD";
  }

  std::vector<tests_context*> test_ctx_tx(sessions);
  std::vector<tests_context*> test_ctx_rx(sessions);
  std::vector<st20_tx_handle> tx_handle(sessions);
  std::vector<st20_rx_handle> rx_handle(sessions);
  std::vector<std::thread> rtp_thread_tx(sessions);
  std::vector<std::thread> rtp_thread_rx(sessions);

  St20DeinitGuard guard(m_handle, test_ctx_tx, test_ctx_rx, tx_handle, rx_handle,
                        &rtp_thread_tx, &rtp_thread_rx);

  for (int i = 0; i < sessions; i++) {
    const auto& st = streams[i];
    test_ctx_tx[i] = init_test_ctx(ctx, i, 3);
    ASSERT_TRUE(test_ctx_tx[i] != NULL);
    test_ctx_tx[i]->stop = false;

    struct st20_tx_ops ops_tx;
    init_single_port_tx(ops_tx, test_ctx_tx[i], "st20_dump_test", udp_port_for_idx(i));
    ops_tx.type = st.type;
    ops_tx.width = st.width;
    ops_tx.height = st.height;
    ops_tx.fps = st.fps;
    ops_tx.fmt = fmt;
    ops_tx.payload_type = ST20_TEST_PAYLOAD_TYPE;
    ops_tx.framebuff_cnt = test_ctx_tx[i]->fb_cnt;
    ops_tx.get_next_frame = tx_next_video_frame;
    if (st.type == ST20_TYPE_RTP_LEVEL) {
      rtp_tx_specific_init(&ops_tx, test_ctx_tx[i]);
    }
    tx_handle[i] = st20_tx_create(m_handle, &ops_tx);
    ASSERT_TRUE(tx_handle[i] != NULL);
    st20_set_handle(test_ctx_tx[i], tx_handle[i]);
    if (st.type == ST20_TYPE_RTP_LEVEL) {
      rtp_thread_tx[i] = std::thread(tx_feed_packet, test_ctx_tx[i]);
    }
  }

  for (int i = 0; i < sessions; i++) {
    const auto& st = streams[i];
    test_ctx_rx[i] = init_test_ctx(ctx, i, 3);
    ASSERT_TRUE(test_ctx_rx[i] != NULL);
    test_ctx_rx[i]->stop = false;

    struct st20_rx_ops ops_rx;
    init_single_port_rx(ops_rx, test_ctx_rx[i], "st20_dump_test", udp_port_for_idx(i));
    ops_rx.type = st.type;
    ops_rx.width = st.width;
    ops_rx.height = st.height;
    ops_rx.fps = st.fps;
    ops_rx.fmt = fmt;
    ops_rx.payload_type = ST20_TEST_PAYLOAD_TYPE;
    ops_rx.framebuff_cnt = test_ctx_rx[i]->fb_cnt;
    ops_rx.notify_frame_ready = st20_rx_frame_ready;
    ops_rx.notify_rtp_ready = rx_rtp_ready;
    rx_handle[i] = st20_rx_create(m_handle, &ops_rx);
    ASSERT_TRUE(rx_handle[i] != NULL);

    test_ctx_rx[i]->total_pkts_in_frame = test_ctx_tx[i]->total_pkts_in_frame;
    st20_set_handle(test_ctx_rx[i], rx_handle[i]);
    if (st.type == ST20_TYPE_RTP_LEVEL) {
      rtp_thread_rx[i] = std::thread(rx_get_packet, test_ctx_rx[i]);
    }
  }

  int ret = mtl_start(m_handle);
  EXPECT_GE(ret, 0);
  guard.set_started(ret >= 0);

  /* Wait until all RX sessions receive warmup frames before synchronous pcap capture. */
  EXPECT_TRUE(mtl_test::wait_for_all(test_ctx_rx, mtl_test::rx_frames, kSt20WarmupFrames,
                                     kSt20WarmupTimeoutMs));

  uint32_t max_dump_packets = 100;
  for (int i = 0; i < sessions; i++) {
    struct st_pcap_dump_meta meta = {};
    ret = st20_rx_pcapng_dump(rx_handle[i], max_dump_packets, true, &meta);
    EXPECT_GE(ret, 0);
    EXPECT_EQ(meta.dumped_packets[MTL_SESSION_PORT_P], max_dump_packets);
    dbg("%s, file_name %s\n", __func__, meta.file_name[MTL_SESSION_PORT_P]);
    if (ret >= 0) remove(meta.file_name[MTL_SESSION_PORT_P]);
  }

  guard.release_sessions();
}

}  // namespace

TEST(St20_rx, pcap_dump) {
  st20_rx_dump_test(
      {
          {ST20_TYPE_FRAME_LEVEL, ST_FPS_P59_94, 1280, 720},
          {ST20_TYPE_RTP_LEVEL, ST_FPS_P50, 1920, 1080},
      },
      ST20_FMT_YUV_422_10BIT);
}
