/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cinttypes>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "harness/pcapng.hpp"
#include "log.h"
#include "test_harness.hpp"

#define ST22_TEST_PAYLOAD_TYPE (114)
#define ST22_TEST_LOOPBACK_UDP_PORT (15000)

namespace {

using ScopedSt22Tx = mtl_test::ScopedHandle<st22_tx_handle, st22_tx_free>;
using ScopedSt22Rx = mtl_test::ScopedHandle<st22_rx_handle, st22_rx_free>;

struct UnlinkGuard {
  const char* path = nullptr;
  ~UnlinkGuard() {
    if (path && path[0] != '\0') unlink(path);
  }
};

inline int atomic_inc(int* p) {
  return __atomic_fetch_add(p, 1, __ATOMIC_RELAXED);
}

inline void set_handle(tests_context* c, void* h) {
  __atomic_store_n(&c->handle, h, __ATOMIC_RELEASE);
}

inline void* get_handle(const tests_context* c) {
  return __atomic_load_n(&c->handle, __ATOMIC_ACQUIRE);
}

/*
 * Steady-state rate window parameters for ST 2110-22 (25-59.94 fps):
 *   - warmup_frames = 3 absorbs TX/RX startup and RL pacing training.
 *   - min_window_ms = 1000, min_delta_frames = 25 collects 50-60 frames at
 *     50-59.94 fps (<= 2.0% edge error) and ~30 frames at 29.97 fps (<= 3.3%
 *     error), well inside the +/-10% FPS tolerance.
 */
constexpr int kSt22RateWarmupFrames = 6;
constexpr int kSt22RateMinDeltaFrames = 50;
constexpr int kSt22RateMinWindowMs = 2000;

struct St22SessionSpec {
  enum st22_type type = ST22_TYPE_FRAME_LEVEL;
  enum st_fps fps = ST_FPS_P59_94;
  int width = 1920;
  int height = 1080;
  int pkt_data_len = 1280;
  int total_pkts = 540;
};

/* Member order guarantees session handle is freed before context teardown. */
struct St22TxSession {
  mtl_test::ScopedContext ctx;
  ScopedSt22Tx handle;
};

struct St22RxSession {
  mtl_test::ScopedContext ctx;
  ScopedSt22Rx handle;
};

template <typename SessionVec>
std::vector<tests_context*> collect_ctxs(const SessionVec& sessions) {
  std::vector<tests_context*> ptrs;
  ptrs.reserve(sessions.size());
  for (const auto& s : sessions) ptrs.push_back(s.ctx.get());
  return ptrs;
}

}  // namespace

static int st22_rtp_notify(void* args) {
  auto ctx = (tests_context*)args;
  if (!get_handle(ctx)) return -EIO;
  std::unique_lock<std::mutex> lck(ctx->mtx);
  ctx->cv.notify_all();
  return 0;
}

static int st22_next_video_frame(void* priv, uint16_t* next_frame_idx,
                                 struct st22_tx_frame_meta* meta) {
  auto ctx = (tests_context*)priv;
  if (!get_handle(ctx)) return -EIO;

  *next_frame_idx = ctx->fb_idx;
  meta->codestream_size = ctx->frame_size;
  if (ctx->user_pacing) {
    meta->tfmt = ST10_TIMESTAMP_FMT_TAI;
    meta->timestamp = mtl_ptp_read_time(ctx->ctx->handle) + 35 * 1000 * 1000;
  }
  ctx->fb_idx = (ctx->fb_idx + 1) % ctx->fb_cnt;
  atomic_inc(&ctx->fb_send);
  return 0;
}

static int st22_frame_done(void* /*priv*/, uint16_t /*frame_idx*/,
                           struct st22_tx_frame_meta* /*meta*/) {
  return 0;
}

static void check_st22_rx_meta(tests_context* ctx,
                               const struct st22_rx_frame_meta* meta) {
  if (!meta || meta->status != ST_FRAME_STATUS_COMPLETE ||
      meta->tfmt != ST10_TIMESTAMP_FMT_MEDIA_CLK ||
      meta->timestamp != meta->rtp_timestamp || meta->pkts_total == 0 ||
      meta->pkts_recv[MTL_SESSION_PORT_P] < meta->pkts_total) {
    atomic_inc(&ctx->rx_meta_fail_cnt);
  }
}

static int st22_rx_frame_ready(void* priv, void* frame, struct st22_rx_frame_meta* meta) {
  auto ctx = (tests_context*)priv;
  void* h = get_handle(ctx);
  if (!h) return -EIO;
  check_st22_rx_meta(ctx, meta);
  if (!meta || meta->status != ST_FRAME_STATUS_COMPLETE) {
    atomic_inc(&ctx->incomplete_frame_cnt);
  }
  atomic_inc(&ctx->fb_rec);
  st22_rx_put_framebuff((st22_rx_handle)h, frame);
  return 0;
}

static void st22_tx_ops_init(tests_context* st22, struct st22_tx_ops* ops) {
  auto ctx = st22->ctx;
  memset(ops, 0, sizeof(*ops));
  ops->name = "st22_test";
  ops->priv = st22;
  ops->num_port = ctx->same_dual_port ? 1 : ctx->para.num_ports;
  memcpy(ops->dip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
         MTL_IP_ADDR_LEN);
  snprintf(ops->port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
           ctx->para.port[MTL_PORT_P]);
  ops->udp_port[MTL_SESSION_PORT_P] = 10000 + st22->idx;
  if (ops->num_port > 1) {
    memcpy(ops->dip_addr[MTL_SESSION_PORT_R], ctx->mcast_ip_addr[MTL_PORT_R],
           MTL_IP_ADDR_LEN);
    snprintf(ops->port[MTL_SESSION_PORT_R], MTL_PORT_MAX_LEN, "%s",
             ctx->para.port[MTL_PORT_R]);
    ops->udp_port[MTL_SESSION_PORT_R] = 10000 + st22->idx;
  }
  ops->pacing = ST21_PACING_NARROW;
  ops->width = 1920;
  ops->height = 1080;
  ops->fps = ST_FPS_P59_94;
  ops->payload_type = ST22_TEST_PAYLOAD_TYPE;
  ops->notify_rtp_done = st22_rtp_notify;
  ops->type = ST22_TYPE_FRAME_LEVEL;
  ops->rtp_ring_size = 1024;
  ops->rtp_pkt_size = 1280 + sizeof(struct st22_rfc9134_rtp_hdr);
  ops->framebuff_cnt = st22->fb_cnt;
  ops->framebuff_max_size = 0x100000;
  ops->notify_frame_done = st22_frame_done;
  ops->get_next_frame = st22_next_video_frame;
}

