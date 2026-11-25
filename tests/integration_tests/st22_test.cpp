/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include <cinttypes>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "log.h"
#include "test_harness.hpp"

#define ST22_TEST_PAYLOAD_TYPE (114)
#define ST22_TEST_LOOPBACK_UDP_PORT (15000)

namespace {

using ScopedSt22Tx = mtl_test::ScopedHandle<st22_tx_handle, st22_tx_free>;
using ScopedSt22Rx = mtl_test::ScopedHandle<st22_rx_handle, st22_rx_free>;

inline int atomic_inc(int* p) {
  return __atomic_fetch_add(p, 1, __ATOMIC_RELAXED);
}

inline void set_handle(tests_context* c, void* h) {
  __atomic_store_n(&c->handle, h, __ATOMIC_RELEASE);
}

inline void* get_handle(const tests_context* c) {
  return __atomic_load_n(&c->handle, __ATOMIC_ACQUIRE);
}

/* RAII guard declared AFTER ScopedMtlStart so worker threads are always stopped
 * and joined BEFORE mtl_stop() even on early ASSERT_* or exception unwind. */
class ScopedStop {
 public:
  explicit ScopedStop(std::function<void()> fn) : fn_(std::move(fn)) {
  }
  ~ScopedStop() {
    stop();
  }
  void stop() {
    if (done_) return;
    done_ = true;
    if (fn_) fn_();
  }
  ScopedStop(const ScopedStop&) = delete;
  ScopedStop& operator=(const ScopedStop&) = delete;

 private:
  std::function<void()> fn_;
  bool done_ = false;
};

/*
 * Steady-state rate window parameters for ST 2110-22 (25-59.94 fps):
 *   - warmup_frames = 3 absorbs TX/RX startup and RL pacing training.
 *   - min_window_ms = 1000, min_delta_frames = 25 collects 50-60 frames at
 *     50-59.94 fps (<= 2.0% edge quantization error) and ~30 frames at 29.97 fps
 *     (<= 3.3% error), well inside the +/-10% FPS tolerance (~9x faster than
 *     sleep(10)).
 */
constexpr int kSt22RateWarmupFrames = 3;
constexpr int kSt22RateMinDeltaFrames = 25;
constexpr int kSt22RateMinWindowMs = 1000;

struct St22SessionSpec {
  enum st22_type type = ST22_TYPE_FRAME_LEVEL;
  enum st_fps fps = ST_FPS_P59_94;
  int width = 1920;
  int height = 1080;
  int pkt_data_len = 1280;
  int total_pkts = 540;
};

struct St22TxSession {
  mtl_test::ScopedContext ctx;
  ScopedSt22Tx handle;
  std::thread worker;

  St22TxSession() = default;
  St22TxSession(St22TxSession&&) noexcept = default;
  St22TxSession& operator=(St22TxSession&&) noexcept = default;
  St22TxSession(const St22TxSession&) = delete;
  St22TxSession& operator=(const St22TxSession&) = delete;

  void stop() {
    mtl_test::stop_worker(ctx.get(), worker);
  }

  ~St22TxSession() {
    stop();
    handle.reset();
  }
};

struct St22RxSession {
  mtl_test::ScopedContext ctx;
  ScopedSt22Rx handle;
  std::thread worker;

  St22RxSession() = default;
  St22RxSession(St22RxSession&&) noexcept = default;
  St22RxSession& operator=(St22RxSession&&) noexcept = default;
  St22RxSession(const St22RxSession&) = delete;
  St22RxSession& operator=(const St22RxSession&) = delete;

  void stop() {
    mtl_test::stop_worker(ctx.get(), worker);
  }

  ~St22RxSession() {
    stop();
    handle.reset();
  }
};

template <typename SessionVec>
std::vector<tests_context*> collect_ctxs(const SessionVec& sessions) {
  std::vector<tests_context*> ptrs;
  ptrs.reserve(sessions.size());
  for (const auto& s : sessions) ptrs.push_back(s.ctx.get());
  return ptrs;
}

}  // namespace

static int st22_tx_rtp_done(void* args) {
  auto ctx = (tests_context*)args;

  if (!get_handle(ctx)) return -EIO; /* not ready */

  std::unique_lock<std::mutex> lck(ctx->mtx);
  ctx->cv.notify_all();
  if (!ctx->start_time) ctx->start_time = st_test_get_monotonic_time();
  return 0;
}

static int st22_rx_rtp_ready(void* args) {
  auto ctx = (tests_context*)args;

  if (!get_handle(ctx)) return -EIO; /* not ready */

  std::unique_lock<std::mutex> lck(ctx->mtx);
  ctx->cv.notify_all();
  if (!ctx->start_time) ctx->start_time = st_test_get_monotonic_time();
  return 0;
}

static int st22_next_video_frame(void* priv, uint16_t* next_frame_idx,
                                 struct st22_tx_frame_meta* meta) {
  auto ctx = (tests_context*)priv;

  if (!get_handle(ctx)) return -EIO; /* not ready */

  *next_frame_idx = ctx->fb_idx;
  meta->codestream_size = ctx->frame_size;
  dbg("%s, next_frame_idx %d frame_size %" PRIu64 "\n", __func__, *next_frame_idx,
      meta->codestream_size);
  ctx->fb_idx++;
  if (ctx->fb_idx >= ctx->fb_cnt) ctx->fb_idx = 0;
  atomic_inc(&ctx->fb_send);
  if (!ctx->start_time) ctx->start_time = st_test_get_monotonic_time();
  return 0;
}

static int st22_next_video_frame_timestamp(void* priv, uint16_t* next_frame_idx,
                                           struct st22_tx_frame_meta* meta) {
  auto ctx = (tests_context*)priv;

  if (!get_handle(ctx)) return -EIO; /* not ready */

  *next_frame_idx = ctx->fb_idx;
  meta->codestream_size = ctx->frame_size;
  meta->tfmt = ST10_TIMESTAMP_FMT_TAI;
  meta->timestamp = mtl_ptp_read_time(ctx->ctx->handle) + 35 * 1000 * 1000;
  dbg("%s, next_frame_idx %d frame_size %" PRIu64 "\n", __func__, *next_frame_idx,
      meta->codestream_size);
  ctx->fb_idx++;
  if (ctx->fb_idx >= ctx->fb_cnt) ctx->fb_idx = 0;
  atomic_inc(&ctx->fb_send);
  if (!ctx->start_time) ctx->start_time = st_test_get_monotonic_time();
  return 0;
}

static int st22_frame_done(void* /*priv*/, uint16_t /*frame_idx*/,
                           struct st22_tx_frame_meta* /*meta*/) {
  return 0;
}

static int st22_rx_frame_ready(void* priv, void* frame, struct st22_rx_frame_meta* meta) {
  auto ctx = (tests_context*)priv;
  void* h = get_handle(ctx);
  if (!h) return -EIO;

  atomic_inc(&ctx->fb_rec);
  if (!ctx->start_time) {
    ctx->rtp_delta = meta->timestamp - ctx->rtp_tmstamp;
    ctx->start_time = st_test_get_monotonic_time();
  }

  if (meta->tfmt == ST10_TIMESTAMP_FMT_MEDIA_CLK) ctx->rtp_tmstamp = meta->timestamp;
  st22_rx_put_framebuff((st22_rx_handle)h, frame);
  return 0;
}

