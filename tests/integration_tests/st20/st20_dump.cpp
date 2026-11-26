/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include <sys/stat.h>

#include "../harness/pcapng.hpp"
#include "st20_common.h"

namespace {

void st20_rx_dump_test(const std::vector<St20StreamSpec>& streams) {
  auto ctx = st_test_ctx();
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

  st20_setup_basic_loopback(ctx, streams, "st20_dump_test", test_ctx_tx, test_ctx_rx,
                            tx_handle, rx_handle, rtp_thread_tx, rtp_thread_rx);
  if (::testing::Test::HasFatalFailure()) return;

  int ret = mtl_start(m_handle);
  EXPECT_GE(ret, 0);
  guard.set_started(ret >= 0);

  EXPECT_TRUE(mtl_test::wait_for_all(test_ctx_rx, mtl_test::rx_frames, kSt20WarmupFrames,
                                     kSt20WarmupTimeoutMs));

  constexpr uint32_t kMaxDumpPackets = 100;
  for (int i = 0; i < sessions; i++) {
    struct st_pcap_dump_meta meta = {};
    ret = st20_rx_pcapng_dump(rx_handle[i], kMaxDumpPackets, true, &meta);
    St20UnlinkGuard unlink_guard{meta.file_name[MTL_SESSION_PORT_P]};
    EXPECT_GE(ret, 0);
    EXPECT_EQ(meta.dumped_packets[MTL_SESSION_PORT_P], kMaxDumpPackets);
    dbg("%s, file_name %s\n", __func__, meta.file_name[MTL_SESSION_PORT_P]);
    if (ret >= 0) {
      struct stat st_buf = {};
      EXPECT_EQ(stat(meta.file_name[MTL_SESSION_PORT_P], &st_buf), 0);
      EXPECT_GT(st_buf.st_size, 0);
      mtl_test::verify_pcapng_file(meta.file_name[MTL_SESSION_PORT_P], kMaxDumpPackets);
    }
  }

  st20_stop_tx_ready(test_ctx_tx);
  guard.release_sessions();
}

}  // namespace

TEST(St20_rx, pcap_dump) {
  st20_rx_dump_test({
      {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_BPM, ST_FPS_P59_94,
       1280, 720},
      {ST20_TYPE_RTP_LEVEL, ST20_TYPE_RTP_LEVEL, ST20_PACKING_BPM, ST_FPS_P50, 1920,
       1080},
  });
}