static void st22_rx_ops_init(tests_context* st22, struct st22_rx_ops* ops) {
  auto ctx = st22->ctx;
  memset(ops, 0, sizeof(*ops));
  ops->name = "st22_test";
  ops->priv = st22;
  ops->num_port = ctx->same_dual_port ? 1 : ctx->para.num_ports;
  memcpy(ops->ip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
         MTL_IP_ADDR_LEN);
  snprintf(ops->port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
           ctx->para.port[MTL_PORT_P]);
  ops->udp_port[MTL_SESSION_PORT_P] = 10000 + st22->idx;
  if (ops->num_port == 2) {
    memcpy(ops->ip_addr[MTL_SESSION_PORT_R], ctx->mcast_ip_addr[MTL_PORT_R],
           MTL_IP_ADDR_LEN);
    snprintf(ops->port[MTL_SESSION_PORT_R], MTL_PORT_MAX_LEN, "%s",
             ctx->para.port[MTL_PORT_R]);
    ops->udp_port[MTL_SESSION_PORT_R] = 10000 + st22->idx;
  }
  ops->pacing = ST21_PACING_NARROW;
  ops->width = 1920;
  ops->height = 1080;
  ops->fps = ST_FPS_P59_94;
  ops->payload_type = ST22_TEST_PAYLOAD_TYPE;
  ops->notify_rtp_ready = st22_rtp_notify;
  ops->rtp_ring_size = 1024;
  ops->type = ST22_TYPE_FRAME_LEVEL;
  ops->framebuff_cnt = st22->fb_cnt;
  ops->framebuff_max_size = 0x100000;
  ops->notify_frame_ready = st22_rx_frame_ready;
}

static void fill_loopback_tx_ops(struct st22_tx_ops* ops, tests_context* s_ctx,
                                 const St22SessionSpec& spec, uint16_t udp_port) {
  auto ctx = s_ctx->ctx;
  s_ctx->pkt_data_len = spec.pkt_data_len;
  s_ctx->total_pkts_in_frame = spec.total_pkts;
  s_ctx->frame_size = static_cast<size_t>(spec.pkt_data_len) * spec.total_pkts;

  st22_tx_ops_init(s_ctx, ops);
  ops->num_port = 1;
  memcpy(ops->dip_addr[MTL_SESSION_PORT_P], ctx->para.sip_addr[MTL_PORT_R],
         MTL_IP_ADDR_LEN);
  ops->udp_port[MTL_SESSION_PORT_P] = udp_port;
  ops->width = spec.width;
  ops->height = spec.height;
  ops->fps = spec.fps;
  ops->type = spec.type;
  ops->pack_type = ST22_PACK_CODESTREAM;
  ops->framebuff_max_size =
      s_ctx->frame_size + static_cast<size_t>(spec.pkt_data_len) * 100;
  ops->rtp_pkt_size = spec.pkt_data_len + sizeof(struct st22_rfc9134_rtp_hdr);
  ops->rtp_frame_total_pkts = spec.total_pkts;
}

static void fill_loopback_rx_ops(struct st22_rx_ops* ops, tests_context* s_ctx,
                                 const St22SessionSpec& spec, uint16_t udp_port) {
  auto ctx = s_ctx->ctx;
  s_ctx->pkt_data_len = spec.pkt_data_len;
  s_ctx->total_pkts_in_frame = spec.total_pkts;
  s_ctx->frame_size = static_cast<size_t>(spec.pkt_data_len) * spec.total_pkts;

  st22_rx_ops_init(s_ctx, ops);
  ops->num_port = 1;
  memcpy(ops->ip_addr[MTL_SESSION_PORT_P], ctx->para.sip_addr[MTL_PORT_P],
         MTL_IP_ADDR_LEN);
  snprintf(ops->port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
           ctx->para.port[MTL_PORT_R]);
  ops->udp_port[MTL_SESSION_PORT_P] = udp_port;
  ops->width = spec.width;
  ops->height = spec.height;
  ops->fps = spec.fps;
  ops->type = spec.type;
  ops->pack_type = ST22_PACK_CODESTREAM;
  ops->framebuff_max_size =
      s_ctx->frame_size + static_cast<size_t>(spec.pkt_data_len) * 100;
}

static void st22_tx_assert_cnt(int expect_cnt) {
  struct st_var_info var;
  EXPECT_GE(st_get_var_info(st_test_ctx()->handle, &var), 0);
  EXPECT_EQ(var.st22_tx_sessions_cnt, expect_cnt);
}

static void st22_rx_assert_cnt(int expect_cnt) {
  struct st_var_info var;
  EXPECT_GE(st_get_var_info(st_test_ctx()->handle, &var), 0);
  EXPECT_EQ(var.st22_rx_sessions_cnt, expect_cnt);
}

TEST(St22_tx, create_free_mix) {
  mtl_test::verify_create_free<struct st22_tx_ops, st22_tx_handle, st22_tx_create,
                               st22_tx_free>(st22_tx_ops_init, st22_tx_assert_cnt, 2, 3,
                                             4);
}
TEST(St22_tx, create_free_max) {
  mtl_test::verify_create_free_max<struct st22_tx_ops, st22_tx_handle, st22_tx_create,
                                   st22_tx_free>(st22_tx_ops_init, st22_tx_assert_cnt,
                                                 TEST_CREATE_FREE_MAX);
}
TEST(St22_tx, create_expect_fail) {
  mtl_test::verify_invalid_ports<struct st22_tx_ops, st22_tx_handle, st22_tx_create,
                                 st22_tx_free>(st22_tx_ops_init);
}
TEST(St22_tx, create_expect_fail_ring_sz) {
  auto rtp_ops_init = [](tests_context* c, struct st22_tx_ops* ops) {
    st22_tx_ops_init(c, ops);
    ops->type = ST22_TYPE_RTP_LEVEL;
    ops->rtp_frame_total_pkts = 1024;
    ops->rtp_pkt_size = 1260;
  };
  {
    auto* ctx = st_test_ctx();
    auto test_ctx = mtl_test::make_context(ctx, 0, 0);
    struct st22_tx_ops ops;
    rtp_ops_init(test_ctx.get(), &ops);
    ops.num_port = 1;
    ops.rtp_ring_size = 128;
    ScopedSt22Tx valid_handle(st22_tx_create(ctx->handle, &ops));
    ASSERT_TRUE(static_cast<bool>(valid_handle));
  }
  mtl_test::verify_rtp_ring_invalid<struct st22_tx_ops, st22_tx_handle, st22_tx_create,
                                    st22_tx_free>(rtp_ops_init, ST22_TYPE_RTP_LEVEL, 0);
  mtl_test::verify_rtp_ring_invalid<struct st22_tx_ops, st22_tx_handle, st22_tx_create,
                                    st22_tx_free>(rtp_ops_init, ST22_TYPE_RTP_LEVEL,
                                                  128 + 1);
}
/* RTP-level create takes rtp_pkt_size in [1:1452], so a 1396-byte ST 2022-6 pkt fits */
TEST(St22_tx, rtp_pkt_size) {
  mtl_test::verify_rtp_pkt_size<struct st22_tx_ops, st22_tx_handle, st22_tx_create,
                                st22_tx_free>(st22_tx_ops_init, ST22_TYPE_RTP_LEVEL, 0,
                                              false, /*set_total_pkts=*/true);
  /* the video TX limit: the 1494-byte TX frame minus the 42-byte Ethernet/IPv4/UDP hdr */
  mtl_test::verify_rtp_pkt_size<struct st22_tx_ops, st22_tx_handle, st22_tx_create,
                                st22_tx_free>(st22_tx_ops_init, ST22_TYPE_RTP_LEVEL, 1452,
                                              true, /*set_total_pkts=*/true);
  mtl_test::verify_rtp_pkt_size<struct st22_tx_ops, st22_tx_handle, st22_tx_create,
                                st22_tx_free>(st22_tx_ops_init, ST22_TYPE_RTP_LEVEL,
                                              1452 + 1, false, /*set_total_pkts=*/true);
}
TEST(St22_tx, create_expect_fail_fb_cnt) {
  mtl_test::verify_invalid_fb_cnt<struct st22_tx_ops, st22_tx_handle, st22_tx_create,
                                  st22_tx_free>(st22_tx_ops_init, 1);
  mtl_test::verify_invalid_fb_cnt<struct st22_tx_ops, st22_tx_handle, st22_tx_create,
                                  st22_tx_free>(st22_tx_ops_init, ST22_FB_MAX_COUNT + 1);
}
TEST(St22_tx, query_params) {
  auto* ctx = st_test_ctx();
  constexpr uint16_t kFbCnt = 3;
  auto test_ctx = mtl_test::make_context(ctx, 0, kFbCnt);

  struct st22_tx_ops ops;
  st22_tx_ops_init(test_ctx.get(), &ops);
  ops.num_port = 1;
  ScopedSt22Tx handle(st22_tx_create(ctx->handle, &ops));
  ASSERT_TRUE(static_cast<bool>(handle));

  void* addrs[kFbCnt] = {};
  for (uint16_t idx = 0; idx < kFbCnt; idx++) {
    addrs[idx] = st22_tx_get_fb_addr(handle.get(), idx);
    EXPECT_NE(addrs[idx], nullptr);
    for (uint16_t prev = 0; prev < idx; prev++) {
      EXPECT_NE(addrs[idx], addrs[prev]);
    }
  }
  EXPECT_EQ(st22_tx_get_fb_addr(handle.get(), kFbCnt), nullptr);
  EXPECT_EQ(st22_tx_get_fb_addr(handle.get(), kFbCnt * 2), nullptr);
  EXPECT_GE(st22_tx_get_sch_idx(handle.get()), 0);
}
TEST(St22_tx, put_mbuf_validation) {
  auto* ctx = st_test_ctx();
  auto test_ctx = mtl_test::make_context(ctx, 0, 3);

  struct st22_tx_ops ops;
  st22_tx_ops_init(test_ctx.get(), &ops);
  ops.num_port = 1;
  ops.type = ST22_TYPE_RTP_LEVEL;
  ops.flags |= ST22_TX_FLAG_USER_PACING;
  ops.rtp_frame_total_pkts = 540;
  ScopedSt22Tx handle(st22_tx_create(ctx->handle, &ops));
  ASSERT_TRUE(static_cast<bool>(handle));

  void* usrptr = nullptr;
  auto check_put = [&](uint16_t len, bool tai, bool expect_ok) {
    void* mbuf = st22_tx_get_mbuf(handle.get(), &usrptr);
    ASSERT_NE(mbuf, nullptr);
    ASSERT_NE(usrptr, nullptr);
    int ret = tai ? st22_tx_put_mbuf_tai(handle.get(), mbuf, len, NS_PER_S)
                  : st22_tx_put_mbuf(handle.get(), mbuf, len);
    if (expect_ok)
      EXPECT_EQ(ret, 0);
    else
      EXPECT_LT(ret, 0);
  };

  for (bool tai : {false, true}) {
    check_put(0, tai, false);
    check_put(ops.rtp_pkt_size + 1, tai, false);
    check_put(ops.rtp_pkt_size, tai, true);
  }
}