static void st22_tx_ops_init(tests_context* st22, struct st22_tx_ops* ops) {
  auto ctx = st22->ctx;

  memset(ops, 0, sizeof(*ops));
  ops->name = "st22_test";
  ops->priv = st22;
  ops->num_port = ctx->para.num_ports;
  if (ctx->same_dual_port) ops->num_port = 1;
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
  ops->notify_rtp_done = st22_tx_rtp_done;
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
  ops->num_port = ctx->para.num_ports;
  if (ctx->same_dual_port) ops->num_port = 1;
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
  ops->notify_rtp_ready = st22_rx_rtp_ready;
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

  memset(ops, 0, sizeof(*ops));
  ops->name = "st22_test";
  ops->priv = s_ctx;
  ops->num_port = 1;
  if (ctx->mcast_only) {
    memcpy(ops->dip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
           MTL_IP_ADDR_LEN);
  } else {
    memcpy(ops->dip_addr[MTL_SESSION_PORT_P], ctx->para.sip_addr[MTL_PORT_R],
           MTL_IP_ADDR_LEN);
  }
  snprintf(ops->port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
           ctx->para.port[MTL_PORT_P]);
  ops->udp_port[MTL_SESSION_PORT_P] = udp_port;
  ops->pacing = ST21_PACING_NARROW;
  ops->width = spec.width;
  ops->height = spec.height;
  ops->fps = spec.fps;
  ops->payload_type = ST22_TEST_PAYLOAD_TYPE;
  ops->type = spec.type;
  ops->pack_type = ST22_PACK_CODESTREAM;
  ops->framebuff_cnt = s_ctx->fb_cnt;
  /* Set max framebuffer size with 100 packets of headroom. */
  ops->framebuff_max_size =
      s_ctx->frame_size + static_cast<size_t>(spec.pkt_data_len) * 100;
  ops->notify_rtp_done = st22_tx_rtp_done;
  ops->rtp_ring_size = 1024;
  ops->rtp_pkt_size = spec.pkt_data_len + sizeof(struct st22_rfc9134_rtp_hdr);
  ops->rtp_frame_total_pkts = spec.total_pkts;
  ops->notify_frame_done = st22_frame_done;
  ops->get_next_frame = st22_next_video_frame;
}

static void fill_loopback_rx_ops(struct st22_rx_ops* ops, tests_context* s_ctx,
                                 const St22SessionSpec& spec, uint16_t udp_port) {
  auto ctx = s_ctx->ctx;
  s_ctx->pkt_data_len = spec.pkt_data_len;
  s_ctx->total_pkts_in_frame = spec.total_pkts;
  s_ctx->frame_size = static_cast<size_t>(spec.pkt_data_len) * spec.total_pkts;

  memset(ops, 0, sizeof(*ops));
  ops->name = "st22_test";
  ops->priv = s_ctx;
  ops->num_port = 1;
  if (ctx->mcast_only) {
    memcpy(ops->ip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
           MTL_IP_ADDR_LEN);
  } else {
    memcpy(ops->ip_addr[MTL_SESSION_PORT_P], ctx->para.sip_addr[MTL_PORT_P],
           MTL_IP_ADDR_LEN);
  }
  snprintf(ops->port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
           ctx->para.port[MTL_PORT_R]);
  ops->udp_port[MTL_SESSION_PORT_P] = udp_port;
  ops->pacing = ST21_PACING_NARROW;
  ops->width = spec.width;
  ops->height = spec.height;
  ops->fps = spec.fps;
  ops->payload_type = ST22_TEST_PAYLOAD_TYPE;
  ops->type = spec.type;
  ops->pack_type = ST22_PACK_CODESTREAM;
  ops->framebuff_cnt = s_ctx->fb_cnt;
  /* Set max framebuffer size with 100 packets of headroom. */
  ops->framebuff_max_size =
      s_ctx->frame_size + static_cast<size_t>(spec.pkt_data_len) * 100;
  ops->notify_rtp_ready = st22_rx_rtp_ready;
  ops->rtp_ring_size = 1024;
  ops->notify_frame_ready = st22_rx_frame_ready;
}

static void st22_tx_assert_cnt(int expect_s22_tx_cnt) {
  auto ctx = st_test_ctx();
  auto handle = ctx->handle;
  struct st_var_info var;
  int ret;

  ret = st_get_var_info(handle, &var);
  EXPECT_GE(ret, 0);
  EXPECT_EQ(var.st22_tx_sessions_cnt, expect_s22_tx_cnt);
}

static void st22_rx_assert_cnt(int expect_s22_rx_cnt) {
  auto ctx = st_test_ctx();
  auto handle = ctx->handle;
  struct st_var_info var;
  int ret;

  ret = st_get_var_info(handle, &var);
  EXPECT_GE(ret, 0);
  EXPECT_EQ(var.st22_rx_sessions_cnt, expect_s22_rx_cnt);
}

TEST(St22_tx, create_free_single) {
  create_free_test(st22_tx, 0, 1, 1);
}
TEST(St22_tx, create_free_multi) {
  create_free_test(st22_tx, 0, 1, 6);
}
TEST(St22_tx, create_free_mix) {
  create_free_test(st22_tx, 2, 3, 4);
}
TEST(St22_tx, create_free_max) {
  create_free_max(st22_tx, TEST_CREATE_FREE_MAX);
}
TEST(St22_tx, create_expect_fail) {
  expect_fail_test(st22_tx);
}
TEST(St22_tx, create_expect_fail_ring_sz) {
  uint16_t ring_size = 0;
  expect_fail_test_rtp_ring(st22_tx, ST22_TYPE_RTP_LEVEL, ring_size);
  ring_size = 128 + 1;
  expect_fail_test_rtp_ring(st22_tx, ST22_TYPE_RTP_LEVEL, ring_size);
}
/* RTP-level create takes rtp_pkt_size in [1:1452], so a 1396-byte ST 2022-6 pkt fits */
TEST(St22_tx, rtp_pkt_size) {
  uint16_t rtp_pkt_size = 0;
  expect_test_rtp_pkt_size(st22_tx, ST22_TYPE_RTP_LEVEL, rtp_pkt_size, false);
  /* the video TX limit: the 1494-byte TX frame minus the 42-byte Ethernet/IPv4/UDP hdr */
  rtp_pkt_size = 1452;
  expect_test_rtp_pkt_size(st22_tx, ST22_TYPE_RTP_LEVEL, rtp_pkt_size, true);
  rtp_pkt_size = 1452 + 1;
  expect_test_rtp_pkt_size(st22_tx, ST22_TYPE_RTP_LEVEL, rtp_pkt_size, false);
}
TEST(St22_tx, create_expect_fail_fb_cnt) {
  uint16_t fbcnt = 1;
  expect_fail_test_fb_cnt(st22_tx, fbcnt);
  fbcnt = ST22_FB_MAX_COUNT + 1;
  expect_fail_test_fb_cnt(st22_tx, fbcnt);
}

