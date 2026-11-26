/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include <atomic>
#include <deque>

#include "st20_common.h"

namespace {

constexpr int kMaxUpdateSrcs = 3;
constexpr uint8_t kFrameSrcMarkerBase = 0xA0;
constexpr size_t kFrameSrcMarkerLen = 8;

struct UpdateSourceRxState {
  tests_context* ctx = nullptr;
  std::atomic<int> src_hits[kMaxUpdateSrcs];
  std::atomic<int> src_unknown{0};

  UpdateSourceRxState() {
    reset_counts();
  }

  void reset_counts() {
    for (int i = 0; i < kMaxUpdateSrcs; i++) {
      src_hits[i].store(0, std::memory_order_relaxed);
    }
    src_unknown.store(0, std::memory_order_relaxed);
  }
};

int st20_update_src_rx_frame_ready(void* priv, void* frame,
                                   struct st20_rx_frame_meta* meta) {
  auto* state = static_cast<UpdateSourceRxState*>(priv);
  if (!state || !state->ctx) return -EIO;
  if (frame && st_is_frame_complete(meta->status)) {
    const auto* bytes = static_cast<const uint8_t*>(frame);
    int matched_src = -1;
    for (int s = 0; s < kMaxUpdateSrcs; s++) {
      const uint8_t expected = static_cast<uint8_t>(kFrameSrcMarkerBase + s);
      bool ok = true;
      for (size_t b = 0; b < kFrameSrcMarkerLen; b++) {
        if (bytes[b] != expected) {
          ok = false;
          break;
        }
      }
      if (ok) {
        matched_src = s;
        break;
      }
    }
    if (matched_src >= 0) {
      state->src_hits[matched_src].fetch_add(1, std::memory_order_relaxed);
    } else {
      state->src_unknown.fetch_add(1, std::memory_order_relaxed);
    }
  }
  return st20_rx_frame_ready(state->ctx, frame, meta);
}

void verify_switch_phase(const std::vector<tests_context*>& test_ctx_rx,
                         std::deque<UpdateSourceRxState>& rx_states,
                         const std::vector<st20_rx_handle>& rx_handle,
                         const std::vector<double>& expect_framerate,
                         const struct st_rx_source_info& src, enum st20_type type,
                         int expected_src, int max_rtp_delta, const char* label) {
  const int rx_sessions = static_cast<int>(rx_handle.size());
  std::vector<int> base_rec(rx_sessions, 0);
  for (int i = 0; i < rx_sessions; i++) {
    int ret = st20_rx_update_source(rx_handle[i], const_cast<st_rx_source_info*>(&src));
    EXPECT_GE(ret, 0);
    __atomic_store_n(&test_ctx_rx[i]->start_time, 0, __ATOMIC_RELEASE);
  }

  /* Allow in-flight packets/frames from the previous source to drain before counting. */
  st_usleep(150 * 1000);
  for (int i = 0; i < rx_sessions; i++) {
    rx_states[i].reset_counts();
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
  const int min_hits = (type == ST20_TYPE_FRAME_LEVEL) ? 5 : 20;
  for (int i = 0; i < rx_sessions; i++) {
    int rec = mtl_test::rx_frames(test_ctx_rx[i]);
    EXPECT_GT(rec, base_rec[i]);
    info("%s, session %d fb_rec %d framerate %f for %s\n", __func__, i, rec - base_rec[i],
         res.rate[i], label);
    EXPECT_NEAR(res.rate[i], expect_framerate[i], expect_framerate[i] * 0.1);
    EXPECT_GT(rx_states[i].src_hits[expected_src].load(std::memory_order_relaxed),
              min_hits)
        << "expected hits from source " << expected_src << " (" << label << ")";
    for (int other = 0; other < kMaxUpdateSrcs; other++) {
      if (other == expected_src) continue;
      EXPECT_EQ(rx_states[i].src_hits[other].load(std::memory_order_relaxed), 0)
          << "unexpected hits from source " << other << " while expecting "
          << expected_src << " (" << label << ")";
    }
    EXPECT_EQ(rx_states[i].src_unknown.load(std::memory_order_relaxed), 0)
        << "unexpected untagged frames/packets (" << label << ")";
    if (type == ST20_TYPE_FRAME_LEVEL) {
      /* Acquire load on start_time pairs with release store after rtp_delta update. */
      EXPECT_NE(__atomic_load_n(&test_ctx_rx[i]->start_time, __ATOMIC_ACQUIRE), 0u);
      const int rtp_delta = __atomic_load_n(&test_ctx_rx[i]->rtp_delta, __ATOMIC_RELAXED);
      EXPECT_GE(rtp_delta, 0);
      EXPECT_LE(rtp_delta, max_rtp_delta);
    }
  }
}

void st20_rx_update_src_test(enum st20_type type, int tx_sessions) {
  auto ctx = st_test_ctx();
  auto m_handle = ctx->handle;

  MTL_TEST_REQUIRE_PORTS(ctx, 2);
  ASSERT_GE(tx_sessions, 1);
  ASSERT_LE(tx_sessions, kMaxUpdateSrcs);

  const bool tx_update_dst = (tx_sessions == 1);
  const int rx_sessions = 1;
  /* 1501/1502 for one frame, max two frames. */
  constexpr int kMaxRtpDelta = 3003;
  const St20StreamSpec st{type, type, ST20_PACKING_BPM, ST_FPS_P59_94, 1920, 1080};

  /* Declare rx_states before St20DeinitGuard so handles/threads stop before destroy. */
  std::deque<UpdateSourceRxState> rx_states(rx_sessions);
  std::vector<tests_context*> test_ctx_tx(tx_sessions);
  std::vector<tests_context*> test_ctx_rx(rx_sessions);
  std::vector<st20_tx_handle> tx_handle(tx_sessions);
  std::vector<st20_rx_handle> rx_handle(rx_sessions);
  std::vector<double> expect_framerate(rx_sessions, st_frame_rate(st.fps));
  std::vector<std::thread> rtp_thread_tx(tx_sessions);
  std::vector<std::thread> rtp_thread_rx(rx_sessions);

  St20DeinitGuard guard(m_handle, test_ctx_tx, test_ctx_rx, tx_handle, rx_handle,
                        &rtp_thread_tx, &rtp_thread_rx);

  for (int i = 0; i < tx_sessions; i++) {
    test_ctx_tx[i] = init_test_ctx(ctx, i, 3);
    struct st20_tx_ops ops_tx;
    st20_fill_loopback_tx_ops(ops_tx, test_ctx_tx[i], st, "st20_test",
                              udp_port_for_idx(i));
    if (2 == i)
      memcpy(ops_tx.dip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_R],
             MTL_IP_ADDR_LEN);
    else if (1 == i)
      memcpy(ops_tx.dip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
             MTL_IP_ADDR_LEN);
    else
      memcpy(ops_tx.dip_addr[MTL_SESSION_PORT_P], ctx->para.sip_addr[MTL_PORT_R],
             MTL_IP_ADDR_LEN);

    tx_handle[i] = st20_tx_create(m_handle, &ops_tx);
    ASSERT_TRUE(tx_handle[i] != NULL);
    if (type == ST20_TYPE_FRAME_LEVEL) {
      const uint8_t marker = static_cast<uint8_t>(kFrameSrcMarkerBase + i);
      for (int f = 0; f < test_ctx_tx[i]->fb_cnt; f++) {
        auto* fb = static_cast<uint8_t*>(
            st20_tx_get_framebuffer(tx_handle[i], static_cast<uint16_t>(f)));
        ASSERT_NE(fb, nullptr);
        memset(fb, marker, kFrameSrcMarkerLen);
      }
    }
    st20_set_handle(test_ctx_tx[i], tx_handle[i]);
    if (type == ST20_TYPE_RTP_LEVEL) {
      rtp_thread_tx[i] = std::thread(tx_feed_packet, test_ctx_tx[i]);
    }
  }

  for (int i = 0; i < rx_sessions; i++) {
    test_ctx_rx[i] = init_test_ctx(ctx, i, 3);
    rx_states[i].ctx = test_ctx_rx[i];
    struct st20_rx_ops ops_rx;
    st20_fill_loopback_rx_ops(ops_rx, test_ctx_rx[i], st, "st20_test",
                              udp_port_for_idx(i));
    ops_rx.flags |= ST20_RX_FLAG_RECEIVE_INCOMPLETE_FRAME;
    if (type == ST20_TYPE_FRAME_LEVEL) {
      ops_rx.priv = &rx_states[i];
      ops_rx.notify_frame_ready = st20_update_src_rx_frame_ready;
    }
    rx_handle[i] = st20_rx_create(m_handle, &ops_rx);
    ASSERT_TRUE(rx_handle[i] != NULL);

    test_ctx_rx[i]->total_pkts_in_frame = test_ctx_tx[i]->total_pkts_in_frame;
    st20_set_handle(test_ctx_rx[i], rx_handle[i]);
    if (type == ST20_TYPE_RTP_LEVEL) {
      UpdateSourceRxState* state = &rx_states[i];
      rtp_thread_rx[i] = std::thread([rctx = test_ctx_rx[i], state]() {
        rx_get_packet_ex(rctx, [state](const struct st20_rfc4175_rtp_hdr* hdr) {
          const uint32_t ssrc = ntohl(hdr->base.ssrc);
          if (ssrc >= kSt20RtpSsrcBase &&
              ssrc < kSt20RtpSsrcBase + static_cast<uint32_t>(kMaxUpdateSrcs)) {
            state->src_hits[ssrc - kSt20RtpSsrcBase].fetch_add(1,
                                                               std::memory_order_relaxed);
          } else {
            state->src_unknown.fetch_add(1, std::memory_order_relaxed);
          }
        });
      });
    }
  }

  int ret = mtl_start(m_handle);
  EXPECT_GE(ret, 0);
  guard.set_started(ret >= 0);

  EXPECT_TRUE(
      mtl_test::wait_for_all(test_ctx_tx, mtl_test::tx_frames, 5, kSt20WarmupTimeoutMs));
  EXPECT_TRUE(
      mtl_test::wait_for_all(test_ctx_rx, mtl_test::rx_frames, 5, kSt20WarmupTimeoutMs));
  EXPECT_GT(rx_states[0].src_hits[0].load(std::memory_order_relaxed), 0);

  auto switch_and_verify = [&](int idx, const uint8_t* tx_ip, const uint8_t* rx_ip,
                               int new_seq, const char* label) {
    struct st_rx_source_info src = {};
    src.udp_port[MTL_SESSION_PORT_P] = udp_port_for_idx(idx);
    memcpy(src.ip_addr[MTL_SESSION_PORT_P], rx_ip, MTL_IP_ADDR_LEN);
    if (tx_update_dst) {
      __atomic_store_n(&test_ctx_tx[0]->seq_id, new_seq, __ATOMIC_RELAXED);
      struct st_tx_dest_info dst = {};
      dst.udp_port[MTL_SESSION_PORT_P] = udp_port_for_idx(idx);
      memcpy(dst.dip_addr[MTL_SESSION_PORT_P], tx_ip, MTL_IP_ADDR_LEN);
      EXPECT_GE(st20_tx_update_destination(tx_handle[0], &dst), 0);
    } else {
      __atomic_store_n(&test_ctx_tx[idx]->seq_id, new_seq, __ATOMIC_RELAXED);
    }
    const int expected_src = tx_update_dst ? 0 : idx;
    verify_switch_phase(test_ctx_rx, rx_states, rx_handle, expect_framerate, src, type,
                        expected_src, kMaxRtpDelta, label);
  };

  switch_and_verify(1, ctx->mcast_ip_addr[MTL_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P], 0,
                    "mcast 1");
  if (tx_sessions > 2) {
    switch_and_verify(2, ctx->mcast_ip_addr[MTL_PORT_R], ctx->mcast_ip_addr[MTL_PORT_R],
                      rand(), "mcast 2");
  }
  switch_and_verify(0, ctx->para.sip_addr[MTL_PORT_R], ctx->para.sip_addr[MTL_PORT_P],
                    rand(), "unicast 0");

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