TEST(St22_rx, create_free_mix) {
  mtl_test::verify_create_free<struct st22_rx_ops, st22_rx_handle, st22_rx_create,
                               st22_rx_free>(st22_rx_ops_init, st22_rx_assert_cnt, 2, 3,
                                             4);
}
TEST(St22_rx, create_free_max) {
  mtl_test::verify_create_free_max<struct st22_rx_ops, st22_rx_handle, st22_rx_create,
                                   st22_rx_free>(st22_rx_ops_init, st22_rx_assert_cnt,
                                                 TEST_CREATE_FREE_MAX);
}
TEST(St22_rx, create_expect_fail) {
  mtl_test::verify_invalid_ports<struct st22_rx_ops, st22_rx_handle, st22_rx_create,
                                 st22_rx_free>(st22_rx_ops_init);
}
TEST(St22_rx, create_expect_fail_ring_sz) {
  mtl_test::verify_rtp_ring_invalid<struct st22_rx_ops, st22_rx_handle, st22_rx_create,
                                    st22_rx_free>(st22_rx_ops_init, ST22_TYPE_RTP_LEVEL,
                                                  0);
  mtl_test::verify_rtp_ring_invalid<struct st22_rx_ops, st22_rx_handle, st22_rx_create,
                                    st22_rx_free>(st22_rx_ops_init, ST22_TYPE_RTP_LEVEL,
                                                  128 + 1);
}
TEST(St22_rx, create_expect_fail_fb_cnt) {
  mtl_test::verify_invalid_fb_cnt<struct st22_rx_ops, st22_rx_handle, st22_rx_create,
                                  st22_rx_free>(st22_rx_ops_init, 1);
  mtl_test::verify_invalid_fb_cnt<struct st22_rx_ops, st22_rx_handle, st22_rx_create,
                                  st22_rx_free>(st22_rx_ops_init, ST22_FB_MAX_COUNT + 1);
}
TEST(St22_rx, query_params) {
  auto* ctx = st_test_ctx();
  constexpr uint16_t kFbCnt = 3;
  auto test_ctx = mtl_test::make_context(ctx, 0, kFbCnt);

  struct st22_rx_ops ops;
  st22_rx_ops_init(test_ctx.get(), &ops);
  ops.num_port = 1;
  ScopedSt22Rx handle(st22_rx_create(ctx->handle, &ops));
  ASSERT_TRUE(static_cast<bool>(handle));

  void* addrs[kFbCnt] = {};
  for (uint16_t idx = 0; idx < kFbCnt; idx++) {
    addrs[idx] = st22_rx_get_fb_addr(handle.get(), idx);
    EXPECT_NE(addrs[idx], nullptr);
    for (uint16_t prev = 0; prev < idx; prev++) {
      EXPECT_NE(addrs[idx], addrs[prev]);
    }
  }
  EXPECT_EQ(st22_rx_get_fb_addr(handle.get(), kFbCnt), nullptr);
  EXPECT_EQ(st22_rx_get_fb_addr(handle.get(), kFbCnt * 2), nullptr);
  EXPECT_GE(st22_rx_get_sch_idx(handle.get()), 0);
}

static int st22_tx_build_rtp_packet(tests_context* s, struct st22_rfc9134_rtp_hdr* rtp,
                                    uint16_t* pkt_len) {
  rtp->base.csrc_count = 0;
  rtp->base.extension = 0;
  rtp->base.padding = 0;
  rtp->base.version = 2;
  rtp->base.marker = 0;
  rtp->base.payload_type = ST22_TEST_PAYLOAD_TYPE;
  rtp->base.tmstamp = htonl(s->rtp_tmstamp);
  int seq = __atomic_fetch_add(&s->seq_id, 1, __ATOMIC_RELAXED);
  rtp->base.seq_number = htons(seq);

  *pkt_len = s->pkt_data_len + sizeof(*rtp);
  s->pkt_idx++;
  if (s->pkt_idx >= s->total_pkts_in_frame) {
    rtp->base.marker = 1;
    s->fb_idx++;
    s->pkt_idx = 0;
    s->rtp_tmstamp++;
    atomic_inc(&s->fb_send);
  }
  return 0;
}