TEST(St22_rx, create_free_single) {
  create_free_test(st22_rx, 0, 1, 1);
}
TEST(St22_rx, create_free_multi) {
  create_free_test(st22_rx, 0, 1, 6);
}
TEST(St22_rx, create_free_mix) {
  create_free_test(st22_rx, 2, 3, 4);
}
TEST(St22_rx, create_free_max) {
  create_free_max(st22_rx, TEST_CREATE_FREE_MAX);
}
TEST(St22_rx, create_expect_fail) {
  expect_fail_test(st22_rx);
}
TEST(St22_rx, create_expect_fail_ring_sz) {
  uint16_t ring_size = 0;
  expect_fail_test_rtp_ring(st22_rx, ST22_TYPE_RTP_LEVEL, ring_size);
  ring_size = 128 + 1;
  expect_fail_test_rtp_ring(st22_rx, ST22_TYPE_RTP_LEVEL, ring_size);
}
TEST(St22_rx, create_expect_fail_fb_cnt) {
  uint16_t fbcnt = 1;
  expect_fail_test_fb_cnt(st22_rx, fbcnt);
  fbcnt = ST22_FB_MAX_COUNT + 1;
  expect_fail_test_fb_cnt(st22_rx, fbcnt);
}

static int st22_tx_build_rtp_packet(tests_context* s, struct st22_rfc9134_rtp_hdr* rtp,
                                    uint16_t* pkt_len) {
  /* update hdr */
  rtp->base.csrc_count = 0;
  rtp->base.extension = 0;
  rtp->base.padding = 0;
  rtp->base.version = 2;
  rtp->base.marker = 0;
  rtp->base.payload_type = ST22_TEST_PAYLOAD_TYPE;
  rtp->base.tmstamp = htonl(s->rtp_tmstamp);
  int seq = __atomic_fetch_add(&s->seq_id, 1, __ATOMIC_RELAXED);
  rtp->base.seq_number = htons(seq);

  uint16_t data_len = s->pkt_data_len;
  *pkt_len = data_len + sizeof(*rtp);
  if (s->check_sha) {
    uint8_t* payload = (uint8_t*)rtp + sizeof(*rtp);
    mtl_memcpy(payload,
               s->frame_buf[s->fb_idx % ST22_TEST_SHA_HIST_NUM] + s->pkt_idx * data_len,
               data_len);
  }

  s->pkt_idx++;
  if (s->pkt_idx >= s->total_pkts_in_frame) {
    /* end of current frame */
    rtp->base.marker = 1;
    s->fb_idx++;
    s->pkt_idx = 0;
    s->rtp_tmstamp++;
    atomic_inc(&s->fb_send);
  }

  return 0;
}

/* tai_of, if set, gives the TAI of the frame a first packet starts, by RTP timestamp */
static void st22_tx_feed_packet_tai(tests_context* ctx, uint64_t (*tai_of)(uint32_t)) {
  auto handle = (st22_tx_handle)get_handle(ctx);
  void* mbuf;
  void* usrptr = NULL;
  uint16_t mbuf_len = 0;
  std::unique_lock<std::mutex> lck(ctx->mtx, std::defer_lock);
  while (!ctx->stop) {
    mbuf = st22_tx_get_mbuf(handle, &usrptr);
    if (!mbuf) {
      lck.lock();
      mbuf = st22_tx_get_mbuf(handle, &usrptr);
      if (mbuf) {
        lck.unlock();
      } else {
        if (!ctx->stop) ctx->cv.wait(lck);
        lck.unlock();
        continue;
      }
    }

    uint64_t tai = (tai_of && !ctx->pkt_idx) ? tai_of(ctx->rtp_tmstamp) : 0;
    st22_tx_build_rtp_packet(ctx, (struct st22_rfc9134_rtp_hdr*)usrptr, &mbuf_len);
    st22_tx_put_mbuf_tai(handle, mbuf, mbuf_len, tai);
  }
}

static void st22_tx_feed_packet(void* args) {
  st22_tx_feed_packet_tai((tests_context*)args, nullptr);
}

static void st22_rx_handle_rtp(tests_context* s, struct st22_rfc9134_rtp_hdr* hdr,
                               bool newframe, int mbuf_len) {
  uint8_t* frame;
  uint8_t* payload;

  if (newframe) {
    if (s->frame_buf[0]) {
      std::unique_lock<std::mutex> lck(s->mtx);
      s->buf_q.push(s->frame_buf[0]);
      s->cv.notify_all();
    }
    s->frame_buf[0] = (uint8_t*)st_test_zmalloc(s->frame_size);
    ASSERT_TRUE(s->frame_buf[0] != NULL);
  }

  frame = s->frame_buf[0];
  payload = (uint8_t*)hdr + sizeof(*hdr);
  int index = ntohs(hdr->base.seq_number) - __atomic_load_n(&s->seq_id, __ATOMIC_RELAXED);
  if (index < 0) {
    index = index + 0x10000;
  }
  mtl_memcpy(frame + index * (mbuf_len - sizeof(*hdr)), payload, mbuf_len - sizeof(*hdr));
}

/* on_packet, if set, sees every packet after the new frame check */
static void st22_rx_get_packet_hook(tests_context* ctx,
                                    void (*on_packet)(tests_context*, uint32_t, bool)) {
  auto handle = (st22_rx_handle)get_handle(ctx);
  void* mbuf;
  void* usrptr = NULL;
  uint16_t mbuf_len = 0;
  std::unique_lock<std::mutex> lck(ctx->mtx, std::defer_lock);
  struct st22_rfc9134_rtp_hdr* hdr;
  while (!ctx->stop) {
    mbuf = st22_rx_get_mbuf(handle, &usrptr, &mbuf_len);
    if (!mbuf) {
      lck.lock();
      mbuf = st22_rx_get_mbuf(handle, &usrptr, &mbuf_len);
      if (mbuf) {
        lck.unlock();
      } else {
        if (!ctx->stop) ctx->cv.wait(lck);
        lck.unlock();
        continue;
      }
    }
    hdr = (struct st22_rfc9134_rtp_hdr*)usrptr;
    bool newframe = false;
    uint32_t tmstamp = ntohl(hdr->base.tmstamp);
    if (tmstamp != ctx->rtp_tmstamp) {
      /* new frame received */
      ctx->rtp_tmstamp = tmstamp;
      atomic_inc(&ctx->fb_rec);
      newframe = true;
      __atomic_store_n(&ctx->seq_id, ntohs(hdr->base.seq_number), __ATOMIC_RELAXED);
    }
    if (ctx->check_sha) {
      st22_rx_handle_rtp(ctx, hdr, newframe, mbuf_len);
    }
    if (on_packet) on_packet(ctx, tmstamp, newframe);
    st22_rx_put_mbuf(handle, mbuf);
  }
}

static void st22_rx_get_packet(void* args) {
  st22_rx_get_packet_hook((tests_context*)args, nullptr);
}

static void st22_rx_fps_test(const std::vector<St22SessionSpec>& specs,
                             enum st_test_level level) {
  auto ctx = st_test_ctx();
  auto m_handle = ctx->handle;
  MTL_TEST_REQUIRE_PORTS(ctx, 2);
  MTL_TEST_REQUIRE_LEVEL(ctx, level);

  const size_t sessions = specs.size();
  std::vector<St22TxSession> tx;
  std::vector<St22RxSession> rx;
  std::vector<double> expect_framerate(sessions);
  tx.reserve(sessions);
  rx.reserve(sessions);

  for (size_t i = 0; i < sessions; i++) {
    const auto& spec = specs[i];
    const uint16_t udp_port = ST22_TEST_LOOPBACK_UDP_PORT + static_cast<uint16_t>(i * 2);
    expect_framerate[i] = st_frame_rate(spec.fps);

    tx.emplace_back();
    auto& s = tx.back();
    s.ctx = mtl_test::make_context(ctx, static_cast<int>(i), 3);

    struct st22_tx_ops ops_tx;
    fill_loopback_tx_ops(&ops_tx, s.ctx.get(), spec, udp_port);
    s.handle.reset(st22_tx_create(m_handle, &ops_tx));
    ASSERT_TRUE(static_cast<bool>(s.handle));
    set_handle(s.ctx.get(), s.handle.get());
    if (spec.type == ST22_TYPE_RTP_LEVEL) {
      s.worker = std::thread(st22_tx_feed_packet, s.ctx.get());
    }
  }

  for (size_t i = 0; i < sessions; i++) {
    const auto& spec = specs[i];
    const uint16_t udp_port = ST22_TEST_LOOPBACK_UDP_PORT + static_cast<uint16_t>(i * 2);

    rx.emplace_back();
    auto& s = rx.back();
    s.ctx = mtl_test::make_context(ctx, static_cast<int>(i), 3);

    struct st22_rx_ops ops_rx;
    fill_loopback_rx_ops(&ops_rx, s.ctx.get(), spec, udp_port);
    s.handle.reset(st22_rx_create(m_handle, &ops_rx));
    ASSERT_TRUE(static_cast<bool>(s.handle));
    set_handle(s.ctx.get(), s.handle.get());
    if (spec.type == ST22_TYPE_RTP_LEVEL) {
      s.worker = std::thread(st22_rx_get_packet, s.ctx.get());
    }
  }

  mtl_test::ScopedMtlStart mtl_guard(m_handle);
  ScopedStop workers_stop([&]() {
    for (auto& s : tx) s.stop();
    for (auto& s : rx) s.stop();
  });
  EXPECT_GE(mtl_guard.start(), 0);

  auto rx_ctxs = collect_ctxs(rx);
  auto rate_res =
      mtl_test::measure_rate_window(rx_ctxs, mtl_test::rx_frames, kSt22RateWarmupFrames,
                                    kSt22RateMinDeltaFrames, kSt22RateMinWindowMs);
  EXPECT_TRUE(rate_res.warmed_up);
  EXPECT_TRUE(rate_res.complete);

  workers_stop.stop();
  EXPECT_GE(mtl_guard.stop(), 0);

  for (size_t i = 0; i < sessions; i++) {
    const int fb_rec = mtl_test::rx_frames(rx[i].ctx.get());
    EXPECT_GT(fb_rec, 0);
    info("%s, session %zu fb_rec %d framerate %f\n", __func__, i, fb_rec,
         rate_res.rate[i]);
    EXPECT_NEAR(rate_res.rate[i], expect_framerate[i], expect_framerate[i] * 0.1);
  }
}

TEST(St22_rx, fps_1080p_s2) {
  st22_rx_fps_test(
      {
          {ST22_TYPE_FRAME_LEVEL, ST_FPS_P59_94, 1920, 1080, 1280, 546},
          {ST22_TYPE_RTP_LEVEL, ST_FPS_P50, 1920, 1080, 1280, 540},
      },
      ST_TEST_LEVEL_MANDATORY);
}

TEST(St22_rx, fps_mix_s2) {
  st22_rx_fps_test(
      {
          {ST22_TYPE_RTP_LEVEL, ST_FPS_P59_94, 1920, 1080, 1280, 540},
          {ST22_TYPE_FRAME_LEVEL, ST_FPS_P50, 1280, 720, 1300, 150},
      },
      ST_TEST_LEVEL_ALL);
}

static void st22_rx_update_src_test(int tx_sessions, enum st_test_level level) {
  auto ctx = st_test_ctx();
  auto m_handle = ctx->handle;
  MTL_TEST_REQUIRE_PORTS(ctx, 2);
  MTL_TEST_REQUIRE_LEVEL(ctx, level);

  const int rx_sessions = 1;
  const St22SessionSpec spec{ST22_TYPE_FRAME_LEVEL, ST_FPS_P59_94, 1920, 1080, 1280, 520};
  const double expect_framerate = st_frame_rate(spec.fps);

  std::vector<St22TxSession> tx;
  std::vector<St22RxSession> rx;
  tx.reserve(tx_sessions);
  rx.reserve(rx_sessions);

  for (int i = 0; i < tx_sessions; i++) {
    tx.emplace_back();
    auto& s = tx.back();
    s.ctx = mtl_test::make_context(ctx, i, 3);

    struct st22_tx_ops ops_tx;
    fill_loopback_tx_ops(&ops_tx, s.ctx.get(), spec,
                         ST22_TEST_LOOPBACK_UDP_PORT + static_cast<uint16_t>(i * 2));
    if (2 == i) {
      memcpy(ops_tx.dip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_R],
             MTL_IP_ADDR_LEN);
    } else if (1 == i) {
      memcpy(ops_tx.dip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
             MTL_IP_ADDR_LEN);
    } else if (ctx->mcast_only) {
      memcpy(ops_tx.dip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_2],
             MTL_IP_ADDR_LEN);
    } else {
      memcpy(ops_tx.dip_addr[MTL_SESSION_PORT_P], ctx->para.sip_addr[MTL_PORT_R],
             MTL_IP_ADDR_LEN);
    }

    s.handle.reset(st22_tx_create(m_handle, &ops_tx));
    ASSERT_TRUE(static_cast<bool>(s.handle));
    set_handle(s.ctx.get(), s.handle.get());
  }

  for (int i = 0; i < rx_sessions; i++) {
    rx.emplace_back();
    auto& s = rx.back();
    s.ctx = mtl_test::make_context(ctx, i, 3);

    struct st22_rx_ops ops_rx;
    fill_loopback_rx_ops(&ops_rx, s.ctx.get(), spec,
                         ST22_TEST_LOOPBACK_UDP_PORT + static_cast<uint16_t>(i * 2));
    if (ctx->mcast_only) {
      memcpy(ops_rx.ip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_2],
             MTL_IP_ADDR_LEN);
    }

    s.handle.reset(st22_rx_create(m_handle, &ops_rx));
    ASSERT_TRUE(static_cast<bool>(s.handle));
    set_handle(s.ctx.get(), s.handle.get());
  }

  mtl_test::ScopedMtlStart mtl_guard(m_handle);
  EXPECT_GE(mtl_guard.start(), 0);
  auto rx_ctxs = collect_ctxs(rx);

  /* Wait for initial stream establishment (~50 ms at 59.94 fps). */
  EXPECT_TRUE(
      mtl_test::wait_for_all(rx_ctxs, mtl_test::rx_frames, kSt22RateWarmupFrames, 6000));

  auto verify_switched_source = [&](const char* label, const uint8_t* src_ip,
                                    uint16_t udp_port) {
    struct st_rx_source_info src;
    memset(&src, 0, sizeof(src));
    src.udp_port[MTL_SESSION_PORT_P] = udp_port;
    memcpy(src.ip_addr[MTL_SESSION_PORT_P], src_ip, MTL_IP_ADDR_LEN);

    std::vector<int> base_rec(rx.size(), 0);
    for (size_t i = 0; i < rx.size(); i++) {
      EXPECT_GE(st22_rx_update_source(rx[i].handle.get(), &src), 0);
      base_rec[i] = mtl_test::rx_frames(rx[i].ctx.get());
    }

    auto delta_rec = [&](const tests_context* c) {
      return mtl_test::rx_frames(c) - base_rec[static_cast<size_t>(c->idx)];
    };
    auto rate_res =
        mtl_test::measure_rate_window(rx_ctxs, delta_rec, kSt22RateWarmupFrames,
                                      kSt22RateMinDeltaFrames, kSt22RateMinWindowMs);
    EXPECT_TRUE(rate_res.warmed_up);
    EXPECT_TRUE(rate_res.complete);
    for (size_t i = 0; i < rx.size(); i++) {
      const int fb_rec = mtl_test::rx_frames(rx[i].ctx.get()) - base_rec[i];
      EXPECT_GT(fb_rec, 0);
      info("%s, session %zu fb_rec %d framerate %f for %s\n", __func__, i, fb_rec,
           rate_res.rate[i], label);
      EXPECT_NEAR(rate_res.rate[i], expect_framerate, expect_framerate * 0.1);
    }
  };

  /* Switch to mcast port P (tx_session:1). */
  verify_switched_source("mcast 1", ctx->mcast_ip_addr[MTL_PORT_P],
                         ST22_TEST_LOOPBACK_UDP_PORT + 2);

  /* Switch to mcast port R (tx_session:2). */
  if (tx_sessions > 2) {
    verify_switched_source("mcast 2", ctx->mcast_ip_addr[MTL_PORT_R],
                           ST22_TEST_LOOPBACK_UDP_PORT + 4);
  }

  /* Switch back to unicast (tx_session:0). */
  const uint8_t* tx0_ip =
      ctx->mcast_only ? ctx->mcast_ip_addr[MTL_PORT_2] : ctx->para.sip_addr[MTL_PORT_P];
  verify_switched_source("unicast 0", tx0_ip, ST22_TEST_LOOPBACK_UDP_PORT);

  EXPECT_GE(mtl_guard.stop(), 0);
}