/* tai_of, if set, gives the TAI of the frame a first packet starts, by RTP timestamp */
static void st22_tx_feed_packet(tests_context* ctx,
                                std::function<uint64_t(uint32_t)> tai_of = nullptr) {
  auto handle = (st22_tx_handle)get_handle(ctx);
  void* usrptr = NULL;
  uint16_t mbuf_len = 0;
  std::unique_lock<std::mutex> lck(ctx->mtx, std::defer_lock);
  while (!ctx->stop) {
    void* mbuf = st22_tx_get_mbuf(handle, &usrptr);
    if (!mbuf) {
      lck.lock();
      mbuf = st22_tx_get_mbuf(handle, &usrptr);
      if (!mbuf) {
        if (!ctx->stop) ctx->cv.wait(lck);
        lck.unlock();
        continue;
      }
      lck.unlock();
    }

    uint64_t tai = (tai_of && !ctx->pkt_idx) ? tai_of(ctx->rtp_tmstamp) : 0;
    st22_tx_build_rtp_packet(ctx, (struct st22_rfc9134_rtp_hdr*)usrptr, &mbuf_len);
    if (tai_of)
      st22_tx_put_mbuf_tai(handle, mbuf, mbuf_len, tai);
    else
      st22_tx_put_mbuf(handle, mbuf, mbuf_len);
  }
}

/* on_packet, if set, sees every packet after the new frame check */
static void st22_rx_get_packet(
    tests_context* ctx,
    std::function<void(tests_context*, uint32_t, bool)> on_packet = nullptr) {
  auto handle = (st22_rx_handle)get_handle(ctx);
  void* usrptr = NULL;
  uint16_t mbuf_len = 0;
  std::unique_lock<std::mutex> lck(ctx->mtx, std::defer_lock);
  while (!ctx->stop) {
    void* mbuf = st22_rx_get_mbuf(handle, &usrptr, &mbuf_len);
    if (!mbuf) {
      lck.lock();
      mbuf = st22_rx_get_mbuf(handle, &usrptr, &mbuf_len);
      if (!mbuf) {
        if (!ctx->stop) ctx->cv.wait(lck);
        lck.unlock();
        continue;
      }
      lck.unlock();
    }
    if (mbuf_len <= sizeof(struct st22_rfc9134_rtp_hdr)) {
      atomic_inc(&ctx->incomplete_frame_cnt);
      st22_rx_put_mbuf(handle, mbuf);
      continue;
    }
    atomic_inc(&ctx->packet_rec);
    auto* hdr = (struct st22_rfc9134_rtp_hdr*)usrptr;
    bool newframe = false;
    uint32_t tmstamp = ntohl(hdr->base.tmstamp);
    if (tmstamp != ctx->rtp_tmstamp) {
      ctx->rtp_tmstamp = tmstamp;
      atomic_inc(&ctx->fb_rec);
      newframe = true;
    }
    if (on_packet) on_packet(ctx, tmstamp, newframe);
    st22_rx_put_mbuf(handle, mbuf);
  }
}

struct St22LoopbackConfig {
  uint16_t fb_cnt = 3;
  bool start_rtp_workers = false;
  std::function<void(size_t, tests_context*, struct st22_tx_ops*)> on_tx_ops;
  std::function<void(size_t, St22TxSession&)> on_tx_created;
  std::function<void(size_t, tests_context*, struct st22_rx_ops*)> on_rx_ops;
  std::function<void(size_t, St22RxSession&)> on_rx_created;
};

static void create_st22_loopback(struct st_tests_context* ctx,
                                 const std::vector<St22SessionSpec>& tx_specs,
                                 const std::vector<St22SessionSpec>& rx_specs,
                                 std::vector<St22TxSession>& tx,
                                 std::vector<St22RxSession>& rx,
                                 std::vector<mtl_test::ScopedWorker>* workers = nullptr,
                                 const St22LoopbackConfig& cfg = {}) {
  auto m_handle = ctx->handle;
  tx.reserve(tx_specs.size());
  rx.reserve(rx_specs.size());

  for (size_t i = 0; i < tx_specs.size(); i++) {
    const auto& spec = tx_specs[i];
    tx.emplace_back();
    auto& s = tx.back();
    s.ctx = mtl_test::make_context(ctx, static_cast<int>(i), cfg.fb_cnt);

    struct st22_tx_ops ops_tx;
    fill_loopback_tx_ops(&ops_tx, s.ctx.get(), spec,
                         ST22_TEST_LOOPBACK_UDP_PORT + static_cast<uint16_t>(i * 2));
    if (cfg.on_tx_ops) cfg.on_tx_ops(i, s.ctx.get(), &ops_tx);
    s.handle.reset(st22_tx_create(m_handle, &ops_tx));
    ASSERT_TRUE(static_cast<bool>(s.handle));
    if (cfg.on_tx_created) {
      cfg.on_tx_created(i, s);
      if (::testing::Test::HasFatalFailure()) return;
    }
    set_handle(s.ctx.get(), s.handle.get());
    if (cfg.start_rtp_workers && workers && spec.type == ST22_TYPE_RTP_LEVEL) {
      workers->emplace_back(s.ctx.get(),
                            std::thread([c = s.ctx.get()]() { st22_tx_feed_packet(c); }));
    }
  }

  for (size_t i = 0; i < rx_specs.size(); i++) {
    const auto& spec = rx_specs[i];
    rx.emplace_back();
    auto& s = rx.back();
    s.ctx = mtl_test::make_context(ctx, static_cast<int>(i), cfg.fb_cnt);

    struct st22_rx_ops ops_rx;
    fill_loopback_rx_ops(&ops_rx, s.ctx.get(), spec,
                         ST22_TEST_LOOPBACK_UDP_PORT + static_cast<uint16_t>(i * 2));
    if (cfg.on_rx_ops) cfg.on_rx_ops(i, s.ctx.get(), &ops_rx);
    s.handle.reset(st22_rx_create(m_handle, &ops_rx));
    ASSERT_TRUE(static_cast<bool>(s.handle));
    set_handle(s.ctx.get(), s.handle.get());
    if (cfg.on_rx_created) {
      cfg.on_rx_created(i, s);
      if (::testing::Test::HasFatalFailure()) return;
    }
    if (cfg.start_rtp_workers && workers && spec.type == ST22_TYPE_RTP_LEVEL) {
      workers->emplace_back(s.ctx.get(),
                            std::thread([c = s.ctx.get()]() { st22_rx_get_packet(c); }));
    }
  }
}

static void assert_rx_rates(const std::vector<St22RxSession>& rx,
                            const std::vector<St22SessionSpec>& specs,
                            const mtl_test::RateWindowResult& res) {
  for (size_t i = 0; i < specs.size(); i++) {
    const double expect_fps = st_frame_rate(specs[i].fps);
    const int fb_rec = mtl_test::rx_frames(rx[i].ctx.get());
    EXPECT_GT(fb_rec, 0);
    EXPECT_EQ(mtl_test::relaxed_load(&rx[i].ctx->rx_meta_fail_cnt), 0);
    info("%s, session %zu fb_rec %d framerate %f\n", __func__, i, fb_rec, res.rate[i]);
    EXPECT_NEAR(res.rate[i], expect_fps, expect_fps * 0.1);
  }
}

TEST(St22_rx, fps_1080p_s2) {
  auto ctx = st_test_ctx();
  MTL_TEST_REQUIRE_PORTS(ctx, 2);

  const std::vector<St22SessionSpec> specs = {
      {ST22_TYPE_FRAME_LEVEL, ST_FPS_P59_94, 1920, 1080, 1280, 546},
      {ST22_TYPE_RTP_LEVEL, ST_FPS_P50, 1920, 1080, 1280, 540},
  };
  std::vector<St22TxSession> tx;
  std::vector<St22RxSession> rx;
  mtl_test::ScopedMtlStart mtl_guard(ctx->handle);
  std::vector<mtl_test::ScopedWorker> workers;

  St22LoopbackConfig cfg;
  cfg.start_rtp_workers = true;
  create_st22_loopback(ctx, specs, specs, tx, rx, &workers, cfg);
  if (::testing::Test::HasFatalFailure()) return;

  EXPECT_GE(mtl_guard.start(), 0);
  auto rate_res = mtl_test::measure_rate_window(
      collect_ctxs(rx), mtl_test::rx_frames, kSt22RateWarmupFrames,
      kSt22RateMinDeltaFrames, kSt22RateMinWindowMs);
  EXPECT_TRUE(rate_res.warmed_up);
  EXPECT_TRUE(rate_res.complete);

  for (auto& w : workers) w.stop();
  EXPECT_GE(mtl_guard.stop(), 0);
  assert_rx_rates(rx, specs, rate_res);
  for (size_t i = 0; i < specs.size(); i++) {
    EXPECT_EQ(mtl_test::relaxed_load(&rx[i].ctx->incomplete_frame_cnt), 0);
  }
  EXPECT_GT(mtl_test::relaxed_load(&rx[1].ctx->packet_rec), 0);
}

namespace {

struct St22SigTracker {
  static constexpr int kMaxSources = 4;
  static constexpr size_t kSigLen = 8;
  std::atomic<int> matched[kMaxSources];
  std::atomic<int> unknown;

  St22SigTracker() {
    reset();
  }

  void reset() {
    for (int i = 0; i < kMaxSources; ++i) {
      matched[i].store(0, std::memory_order_relaxed);
    }
    unknown.store(0, std::memory_order_relaxed);
  }