TEST(St22_rx, update_source) {
  st22_rx_update_src_test(2, ST_TEST_LEVEL_ALL);
}

static void st22_rx_after_start_test(const std::vector<St22SessionSpec>& specs,
                                     int repeat) {
  auto ctx = st_test_ctx();
  auto m_handle = ctx->handle;
  MTL_TEST_REQUIRE_PORTS(ctx, 2);

  const size_t sessions = specs.size();

  mtl_test::ScopedMtlStart mtl_guard(m_handle);
  EXPECT_GE(mtl_guard.start(), 0);

  for (int r = 0; r < repeat; r++) {
    std::vector<St22TxSession> tx;
    std::vector<St22RxSession> rx;
    std::vector<double> expect_framerate(sessions);
    tx.reserve(sessions);
    rx.reserve(sessions);

    for (size_t i = 0; i < sessions; i++) {
      const auto& spec = specs[i];
      const uint16_t udp_port =
          ST22_TEST_LOOPBACK_UDP_PORT + static_cast<uint16_t>(i * 2);
      expect_framerate[i] = st_frame_rate(spec.fps);

      tx.emplace_back();
      auto& s = tx.back();
      s.ctx = mtl_test::make_context(ctx, static_cast<int>(i), 3);

      struct st22_tx_ops ops_tx;
      fill_loopback_tx_ops(&ops_tx, s.ctx.get(), spec, udp_port);
      s.handle.reset(st22_tx_create(m_handle, &ops_tx));
      ASSERT_TRUE(static_cast<bool>(s.handle));
      set_handle(s.ctx.get(), s.handle.get());
    }

    for (size_t i = 0; i < sessions; i++) {
      const auto& spec = specs[i];
      const uint16_t udp_port =
          ST22_TEST_LOOPBACK_UDP_PORT + static_cast<uint16_t>(i * 2);

      rx.emplace_back();
      auto& s = rx.back();
      s.ctx = mtl_test::make_context(ctx, static_cast<int>(i), 3);

      struct st22_rx_ops ops_rx;
      fill_loopback_rx_ops(&ops_rx, s.ctx.get(), spec, udp_port);
      s.handle.reset(st22_rx_create(m_handle, &ops_rx));
      ASSERT_TRUE(static_cast<bool>(s.handle));
      set_handle(s.ctx.get(), s.handle.get());
    }

    auto rx_ctxs = collect_ctxs(rx);
    auto rate_res =
        mtl_test::measure_rate_window(rx_ctxs, mtl_test::rx_frames, kSt22RateWarmupFrames,
                                      kSt22RateMinDeltaFrames, kSt22RateMinWindowMs);
    EXPECT_TRUE(rate_res.warmed_up);
    EXPECT_TRUE(rate_res.complete);

    for (size_t i = 0; i < sessions; i++) {
      const int fb_rec = mtl_test::rx_frames(rx[i].ctx.get());
      EXPECT_GT(fb_rec, 0);
      info("%s, session %zu fb_rec %d framerate %f\n", __func__, i, fb_rec,
           rate_res.rate[i]);
      EXPECT_NEAR(rate_res.rate[i], expect_framerate[i], expect_framerate[i] * 0.1);
      EXPECT_LT(mtl_test::relaxed_load(&rx[i].ctx->sha_fail_cnt),
                2); /* the first frame may be incomplete */
    }
    /* rx then tx destroyed here in reverse order before the next repeat */
  }

  EXPECT_GE(mtl_guard.stop(), 0);
}

TEST(St22_rx, after_start_s2) {
  st22_rx_after_start_test(
      {
          {ST22_TYPE_FRAME_LEVEL, ST_FPS_P59_94, 1920, 1080, 1280, 540},
          {ST22_TYPE_FRAME_LEVEL, ST_FPS_P50, 1280, 720, 1300, 150},
      },
      2);
}

static void st22_rx_dump_test(const std::vector<St22SessionSpec>& specs) {
  auto ctx = st_test_ctx();
  auto m_handle = ctx->handle;
  MTL_TEST_REQUIRE_PORTS(ctx, 2);

  if (!mtl_pmd_is_dpdk_based(m_handle, MTL_PORT_R)) {
    GTEST_SKIP() << "MTL_PORT_R is not a DPDK-based PMD";
  }

  const size_t sessions = specs.size();
  std::vector<St22TxSession> tx;
  std::vector<St22RxSession> rx;
  tx.reserve(sessions);
  rx.reserve(sessions);

  for (size_t i = 0; i < sessions; i++) {
    const auto& spec = specs[i];
    const uint16_t udp_port = ST22_TEST_LOOPBACK_UDP_PORT + static_cast<uint16_t>(i * 2);

    tx.emplace_back();
    auto& s = tx.back();
    s.ctx = mtl_test::make_context(ctx, static_cast<int>(i), 3);

    struct st22_tx_ops ops_tx;
    fill_loopback_tx_ops(&ops_tx, s.ctx.get(), spec, udp_port);
    s.handle.reset(st22_tx_create(m_handle, &ops_tx));
    ASSERT_TRUE(static_cast<bool>(s.handle));
    set_handle(s.ctx.get(), s.handle.get());
  }

  for (size_t i = 0; i < sessions; i++) {
    const auto& spec = specs[i];
    const uint16_t udp_port = ST22_TEST_LOOPBACK_UDP_PORT + static_cast<uint16_t>(i * 2);

    rx.emplace_back();
    auto& s = rx.back();
    s.ctx = mtl_test::make_context(ctx, static_cast<int>(i), 3);

    struct st22_rx_ops ops_rx;
    fill_loopback_rx_ops(&ops_rx, s.ctx.get(), spec, udp_port);
    s.handle.reset(st22_rx_create(m_handle, &ops_rx));
    ASSERT_TRUE(static_cast<bool>(s.handle));
    set_handle(s.ctx.get(), s.handle.get());
  }

  mtl_test::ScopedMtlStart mtl_guard(m_handle);
  EXPECT_GE(mtl_guard.start(), 0);

  /* Wait for datapath bring-up (3 frames = ~1620 packets at 59.94 fps, ~50 ms). */
  auto rx_ctxs = collect_ctxs(rx);
  EXPECT_TRUE(
      mtl_test::wait_for_all(rx_ctxs, mtl_test::rx_frames, kSt22RateWarmupFrames, 6000));

  const uint32_t max_dump_packets = 100;
  for (size_t i = 0; i < sessions; i++) {
    struct st_pcap_dump_meta meta;
    int ret = st22_rx_pcapng_dump(rx[i].handle.get(), max_dump_packets, true, &meta);
    EXPECT_GE(ret, 0);
    EXPECT_EQ(meta.dumped_packets[MTL_SESSION_PORT_P], max_dump_packets);
    dbg("%s, file_name %s\n", __func__, meta.file_name[MTL_SESSION_PORT_P]);
    if (ret >= 0) remove(meta.file_name[MTL_SESSION_PORT_P]);
  }

  EXPECT_GE(mtl_guard.stop(), 0);
}