  void inspect(const void* frame, size_t len) {
    if (!frame || len < kSigLen) {
      unknown.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    const auto* bytes = static_cast<const uint8_t*>(frame);
    const uint8_t tag = bytes[0];
    if (tag < 0xA0 || tag >= static_cast<uint8_t>(0xA0 + kMaxSources)) {
      unknown.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    for (size_t k = 1; k < kSigLen; ++k) {
      if (bytes[k] != tag) {
        unknown.fetch_add(1, std::memory_order_relaxed);
        return;
      }
    }
    matched[tag - 0xA0].fetch_add(1, std::memory_order_relaxed);
  }
};

struct St22SigRxPriv {
  tests_context* ctx = nullptr;
  St22SigTracker* tracker = nullptr;
};

int st22_sig_rx_frame_ready(void* priv, void* frame, struct st22_rx_frame_meta* meta) {
  auto* p = static_cast<St22SigRxPriv*>(priv);
  tests_context* ctx = p->ctx;
  void* h = get_handle(ctx);
  if (!h) return -EIO;
  if (p->tracker) p->tracker->inspect(frame, meta->frame_total_size);
  atomic_inc(&ctx->fb_rec);
  st22_rx_put_framebuff(static_cast<st22_rx_handle>(h), frame);
  return 0;
}

}  // namespace

static void st22_rx_update_src_test(int tx_sessions, bool tx_update_dst = false) {
  auto ctx = st_test_ctx();
  MTL_TEST_REQUIRE_PORTS(ctx, 2);
  ASSERT_LE(tx_sessions, St22SigTracker::kMaxSources);

  const St22SessionSpec spec{ST22_TYPE_FRAME_LEVEL, ST_FPS_P59_94, 1920, 1080, 1280, 520};
  const double expect_fps = st_frame_rate(spec.fps);

  /* Declare tracker before sessions/mtl_guard so callbacks never outlive it. */
  St22SigTracker tracker;
  St22SigRxPriv sig_rx_priv{};
  std::vector<St22TxSession> tx;
  std::vector<St22RxSession> rx;
  St22LoopbackConfig cfg;
  cfg.on_tx_ops = [ctx](size_t i, tests_context*, struct st22_tx_ops* ops) {
    const uint8_t* ip = (i == 2)   ? ctx->mcast_ip_addr[MTL_PORT_R]
                        : (i == 1) ? ctx->mcast_ip_addr[MTL_PORT_P]
                                   : ctx->para.sip_addr[MTL_PORT_R];
    memcpy(ops->dip_addr[MTL_SESSION_PORT_P], ip, MTL_IP_ADDR_LEN);
  };
  cfg.on_tx_created = [&cfg](size_t i, St22TxSession& s) {
    const uint8_t tag = static_cast<uint8_t>(0xA0 + i);
    for (uint16_t f = 0; f < cfg.fb_cnt; ++f) {
      auto* fb = static_cast<uint8_t*>(st22_tx_get_fb_addr(s.handle.get(), f));
      ASSERT_NE(fb, nullptr);
      memset(fb, tag, St22SigTracker::kSigLen);
    }
  };
  cfg.on_rx_ops = [&tracker, &sig_rx_priv](size_t, tests_context* rx_ctx,
                                           struct st22_rx_ops* ops) {
    sig_rx_priv.ctx = rx_ctx;
    sig_rx_priv.tracker = &tracker;
    ops->priv = &sig_rx_priv;
    ops->notify_frame_ready = st22_sig_rx_frame_ready;
  };
  create_st22_loopback(ctx, std::vector<St22SessionSpec>(tx_sessions, spec), {spec}, tx,
                       rx, nullptr, cfg);
  if (::testing::Test::HasFatalFailure()) return;

  mtl_test::ScopedMtlStart mtl_guard(ctx->handle);
  EXPECT_GE(mtl_guard.start(), 0);
  auto rx_ctxs = collect_ctxs(rx);

  EXPECT_TRUE(
      mtl_test::wait_for_all(rx_ctxs, mtl_test::rx_frames, kSt22RateWarmupFrames, 6000));
  EXPECT_GT(tracker.matched[0].load(std::memory_order_relaxed), 0);
  for (int s_idx = 1; s_idx < tx_sessions; ++s_idx) {
    EXPECT_EQ(tracker.matched[s_idx].load(std::memory_order_relaxed), 0);
  }

  auto verify_switch = [&](const char* label, const uint8_t* tx_dst_ip,
                           const uint8_t* rx_src_ip, uint16_t udp_port, int target_idx) {
    if (tx_update_dst) {
      struct st_tx_dest_info dst = {};
      dst.udp_port[MTL_SESSION_PORT_P] = udp_port;
      memcpy(dst.dip_addr[MTL_SESSION_PORT_P], tx_dst_ip, MTL_IP_ADDR_LEN);
      EXPECT_GE(st22_tx_update_destination(tx[0].handle.get(), &dst), 0);
    }

    struct st_rx_source_info src = {};
    src.udp_port[MTL_SESSION_PORT_P] = udp_port;
    memcpy(src.ip_addr[MTL_SESSION_PORT_P], rx_src_ip, MTL_IP_ADDR_LEN);
    EXPECT_GE(st22_rx_update_source(rx[0].handle.get(), &src), 0);

    /* Allow in-flight frames from the previous source to drain before counting. */
    st_usleep(150 * 1000);
    tracker.reset();

    const int base_rec = mtl_test::rx_frames(rx[0].ctx.get());
    auto delta_rec = [base_rec](const tests_context* c) {
      return mtl_test::rx_frames(c) - base_rec;
    };
    auto rate_res =
        mtl_test::measure_rate_window(rx_ctxs, delta_rec, kSt22RateWarmupFrames,
                                      kSt22RateMinDeltaFrames, kSt22RateMinWindowMs);
    EXPECT_TRUE(rate_res.warmed_up);
    EXPECT_TRUE(rate_res.complete);
    const int fb_rec = mtl_test::rx_frames(rx[0].ctx.get()) - base_rec;
    EXPECT_GT(fb_rec, 0);
    info("%s, %s fb_rec %d framerate %f\n", __func__, label, fb_rec, rate_res.rate[0]);
    EXPECT_NEAR(rate_res.rate[0], expect_fps, expect_fps * 0.1);

    EXPECT_GT(tracker.matched[target_idx].load(std::memory_order_relaxed), 5)
        << label << " did not receive frames from source " << target_idx;
    for (int s_idx = 0; s_idx < tx_sessions; ++s_idx) {
      if (s_idx == target_idx) continue;
      EXPECT_EQ(tracker.matched[s_idx].load(std::memory_order_relaxed), 0)
          << label << " still received frames from stale source " << s_idx;
    }
    EXPECT_EQ(tracker.unknown.load(std::memory_order_relaxed), 0)
        << label << " received frames with unexpected signature";
  };

  verify_switch("mcast 1", ctx->mcast_ip_addr[MTL_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
                ST22_TEST_LOOPBACK_UDP_PORT + 2, tx_update_dst ? 0 : 1);
  if (tx_sessions > 2) {
    verify_switch("mcast 2", ctx->mcast_ip_addr[MTL_PORT_R],
                  ctx->mcast_ip_addr[MTL_PORT_R], ST22_TEST_LOOPBACK_UDP_PORT + 4, 2);
  }
  verify_switch("unicast 0", ctx->para.sip_addr[MTL_PORT_R],
                ctx->para.sip_addr[MTL_PORT_P], ST22_TEST_LOOPBACK_UDP_PORT, 0);

  EXPECT_GE(mtl_guard.stop(), 0);
}

TEST(St22_rx, update_source) {
  st22_rx_update_src_test(2);
}

TEST(St22_tx, update_destination) {
  st22_rx_update_src_test(1, /*tx_update_dst=*/true);
}

TEST(St22_rx, after_start_s2) {
  auto ctx = st_test_ctx();
  MTL_TEST_REQUIRE_PORTS(ctx, 2);

  const std::vector<St22SessionSpec> specs = {
      {ST22_TYPE_FRAME_LEVEL, ST_FPS_P59_94, 1920, 1080, 1280, 540},
      {ST22_TYPE_FRAME_LEVEL, ST_FPS_P50, 1280, 720, 1300, 150},
  };
  mtl_test::ScopedMtlStart mtl_guard(ctx->handle);
  EXPECT_GE(mtl_guard.start(), 0);

  for (int r = 0; r < 2; r++) {
    std::vector<St22TxSession> tx;
    std::vector<St22RxSession> rx;
    create_st22_loopback(ctx, specs, specs, tx, rx);
    if (::testing::Test::HasFatalFailure()) return;

    auto rate_res = mtl_test::measure_rate_window(
        collect_ctxs(rx), mtl_test::rx_frames, kSt22RateWarmupFrames,
        kSt22RateMinDeltaFrames, kSt22RateMinWindowMs);
    EXPECT_TRUE(rate_res.warmed_up);
    EXPECT_TRUE(rate_res.complete);
    assert_rx_rates(rx, specs, rate_res);
  }

  EXPECT_GE(mtl_guard.stop(), 0);
}

TEST(St22_rx, pcap_dump) {
  auto ctx = st_test_ctx();
  MTL_TEST_REQUIRE_PORTS(ctx, 2);
  if (!mtl_pmd_is_dpdk_based(ctx->handle, MTL_PORT_R)) {
    GTEST_SKIP() << "MTL_PORT_R is not a DPDK-based PMD";
  }

  const std::vector<St22SessionSpec> specs = {
      {ST22_TYPE_FRAME_LEVEL, ST_FPS_P59_94, 1280, 720, 1280, 540},
  };
  std::vector<St22TxSession> tx;
  std::vector<St22RxSession> rx;
  create_st22_loopback(ctx, specs, specs, tx, rx);
  if (::testing::Test::HasFatalFailure()) return;

  mtl_test::ScopedMtlStart mtl_guard(ctx->handle);
  EXPECT_GE(mtl_guard.start(), 0);

  EXPECT_TRUE(mtl_test::wait_for_all(collect_ctxs(rx), mtl_test::rx_frames,
                                     kSt22RateWarmupFrames, 6000));

  constexpr uint32_t kMaxDumpPackets = 100;
  struct st_pcap_dump_meta meta = {};
  int ret = st22_rx_pcapng_dump(rx[0].handle.get(), kMaxDumpPackets, true, &meta);
  UnlinkGuard unlink_guard{meta.file_name[MTL_SESSION_PORT_P]};
  EXPECT_GE(ret, 0);
  if (ret >= 0) {
    EXPECT_EQ(meta.dumped_packets[MTL_SESSION_PORT_P], kMaxDumpPackets);
    struct stat st_buf = {};
    EXPECT_EQ(stat(meta.file_name[MTL_SESSION_PORT_P], &st_buf), 0);
    EXPECT_GT(st_buf.st_size, 0);
    mtl_test::verify_pcapng_file(meta.file_name[MTL_SESSION_PORT_P], kMaxDumpPackets);
  }

  EXPECT_GE(mtl_guard.stop(), 0);
}

static constexpr size_t kSt22BoxesHdrLen = 60u;

static int st22_digest_rx_frame_ready(void* priv, void* frame,
                                      struct st22_rx_frame_meta* meta) {
  auto ctx = (tests_context*)priv;
  void* h = get_handle(ctx);
  if (!h) return -EIO;

  check_st22_rx_meta(ctx, meta);
  const size_t box_hdr_len = static_cast<size_t>(ctx->pkt_data_len);
  if (!meta || meta->frame_total_size != ctx->frame_size + box_hdr_len) {
    atomic_inc(&ctx->incomplete_frame_cnt);
    st22_rx_put_framebuff((st22_rx_handle)h, frame);
    return 0;
  }

  {
    std::unique_lock<std::mutex> lck(ctx->mtx);
    if (!ctx->stop.load() && ctx->buf_q.empty()) {
      ctx->buf_q.push(frame);
      ctx->cv.notify_all();
    } else {
      st22_rx_put_framebuff((st22_rx_handle)h, frame);
    }
  }
  atomic_inc(&ctx->fb_rec);
  return 0;
}

static void st22_digest_rx_frame_check(void* args) {
  auto ctx = (tests_context*)args;
  auto handle = (st22_rx_handle)get_handle(ctx);
  unsigned char result[SHA256_DIGEST_LENGTH];
  const size_t box_hdr_len = static_cast<size_t>(ctx->pkt_data_len);
  while (true) {
    void* frame = nullptr;
    {
      std::unique_lock<std::mutex> lck(ctx->mtx);
      ctx->cv.wait(lck, [&]() { return !ctx->buf_q.empty() || ctx->stop.load(); });
      if (ctx->stop) {
        while (!ctx->buf_q.empty()) {
          st22_rx_put_framebuff(handle, ctx->buf_q.front());
          ctx->buf_q.pop();
        }
        break;
      }
      frame = ctx->buf_q.front();
      ctx->buf_q.pop();
    }
    const auto* bytes = static_cast<const unsigned char*>(frame);
    bool hdr_ok = true;
    if (box_hdr_len == kSt22BoxesHdrLen) {
      uint32_t jpvs_len = 0, colr_len = 0;
      memcpy(&jpvs_len, bytes, sizeof(jpvs_len));
      memcpy(&colr_len, bytes + 42, sizeof(colr_len));
      hdr_ok = ntohl(jpvs_len) == 42u && memcmp(bytes + 4, "jpvs", 4) == 0 &&
               ntohl(colr_len) == 18u && memcmp(bytes + 46, "colr", 4) == 0;
    }
    SHA256(bytes + box_hdr_len, ctx->frame_size, result);
    int i;
    for (i = 0; i < ST22_TEST_SHA_HIST_NUM; i++) {
      if (!memcmp(result, ctx->shas[i], SHA256_DIGEST_LENGTH)) break;
    }
    if (!hdr_ok || i >= ST22_TEST_SHA_HIST_NUM) {
      test_sha_dump("st22_rx_error_sha", result);
      atomic_inc(&ctx->sha_fail_cnt);
    }
    atomic_inc(&ctx->check_sha_frame_cnt);
    st22_rx_put_framebuff(handle, frame);
  }
}

/* Frame-level SHA256 digest and steady-state FPS verification. */
static void st22_rx_digest_test(const std::vector<St22SessionSpec>& specs,
                                bool enable_rtcp = false) {
  auto ctx = st_test_ctx();
  MTL_TEST_REQUIRE_PORTS(ctx, 2);

  std::vector<St22TxSession> tx;
  std::vector<St22RxSession> rx;
  mtl_test::ScopedMtlStart mtl_guard(ctx->handle);
  std::vector<mtl_test::ScopedWorker> workers;

  St22LoopbackConfig cfg;
  cfg.fb_cnt = ST22_TEST_SHA_HIST_NUM;
  cfg.on_tx_ops = [enable_rtcp](size_t, tests_context*, struct st22_tx_ops* ops) {
    if (enable_rtcp) {
      ops->flags |= ST22_TX_FLAG_ENABLE_RTCP;
      ops->rtcp.buffer_size = 512;
    }
  };
  cfg.on_tx_created = [](size_t, St22TxSession& s) {
    size_t frame_size = s.ctx->frame_size;
    for (int frame = 0; frame < ST22_TEST_SHA_HIST_NUM; frame++) {
      uint8_t* fb = (uint8_t*)st22_tx_get_fb_addr(s.handle.get(), frame);
      ASSERT_TRUE(fb != NULL);
      st_test_rand_data(fb, frame_size, frame);
      SHA256((unsigned char*)fb, frame_size, s.ctx->shas[frame]);
      test_sha_dump("st22_tx", s.ctx->shas[frame]);
    }
  };
  cfg.on_rx_ops = [&tx, enable_rtcp, &specs](size_t i, tests_context* rx_ctx,
                                             struct st22_rx_ops* ops) {
    ops->notify_frame_ready = st22_digest_rx_frame_ready;
    memcpy(rx_ctx->shas, tx[i].ctx->shas, ST22_TEST_SHA_HIST_NUM * SHA256_DIGEST_LENGTH);
    rx_ctx->pkt_data_len = 0;
    if (specs.size() > 1 && i == 1) {
      /* Preserve TX-generated jpvs/colr boxes on session 1 so both wire box
       * insertion and RX box stripping (session 0) are verified end to end. */
      ops->flags |= ST22_RX_FLAG_DISABLE_BOXES;
      ops->framebuff_max_size += kSt22BoxesHdrLen;
      rx_ctx->pkt_data_len = static_cast<int>(kSt22BoxesHdrLen);
    }
    if (enable_rtcp) {
      ops->flags |= ST22_RX_FLAG_ENABLE_RTCP | ST22_RX_FLAG_SIMULATE_PKT_LOSS;
      ops->rtcp.nack_interval_us = 100;
      ops->rtcp.seq_skip_window = 0;
      ops->rtcp.burst_loss_max = 4;
      ops->rtcp.sim_loss_rate = 0.002f;
    }
  };
  cfg.on_rx_created = [&workers](size_t, St22RxSession& s) {
    workers.emplace_back(s.ctx.get(),
                         std::thread(st22_digest_rx_frame_check, s.ctx.get()));
    struct st_queue_meta meta = {};
    EXPECT_GE(st22_rx_get_queue_meta(s.handle.get(), &meta), 0);
    EXPECT_EQ(meta.num_port, 1);
  };
  create_st22_loopback(ctx, specs, specs, tx, rx, &workers, cfg);
  if (::testing::Test::HasFatalFailure()) return;

  EXPECT_GE(mtl_guard.start(), 0);
  auto rx_ctxs = collect_ctxs(rx);
  auto rate_res =
      mtl_test::measure_rate_window(rx_ctxs, mtl_test::rx_frames, kSt22RateWarmupFrames,
                                    kSt22RateMinDeltaFrames, kSt22RateMinWindowMs);
  EXPECT_TRUE(rate_res.warmed_up);
  EXPECT_TRUE(rate_res.complete);
  EXPECT_TRUE(mtl_test::wait_for_all(rx_ctxs, mtl_test::sha_checked_frames, 1, 2000));

  for (auto& w : workers) w.stop();
  EXPECT_GE(mtl_guard.stop(), 0);

  assert_rx_rates(rx, specs, rate_res);
  for (size_t i = 0; i < specs.size(); i++) {
    EXPECT_GT(mtl_test::sha_checked_frames(rx[i].ctx.get()), 0);
    EXPECT_EQ(mtl_test::relaxed_load(&rx[i].ctx->sha_fail_cnt), 0);
    EXPECT_EQ(mtl_test::relaxed_load(&rx[i].ctx->incomplete_frame_cnt), 0);
  }
}

TEST(St22_rx, digest_s2) {
  st22_rx_digest_test({
      {ST22_TYPE_FRAME_LEVEL, ST_FPS_P59_94, 1920, 1080, 1280, 551},
      {ST22_TYPE_FRAME_LEVEL, ST_FPS_P50, 1920, 1080, 1280, 1520},
  });
}

TEST(St22_rx, digest_rtcp_s2) {
  st22_rx_digest_test(
      {
          {ST22_TYPE_FRAME_LEVEL, ST_FPS_P59_94, 1920, 1080, 1280, 551},
          {ST22_TYPE_FRAME_LEVEL, ST_FPS_P50, 1920, 1080, 1280, 1520},
      },
      true);
}

TEST(St22_tx, tx_user_pacing) {
  auto ctx = st_test_ctx();
  MTL_TEST_REQUIRE_PORTS(ctx, 2);

  const std::vector<St22SessionSpec> specs = {
      {ST22_TYPE_FRAME_LEVEL, ST_FPS_P59_94, 1920, 1080, 1260, 602},
  };
  std::vector<St22TxSession> tx;
  std::vector<St22RxSession> rx;
  St22LoopbackConfig cfg;
  cfg.on_tx_ops = [](size_t, tests_context* tx_ctx, struct st22_tx_ops* ops) {
    tx_ctx->user_pacing = true;
    ops->flags = ST22_TX_FLAG_USER_PACING;
  };
  create_st22_loopback(ctx, specs, specs, tx, rx, nullptr, cfg);
  if (::testing::Test::HasFatalFailure()) return;

  mtl_test::ScopedMtlStart mtl_guard(ctx->handle);
  EXPECT_GE(mtl_guard.start(), 0);

  /* Half of 59.94 fps (~29.97 fps): 1.0 s window collects ~30 frames (>= 25),
   * <= 3.3% quantization error, well inside +/-10% for both TX and RX rates. */
  auto win = mtl_test::measure_pipeline_window(
      collect_ctxs(tx), collect_ctxs(rx), kSt22RateWarmupFrames, kSt22RateMinDeltaFrames,
      kSt22RateMinWindowMs);
  EXPECT_TRUE(win.warmed_up);
  EXPECT_TRUE(win.complete);
  EXPECT_GE(mtl_guard.stop(), 0);

  /* +35 ms TAI timestamp skips every other 16.68 ms epoch -> half frame rate. */
  const double expect_fps = st_frame_rate(specs[0].fps) / 2;
  EXPECT_GT(mtl_test::rx_frames(rx[0].ctx.get()), 0);
  EXPECT_NEAR(win.rx_fps[0], expect_fps, expect_fps * 0.1);
  EXPECT_NEAR(win.tx_fps[0], expect_fps, expect_fps * 0.1);
}

namespace {
/* Frame k (RTP timestamp k + 1) is put with a TAI on every second epoch from
 * base_epoch, off the grid alternately by +0.4 and -0.3 frame: only nearest-epoch
 * rounding matches. */
struct St22RtpPacing {
  uint64_t base_epoch = 0;
  long double frame_ns = 0;
  std::mutex mtx;
  std::vector<std::pair<uint32_t, uint64_t>> arrivals;

  uint64_t epoch(uint32_t rtp_ts) const {
    return base_epoch + 2 * (uint64_t)(rtp_ts - 1);
  }

  uint64_t target(uint32_t rtp_ts) const {
    long double off = (rtp_ts % 2) ? 0.4L : -0.3L;
    return (uint64_t)((epoch(rtp_ts) + off) * frame_ns);
  }

  void record_first_packet(tests_context* ctx, uint32_t tmstamp, bool newframe) {
    if (!newframe) return;
    uint64_t now = mtl_ptp_read_time(ctx->ctx->handle);
    std::lock_guard<std::mutex> rec(mtx);
    arrivals.emplace_back(tmstamp, now);
  }
};
}  // namespace

/* ST22 has no EXACT flag: st22_tx_put_mbuf_tai() picks the epoch nearest to its TAI. */
TEST(St22_tx, rtp_user_pacing_nearest_epoch) {
  auto ctx = st_test_ctx();
  const St22SessionSpec spec{ST22_TYPE_RTP_LEVEL, ST_FPS_P59_94, 1920, 1080, 1280, 540};
  constexpr uint64_t kLeadEpochs = 18;    /* ~300ms */
  constexpr int64_t kEarlyNs = 1'000'000; /* RL warm-up and clock reads */
  constexpr int64_t kLateNs = 3'000'000;  /* tr_offset and RX thread wake */
  constexpr size_t kMinFrames = 60;
  /* Every 2nd epoch at 59.94 fps (~29.97 fps) after 18 lead epochs (~0.30 s):
   * 100 frames takes ~3.6 s, satisfies kMinFrames=60, and keeps the 1% miss budget. */
  constexpr size_t kTargetFrames = 100;

  MTL_TEST_REQUIRE_PORTS(ctx, 2);

  /* Declare pacing before sessions/workers so workers stop before pacing is destroyed. */
  St22RtpPacing pacing;
  std::vector<St22TxSession> tx;
  std::vector<St22RxSession> rx;
  mtl_test::ScopedMtlStart mtl_guard(ctx->handle);
  std::vector<mtl_test::ScopedWorker> workers;
  ASSERT_GE(mtl_guard.start(), 0);

  St22LoopbackConfig cfg;
  cfg.on_tx_ops = [](size_t, tests_context* tx_ctx, struct st22_tx_ops* ops) {
    tx_ctx->rtp_tmstamp = 1;
    ops->flags = ST22_TX_FLAG_USER_PACING | ST22_TX_FLAG_USER_TIMESTAMP;
  };
  create_st22_loopback(ctx, {spec}, {spec}, tx, rx, nullptr, cfg);
  if (::testing::Test::HasFatalFailure()) return;

  pacing.frame_ns = (long double)NS_PER_S / st_frame_rate(spec.fps);
  pacing.base_epoch =
      (uint64_t)(mtl_ptp_read_time(ctx->handle) / pacing.frame_ns) + kLeadEpochs;
  workers.emplace_back(
      rx[0].ctx.get(),
      std::thread(st22_rx_get_packet, rx[0].ctx.get(),
                  [&pacing](tests_context* c, uint32_t tmstamp, bool newframe) {
                    pacing.record_first_packet(c, tmstamp, newframe);
                  }));
  workers.emplace_back(tx[0].ctx.get(), std::thread(st22_tx_feed_packet, tx[0].ctx.get(),
                                                    [&pacing](uint32_t rtp_ts) {
                                                      return pacing.target(rtp_ts);
                                                    }));

  EXPECT_TRUE(mtl_test::wait_until(
      [&pacing]() {
        std::lock_guard<std::mutex> lck(pacing.mtx);
        return pacing.arrivals.size() >= kTargetFrames;
      },
      /*timeout_ms=*/8000, /*poll_ms=*/10));
  for (auto& w : workers) w.stop();
  EXPECT_GE(mtl_guard.stop(), 0);

  std::vector<std::pair<uint32_t, uint64_t>> arrivals;
  {
    std::lock_guard<std::mutex> lck(pacing.mtx);
    arrivals = pacing.arrivals;
  }
  size_t misses = 0;
  for (const auto& a : arrivals) {
    uint64_t epoch_start = (uint64_t)(pacing.epoch(a.first) * pacing.frame_ns);
    int64_t delta = (int64_t)(a.second - epoch_start);
    if (delta < -kEarlyNs || delta > kLateNs) {
      misses++;
      info("%s, frame %u arrived %" PRId64 "ns from its nearest epoch\n", __func__,
           a.first, delta);
    }
  }
  info("%s, frames %zu misses %zu\n", __func__, arrivals.size(), misses);
  EXPECT_GT(mtl_test::relaxed_load(&rx[0].ctx->packet_rec), 0);
  EXPECT_EQ(mtl_test::relaxed_load(&rx[0].ctx->incomplete_frame_cnt), 0);
  EXPECT_GE(arrivals.size(), kMinFrames);
  EXPECT_LE(misses, arrivals.size() / 100);
}