TEST(St22_rx, pcap_dump) {
  st22_rx_dump_test({
      {ST22_TYPE_FRAME_LEVEL, ST_FPS_P59_94, 1280, 720, 1280, 540},
  });
}

static int st22_digest_rx_frame_ready(void* priv, void* frame,
                                      struct st22_rx_frame_meta* meta) {
  auto ctx = (tests_context*)priv;
  void* h = get_handle(ctx);
  if (!h) return -EIO;

  if (meta->frame_total_size != ctx->frame_size) {
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
  if (!ctx->start_time) ctx->start_time = st_test_get_monotonic_time();
  dbg("%s, frame %p\n", __func__, frame);
  return 0;
}

static void st22_digest_rx_frame_check(void* args) {
  auto ctx = (tests_context*)args;
  auto handle = (st22_rx_handle)get_handle(ctx);
  unsigned char result[SHA256_DIGEST_LENGTH];
  while (true) {
    void* frame = nullptr;
    {
      std::unique_lock<std::mutex> lck(ctx->mtx);
      ctx->cv.wait(lck, [&]() { return !ctx->buf_q.empty() || ctx->stop.load(); });
      if (ctx->stop && ctx->buf_q.empty()) break;
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
    dbg("%s, frame %p\n", __func__, frame);
    SHA256((unsigned char*)frame, ctx->frame_size, result);
    int i;
    for (i = 0; i < ST22_TEST_SHA_HIST_NUM; i++) {
      unsigned char* target_sha = ctx->shas[i];
      if (!memcmp(result, target_sha, SHA256_DIGEST_LENGTH)) break;
    }
    if (i >= ST22_TEST_SHA_HIST_NUM) {
      test_sha_dump("st22_rx_error_sha", result);
      atomic_inc(&ctx->sha_fail_cnt);
    }
    atomic_inc(&ctx->check_sha_frame_cnt);
    st22_rx_put_framebuff(handle, frame);
  }
}

/* Frame-level SHA256 digest and steady-state FPS verification. */
static void st22_rx_digest_test(const std::vector<St22SessionSpec>& specs,
                                enum st_test_level level, bool enable_rtcp = false) {
  auto ctx = st_test_ctx();
  auto m_handle = ctx->handle;
  MTL_TEST_REQUIRE_LEVEL(ctx, level);
  MTL_TEST_REQUIRE_PORTS(ctx, 2);

  const size_t sessions = specs.size();
  std::vector<St22TxSession> tx;
  std::vector<St22RxSession> rx;
  std::vector<double> expect_framerate(sessions);
  tx.reserve(sessions);
  rx.reserve(sessions);

  for (size_t i = 0; i < sessions; i++) {
    const auto& spec = specs[i];
    const uint16_t udp_port = ST22_TEST_LOOPBACK_UDP_PORT + static_cast<uint16_t>(i * 2);
    expect_framerate[i] = st_frame_rate(spec.fps);

    tx.emplace_back();
    auto& s = tx.back();
    s.ctx = mtl_test::make_context(ctx, static_cast<int>(i), ST22_TEST_SHA_HIST_NUM);

    struct st22_tx_ops ops_tx;
    fill_loopback_tx_ops(&ops_tx, s.ctx.get(), spec, udp_port);
    if (enable_rtcp) {
      ops_tx.flags |= ST22_TX_FLAG_ENABLE_RTCP;
      ops_tx.rtcp.buffer_size = 512;
    }

    s.handle.reset(st22_tx_create(m_handle, &ops_tx));
    ASSERT_TRUE(static_cast<bool>(s.handle));

    /* Populate TX framebuffers and record expected SHA256 digests. */
    size_t frame_size = s.ctx->frame_size;
    for (int frame = 0; frame < ST22_TEST_SHA_HIST_NUM; frame++) {
      uint8_t* fb = (uint8_t*)st22_tx_get_fb_addr(s.handle.get(), frame);
      ASSERT_TRUE(fb != NULL);
      st_test_rand_data(fb, frame_size, frame);
      unsigned char* result = s.ctx->shas[frame];
      SHA256((unsigned char*)fb, frame_size, result);
      test_sha_dump("st20_rx", result);
    }
    set_handle(s.ctx.get(), s.handle.get());
  }

  for (size_t i = 0; i < sessions; i++) {
    const auto& spec = specs[i];
    const uint16_t udp_port = ST22_TEST_LOOPBACK_UDP_PORT + static_cast<uint16_t>(i * 2);

    rx.emplace_back();
    auto& s = rx.back();
    s.ctx = mtl_test::make_context(ctx, static_cast<int>(i), ST22_TEST_SHA_HIST_NUM);

    struct st22_rx_ops ops_rx;
    fill_loopback_rx_ops(&ops_rx, s.ctx.get(), spec, udp_port);
    ops_rx.notify_frame_ready = st22_digest_rx_frame_ready;
    if (enable_rtcp) {
      ops_rx.flags |= ST22_RX_FLAG_ENABLE_RTCP | ST22_RX_FLAG_SIMULATE_PKT_LOSS;
      ops_rx.rtcp.nack_interval_us = 100;
      ops_rx.rtcp.seq_skip_window = 0;
      ops_rx.rtcp.burst_loss_max = 4;
      ops_rx.rtcp.sim_loss_rate = 0.0001;
    }

    s.handle.reset(st22_rx_create(m_handle, &ops_rx));
    ASSERT_TRUE(static_cast<bool>(s.handle));

    memcpy(s.ctx->shas, tx[i].ctx->shas, ST22_TEST_SHA_HIST_NUM * SHA256_DIGEST_LENGTH);
    set_handle(s.ctx.get(), s.handle.get());
    s.worker = std::thread(st22_digest_rx_frame_check, s.ctx.get());

    struct st_queue_meta meta;
    EXPECT_GE(st22_rx_get_queue_meta(s.handle.get(), &meta), 0);
  }

  mtl_test::ScopedMtlStart mtl_guard(m_handle);
  ScopedStop workers_stop([&]() {
    for (auto& s : rx) s.stop();
  });
  EXPECT_GE(mtl_guard.start(), 0);

  auto rx_ctxs = collect_ctxs(rx);
  auto rate_res =
      mtl_test::measure_rate_window(rx_ctxs, mtl_test::rx_frames, kSt22RateWarmupFrames,
                                    kSt22RateMinDeltaFrames, kSt22RateMinWindowMs);
  EXPECT_TRUE(rate_res.warmed_up);
  EXPECT_TRUE(rate_res.complete);
  /* Ensure the SHA worker verified at least one frame before stopping. */
  EXPECT_TRUE(mtl_test::wait_for_all(rx_ctxs, mtl_test::sha_checked_frames, 1, 2000));

  workers_stop.stop();
  EXPECT_GE(mtl_guard.stop(), 0);

  for (size_t i = 0; i < sessions; i++) {
    const int fb_rec = mtl_test::rx_frames(rx[i].ctx.get());
    const int sha_checked = mtl_test::sha_checked_frames(rx[i].ctx.get());
    EXPECT_GT(fb_rec, 0);
    EXPECT_GT(sha_checked, 0);
    info("%s, session %zu fb_rec %d framerate %f\n", __func__, i, fb_rec,
         rate_res.rate[i]);
    EXPECT_NEAR(rate_res.rate[i], expect_framerate[i], expect_framerate[i] * 0.1);
    EXPECT_EQ(mtl_test::relaxed_load(&rx[i].ctx->sha_fail_cnt), 0);
    EXPECT_EQ(mtl_test::relaxed_load(&rx[i].ctx->incomplete_frame_cnt), 0);
  }
}

TEST(St22_rx, digest_s2) {
  st22_rx_digest_test(
      {
          {ST22_TYPE_FRAME_LEVEL, ST_FPS_P59_94, 1920, 1080, 1280, 551},
          {ST22_TYPE_FRAME_LEVEL, ST_FPS_P50, 1920, 1080, 1280, 1520},
      },
      ST_TEST_LEVEL_MANDATORY);
}

TEST(St22_rx, digest_rtcp_s2) {
  st22_rx_digest_test(
      {
          {ST22_TYPE_FRAME_LEVEL, ST_FPS_P59_94, 1920, 1080, 1280, 551},
          {ST22_TYPE_FRAME_LEVEL, ST_FPS_P50, 1920, 1080, 1280, 1520},
      },
      ST_TEST_LEVEL_ALL, true);
}

static void st22_tx_user_pacing_test(const std::vector<St22SessionSpec>& specs,
                                     enum st_test_level level) {
  auto ctx = st_test_ctx();
  auto m_handle = ctx->handle;
  MTL_TEST_REQUIRE_LEVEL(ctx, level);
  MTL_TEST_REQUIRE_PORTS(ctx, 2);

  const size_t sessions = specs.size();
  std::vector<St22TxSession> tx;
  std::vector<St22RxSession> rx;
  std::vector<double> expect_framerate(sessions);
  tx.reserve(sessions);
  rx.reserve(sessions);

  for (size_t i = 0; i < sessions; i++) {
    const auto& spec = specs[i];
    const uint16_t udp_port = ST22_TEST_LOOPBACK_UDP_PORT + static_cast<uint16_t>(i * 2);
    /* +35 ms TAI timestamp skips every other 16.68 ms epoch -> half frame rate. */
    expect_framerate[i] = st_frame_rate(spec.fps) / 2;

    tx.emplace_back();
    auto& s = tx.back();
    s.ctx = mtl_test::make_context(ctx, static_cast<int>(i), 3);

    struct st22_tx_ops ops_tx;
    fill_loopback_tx_ops(&ops_tx, s.ctx.get(), spec, udp_port);
    ops_tx.flags = ST22_TX_FLAG_USER_PACING;
    ops_tx.get_next_frame = st22_next_video_frame_timestamp;

    s.handle.reset(st22_tx_create(m_handle, &ops_tx));
    ASSERT_TRUE(static_cast<bool>(s.handle));
    set_handle(s.ctx.get(), s.handle.get());
  }

  for (size_t i = 0; i < sessions; i++) {
    const auto& spec = specs[i];
    const uint16_t udp_port = ST22_TEST_LOOPBACK_UDP_PORT + static_cast<uint16_t>(i * 2);

    rx.emplace_back();
    auto& s = rx.back();
    s.ctx = mtl_test::make_context(ctx, static_cast<int>(i), 3);

    struct st22_rx_ops ops_rx;
    fill_loopback_rx_ops(&ops_rx, s.ctx.get(), spec, udp_port);
    s.handle.reset(st22_rx_create(m_handle, &ops_rx));
    ASSERT_TRUE(static_cast<bool>(s.handle));
    set_handle(s.ctx.get(), s.handle.get());
  }

  mtl_test::ScopedMtlStart mtl_guard(m_handle);
  EXPECT_GE(mtl_guard.start(), 0);

  /* Half of 59.94 fps (~29.97 fps): 1.0 s window collects ~30 frames (>= 25),
   * <= 3.3% quantization error, well inside +/-10% for both TX and RX rates. */
  auto tx_ctxs = collect_ctxs(tx);
  auto rx_ctxs = collect_ctxs(rx);
  auto win =
      mtl_test::measure_pipeline_window(tx_ctxs, rx_ctxs, kSt22RateWarmupFrames,
                                        kSt22RateMinDeltaFrames, kSt22RateMinWindowMs);
  EXPECT_TRUE(win.warmed_up);
  EXPECT_TRUE(win.complete);

  EXPECT_GE(mtl_guard.stop(), 0);

  for (size_t i = 0; i < sessions; i++) {
    const int fb_rec = mtl_test::rx_frames(rx[i].ctx.get());
    const int fb_send = mtl_test::tx_frames(tx[i].ctx.get());
    EXPECT_GT(fb_rec, 0);
    info("%s, session %zu fb_rec %d framerate %f\n", __func__, i, fb_rec, win.rx_fps[i]);
    info("%s, session %zu fb_send %d framerate %f\n", __func__, i, fb_send,
         win.tx_fps[i]);
    EXPECT_NEAR(win.rx_fps[i], expect_framerate[i], expect_framerate[i] * 0.1);
    EXPECT_NEAR(win.tx_fps[i], expect_framerate[i], expect_framerate[i] * 0.1);
  }
}

TEST(St22_tx, tx_user_pacing) {
  st22_tx_user_pacing_test(
      {
          {ST22_TYPE_FRAME_LEVEL, ST_FPS_P59_94, 1920, 1080, 1260, 602},
      },
      ST_TEST_LEVEL_ALL);
}

namespace {
/* Frame k, RTP timestamp k + 1 so the first differs from the RX context's initial
 * rtp_tmstamp 0, which the getter's new frame check compares with, is put with a TAI on
 * every second epoch from base_epoch, off the grid alternately by +0.4 and -0.3 frame:
 * only the nearest epoch, not floor, ceil, the TAI itself or the consecutive epochs of
 * the default pacing, matches it. */
struct St22RtpPacing {
  uint64_t base_epoch;
  long double frame_ns;
  std::mutex mtx;
  std::vector<std::pair<uint32_t, uint64_t>> arrivals; /* RTP timestamp, first pkt */
};
St22RtpPacing st22_rtp_pacing;

uint64_t st22_rtp_pacing_epoch(uint32_t rtp_ts) {
  return st22_rtp_pacing.base_epoch + 2 * (uint64_t)(rtp_ts - 1);
}

uint64_t st22_rtp_pacing_target(uint32_t rtp_ts) {
  long double off = (rtp_ts % 2) ? 0.4L : -0.3L;
  return (uint64_t)((st22_rtp_pacing_epoch(rtp_ts) + off) * st22_rtp_pacing.frame_ns);
}

void st22_rx_record_first_packet(tests_context* ctx, uint32_t tmstamp, bool newframe) {
  if (!newframe) return;
  uint64_t now = mtl_ptp_read_time(ctx->ctx->handle);
  std::lock_guard<std::mutex> rec(st22_rtp_pacing.mtx);
  st22_rtp_pacing.arrivals.emplace_back(tmstamp, now);
}

/* Stops the feed threads, then mtl, then frees the sessions, whichever were set up. */
struct St22RtpPacingSessions {
  explicit St22RtpPacingSessions(mtl_handle h) : mtl_guard(h) {
  }
  mtl_test::ScopedMtlStart mtl_guard;
  tests_context tx_ctx;
  tests_context rx_ctx;
  ScopedSt22Tx tx;
  ScopedSt22Rx rx;
  std::thread tx_thread;
  std::thread rx_thread;

  void stop_threads() {
    mtl_test::stop_worker(&tx_ctx, tx_thread);
    mtl_test::stop_worker(&rx_ctx, rx_thread);
  }
  ~St22RtpPacingSessions() {
    stop_threads();
    mtl_guard.stop();
    tx.reset();
    rx.reset();
  }
};
}  // namespace

/* ST22 has no EXACT flag: st22_tx_put_mbuf_tai() only picks the epoch the frame goes
 * on, the nearest to its TAI. Checked from RX by the epoch each frame arrives in. */
static void st22_tx_rtp_user_pacing_test(enum st_test_level level) {
  auto ctx = st_test_ctx();
  constexpr int kPktDataLen = 1280;
  constexpr int kTotalPkts = 540;
  constexpr uint64_t kLeadEpochs = 18;    /* ~300ms */
  constexpr int64_t kEarlyNs = 1'000'000; /* RL warm-up and clock reads */
  constexpr int64_t kLateNs = 3'000'000;  /* tr_offset and RX thread wake */
  constexpr size_t kMinFrames = 60;
  /* Every 2nd epoch at 59.94 fps (~29.97 fps) after 18 lead epochs (~0.30 s):
   * 100 frames takes ~3.6 s, satisfies kMinFrames=60, and keeps the 1% miss
   * budget (arrivals.size() / 100 >= 1) active without a blind sleep(). */
  constexpr size_t kTargetFrames = 100;

  MTL_TEST_REQUIRE_LEVEL(ctx, level);
  MTL_TEST_REQUIRE_PORTS(ctx, 2);

  St22RtpPacingSessions s(ctx->handle);
  {
    std::lock_guard<std::mutex> lck(st22_rtp_pacing.mtx);
    st22_rtp_pacing.arrivals.clear();
  }
  ASSERT_GE(s.mtl_guard.start(), 0);

  for (tests_context* c : {&s.tx_ctx, &s.rx_ctx}) {
    c->ctx = ctx;
    c->fb_cnt = 3;
    c->pkt_data_len = kPktDataLen;
    c->total_pkts_in_frame = kTotalPkts;
    c->frame_size = (size_t)kPktDataLen * kTotalPkts;
  }

  /* The create/free ops, made a one-port RTP-level pair from TX port P to RX port R. */
  struct st22_tx_ops ops_tx;
  st22_tx_ops_init(&s.tx_ctx, &ops_tx);
  ops_tx.num_port = 1;
  if (!ctx->mcast_only)
    memcpy(ops_tx.dip_addr[MTL_SESSION_PORT_P], ctx->para.sip_addr[MTL_PORT_R],
           MTL_IP_ADDR_LEN);
  ops_tx.type = ST22_TYPE_RTP_LEVEL;
  ops_tx.flags = ST22_TX_FLAG_USER_PACING | ST22_TX_FLAG_USER_TIMESTAMP;
  ops_tx.rtp_pkt_size = kPktDataLen + sizeof(struct st22_rfc9134_rtp_hdr);
  ops_tx.rtp_frame_total_pkts = kTotalPkts;
  s.tx.reset(st22_tx_create(ctx->handle, &ops_tx));
  ASSERT_TRUE(static_cast<bool>(s.tx));

  struct st22_rx_ops ops_rx;
  st22_rx_ops_init(&s.rx_ctx, &ops_rx);
  ops_rx.num_port = 1;
  if (!ctx->mcast_only)
    memcpy(ops_rx.ip_addr[MTL_SESSION_PORT_P], ctx->para.sip_addr[MTL_PORT_P],
           MTL_IP_ADDR_LEN);
  (void)snprintf(ops_rx.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
                 ctx->para.port[MTL_PORT_R]);
  ops_rx.type = ST22_TYPE_RTP_LEVEL;
  s.rx.reset(st22_rx_create(ctx->handle, &ops_rx));
  ASSERT_TRUE(static_cast<bool>(s.rx));

  st22_rtp_pacing.frame_ns = (long double)NS_PER_S / st_frame_rate(ops_tx.fps);
  st22_rtp_pacing.base_epoch =
      (uint64_t)(mtl_ptp_read_time(ctx->handle) / st22_rtp_pacing.frame_ns) + kLeadEpochs;
  s.tx_ctx.rtp_tmstamp = 1;
  set_handle(&s.rx_ctx, s.rx.get());
  set_handle(&s.tx_ctx, s.tx.get());
  s.rx_thread =
      std::thread(st22_rx_get_packet_hook, &s.rx_ctx, st22_rx_record_first_packet);
  s.tx_thread = std::thread(st22_tx_feed_packet_tai, &s.tx_ctx, st22_rtp_pacing_target);

  mtl_test::wait_until(
      []() {
        std::lock_guard<std::mutex> lck(st22_rtp_pacing.mtx);
        return st22_rtp_pacing.arrivals.size() >= kTargetFrames;
      },
      /*timeout_ms=*/8000, /*poll_ms=*/10);
  s.stop_threads();

  std::vector<std::pair<uint32_t, uint64_t>> arrivals;
  {
    std::lock_guard<std::mutex> lck(st22_rtp_pacing.mtx);
    arrivals = st22_rtp_pacing.arrivals;
  }
  size_t misses = 0;
  for (const auto& a : arrivals) {
    uint64_t epoch_start =
        (uint64_t)(st22_rtp_pacing_epoch(a.first) * st22_rtp_pacing.frame_ns);
    int64_t delta = (int64_t)(a.second - epoch_start);
    if (delta < -kEarlyNs || delta > kLateNs) {
      misses++;
      info("%s, frame %u arrived %" PRId64 "ns from its nearest epoch\n", __func__,
           a.first, delta);
    }
  }
  info("%s, frames %zu misses %zu\n", __func__, arrivals.size(), misses);
  EXPECT_GE(arrivals.size(), kMinFrames);
  /* a frame whose first packet is lost is first seen later in its epoch */
  EXPECT_LE(misses, arrivals.size() / 100);
}

TEST(St22_tx, rtp_user_pacing_nearest_epoch) {
  st22_tx_rtp_user_pacing_test(ST_TEST_LEVEL_ALL);
}
