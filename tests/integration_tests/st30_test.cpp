/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include <thread>
#include <utility>
#include <vector>

#include "log.h"
#include "test_harness.hpp"
#include "tests.hpp"

#define ST30_TEST_PAYLOAD_TYPE (111)
#define ST30_TEST_UDP_PORT (20000)

/*
 * Audio TX/RX rate measurement parameters.
 * Audio runs at 250-12500 pps (4ms down to 80us ptime; 500 pps for 2x
 * user-timestamp spacing). A 600ms window collects >= 150 packets
 * (<= 0.67% edge quantization error), well inside the +/-10% tolerance.
 */
static constexpr int kSt30RateWarmupFrames = 5;
static constexpr int kSt30RateMinDeltaFrames = 40;
static constexpr int kSt30RateMinWindowMs = 600;
static constexpr int kSt30PhaseTargetFrames = 30;
static constexpr int kSt30PhaseTimeoutMs = 5000;

static void st30_verify_sha(tests_context* ctx, const void* payload) {
  unsigned char result[SHA256_DIGEST_LENGTH];
  SHA256(static_cast<const unsigned char*>(payload), ctx->frame_size, result);
  int i;
  for (i = 0; i < TEST_SHA_HIST_NUM; i++) {
    if (!memcmp(result, ctx->shas[i], SHA256_DIGEST_LENGTH)) break;
  }
  if (i >= TEST_SHA_HIST_NUM) {
    test_sha_dump("st30_rx_error_sha", result);
    ctx->sha_fail_cnt++;
  }
  ctx->check_sha_frame_cnt++;
}

static int tx_audio_next_frame(void* priv, uint16_t* next_frame_idx,
                               struct st30_tx_frame_meta* /*meta*/) {
  return tx_next_frame(priv, next_frame_idx);
}

static int tx_audio_next_frame_timestamp(void* priv, uint16_t* next_frame_idx,
                                         struct st30_tx_frame_meta* meta) {
  auto ctx = static_cast<tests_context*>(priv);

  if (!__atomic_load_n(&ctx->handle, __ATOMIC_ACQUIRE)) return -EIO; /* not ready */

  if (!ctx->ptp_time_first_frame) {
    ctx->ptp_time_first_frame = mtl_ptp_read_time(ctx->ctx->handle);
  }

  const int sent = __atomic_fetch_add(&ctx->fb_send, 1, __ATOMIC_RELAXED);
  meta->tfmt = ST10_TIMESTAMP_FMT_TAI;
  meta->timestamp = ctx->ptp_time_first_frame + sent * ctx->frame_time * 2;
  *next_frame_idx = ctx->fb_idx;
  dbg("%s, next_frame_idx %d\n", __func__, *next_frame_idx);
  ctx->fb_idx++;
  if (ctx->fb_idx >= ctx->fb_cnt) ctx->fb_idx = 0;
  return 0;
}

static int tx_audio_build_rtp_packet(tests_context* s, struct st_rfc3550_rtp_hdr* rtp,
                                     uint16_t* pkt_len) {
  memset(rtp, 0x0, sizeof(*rtp));
  rtp->csrc_count = 0;
  rtp->extension = 0;
  rtp->padding = 0;
  rtp->version = 2;
  rtp->marker = 0;
  rtp->payload_type = ST30_TEST_PAYLOAD_TYPE;
  rtp->ssrc = htonl(0x66666666 + s->idx);
  rtp->tmstamp = s->rtp_tmstamp;
  s->rtp_tmstamp++;
  const uint16_t seq =
      static_cast<uint16_t>(__atomic_fetch_add(&s->seq_id, 1, __ATOMIC_RELAXED) & 0xFFFF);
  rtp->seq_number = htons(seq);
  if (s->check_sha) {
    uint8_t* payload = reinterpret_cast<uint8_t*>(rtp) + sizeof(*rtp);
    mtl_memcpy(payload, s->frame_buf[s->fb_idx], s->pkt_data_len);
    s->fb_idx++;
    if (s->fb_idx >= TEST_SHA_HIST_NUM) s->fb_idx = 0;
  }
  *pkt_len = sizeof(struct st_rfc3550_rtp_hdr) + s->pkt_data_len;
  return 0;
}

static void tx_feed_packet(void* args) {
  auto ctx = static_cast<tests_context*>(args);
  auto handle =
      static_cast<st30_tx_handle>(__atomic_load_n(&ctx->handle, __ATOMIC_ACQUIRE));
  void* mbuf;
  void* usrptr = NULL;
  uint16_t mbuf_len = 0;
  std::unique_lock<std::mutex> lck(ctx->mtx, std::defer_lock);
  while (!ctx->stop) {
    mbuf = st30_tx_get_mbuf(handle, &usrptr);
    if (!mbuf) {
      lck.lock();
      mbuf = st30_tx_get_mbuf(handle, &usrptr);
      if (mbuf) {
        lck.unlock();
      } else {
        if (!ctx->stop) ctx->cv.wait(lck);
        lck.unlock();
        continue;
      }
    }

    tx_audio_build_rtp_packet(ctx, static_cast<struct st_rfc3550_rtp_hdr*>(usrptr),
                              &mbuf_len);
    st30_tx_put_mbuf(handle, mbuf, mbuf_len);
  }
}

static int tx_rtp_done(void* args) {
  auto ctx = static_cast<tests_context*>(args);

  if (!__atomic_load_n(&ctx->handle, __ATOMIC_ACQUIRE)) return -EIO; /* not ready */

  std::unique_lock<std::mutex> lck(ctx->mtx);
  ctx->cv.notify_all();
  __atomic_fetch_add(&ctx->fb_send, 1, __ATOMIC_RELAXED);
  return 0;
}

static int rx_rtp_ready(void* args) {
  auto ctx = static_cast<tests_context*>(args);

  if (!__atomic_load_n(&ctx->handle, __ATOMIC_ACQUIRE)) return -EIO; /* not ready */

  std::unique_lock<std::mutex> lck(ctx->mtx);
  ctx->cv.notify_all();
  return 0;
}

static void rx_get_packet(void* args) {
  auto ctx = static_cast<tests_context*>(args);
  auto handle =
      static_cast<st30_rx_handle>(__atomic_load_n(&ctx->handle, __ATOMIC_ACQUIRE));
  void* mbuf;
  void* usrptr = NULL;
  uint16_t mbuf_len = 0;
  std::unique_lock<std::mutex> lck(ctx->mtx, std::defer_lock);
  while (!ctx->stop) {
    mbuf = st30_rx_get_mbuf(handle, &usrptr, &mbuf_len);
    if (!mbuf) {
      lck.lock();
      mbuf = st30_rx_get_mbuf(handle, &usrptr, &mbuf_len);
      if (mbuf) {
        lck.unlock();
      } else {
        if (!ctx->stop) ctx->cv.wait(lck);
        lck.unlock();
        continue;
      }
    }
    if (ctx->check_sha) {
      const uint8_t* payload =
          static_cast<const uint8_t*>(usrptr) + sizeof(struct st_rfc3550_rtp_hdr);
      st30_verify_sha(ctx, payload);
    }
    __atomic_fetch_add(&ctx->fb_rec, 1, __ATOMIC_RELAXED);
    st30_rx_put_mbuf(handle, mbuf);
  }
}

static int st30_rx_frame_ready(void* priv, void* frame,
                               struct st30_rx_frame_meta* /*meta*/) {
  auto ctx = static_cast<tests_context*>(priv);
  auto handle =
      static_cast<st30_rx_handle>(__atomic_load_n(&ctx->handle, __ATOMIC_ACQUIRE));

  if (!handle) return -EIO;

  /* Direct SHA check because audio frame size is small. */
  if (ctx->check_sha) st30_verify_sha(ctx, frame);

  st30_rx_put_framebuff(handle, frame);
  __atomic_fetch_add(&ctx->fb_rec, 1, __ATOMIC_RELAXED);
  return 0;
}

static int st30_rx_meta_frame_ready(void* priv, void* frame,
                                    struct st30_rx_frame_meta* meta) {
  auto ctx = static_cast<tests_context*>(priv);
  auto handle =
      static_cast<st30_rx_handle>(__atomic_load_n(&ctx->handle, __ATOMIC_ACQUIRE));
  auto expect_meta = static_cast<struct st30_rx_frame_meta*>(ctx->priv);

  if (!handle) return -EIO;

  __atomic_fetch_add(&ctx->fb_rec, 1, __ATOMIC_RELAXED);
  if (expect_meta->sampling != meta->sampling) ctx->rx_meta_fail_cnt++;
  if (expect_meta->channel != meta->channel) ctx->rx_meta_fail_cnt++;
  if (expect_meta->fmt != meta->fmt) ctx->rx_meta_fail_cnt++;
  if (expect_meta->timestamp == meta->timestamp) ctx->rx_meta_fail_cnt++;
  expect_meta->timestamp = meta->timestamp;

  st30_rx_put_framebuff(handle, frame);
  return 0;
}

static void st30_rx_ops_init(tests_context* st30, struct st30_rx_ops* ops) {
  auto ctx = st30->ctx;

  memset(ops, 0, sizeof(*ops));
  ops->name = "st30_test";
  ops->priv = st30;
  ops->num_port = ctx->same_dual_port ? 1 : ctx->para.num_ports;
  memcpy(ops->ip_addr[MTL_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P], MTL_IP_ADDR_LEN);
  snprintf(ops->port[MTL_PORT_P], MTL_PORT_MAX_LEN, "%s", ctx->para.port[MTL_PORT_P]);
  ops->udp_port[MTL_PORT_P] = ST30_TEST_UDP_PORT + st30->idx;
  if (ops->num_port == 2) {
    memcpy(ops->ip_addr[MTL_PORT_R], ctx->mcast_ip_addr[MTL_PORT_R], MTL_IP_ADDR_LEN);
    snprintf(ops->port[MTL_PORT_R], MTL_PORT_MAX_LEN, "%s", ctx->para.port[MTL_PORT_R]);
    ops->udp_port[MTL_PORT_R] = ST30_TEST_UDP_PORT + st30->idx;
  }
  ops->type = ST30_TYPE_FRAME_LEVEL;
  ops->channel = 2;
  ops->fmt = ST30_FMT_PCM16;
  ops->payload_type = ST30_TEST_PAYLOAD_TYPE;
  ops->sampling = ST30_SAMPLING_48K;
  ops->ptime = ST30_PTIME_1MS;
  ops->framebuff_cnt = st30->fb_cnt;
  ops->framebuff_size =
      st30_get_packet_size(ops->fmt, ops->ptime, ops->sampling, ops->channel);
  ops->notify_frame_ready = st30_rx_frame_ready;
  ops->notify_rtp_ready = rx_rtp_ready;
  ops->rtp_ring_size = 1024;
}

static void st30_tx_ops_init(tests_context* st30, struct st30_tx_ops* ops) {
  auto ctx = st30->ctx;

  memset(ops, 0, sizeof(*ops));
  ops->name = "st30_test";
  ops->priv = st30;
  ops->num_port = ctx->same_dual_port ? 1 : ctx->para.num_ports;
  memcpy(ops->dip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
         MTL_IP_ADDR_LEN);
  snprintf(ops->port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
           ctx->para.port[MTL_PORT_P]);
  ops->udp_port[MTL_SESSION_PORT_P] = ST30_TEST_UDP_PORT + st30->idx;
  if (ops->num_port == 2) {
    memcpy(ops->dip_addr[MTL_SESSION_PORT_R], ctx->mcast_ip_addr[MTL_PORT_R],
           MTL_IP_ADDR_LEN);
    snprintf(ops->port[MTL_SESSION_PORT_R], MTL_PORT_MAX_LEN, "%s",
             ctx->para.port[MTL_PORT_R]);
    ops->udp_port[MTL_SESSION_PORT_R] = ST30_TEST_UDP_PORT + st30->idx;
  }
  ops->type = ST30_TYPE_FRAME_LEVEL;
  ops->channel = 2;
  ops->fmt = ST30_FMT_PCM16;
  ops->payload_type = ST30_TEST_PAYLOAD_TYPE;
  ops->sampling = ST30_SAMPLING_48K;
  ops->ptime = ST30_PTIME_1MS;
  ops->framebuff_cnt = st30->fb_cnt;
  st30->pkt_data_len =
      st30_get_packet_size(ops->fmt, ops->ptime, ops->sampling, ops->channel);
  ops->framebuff_size = st30->pkt_data_len;
  ops->get_next_frame = tx_audio_next_frame;
  ops->notify_rtp_done = tx_rtp_done;
  ops->rtp_ring_size = 1024;
}

static void st30_tx_assert_cnt(int expect_s30_tx_cnt) {
  auto ctx = st_test_ctx();
  struct st_var_info var;
  int ret = st_get_var_info(ctx->handle, &var);
  EXPECT_GE(ret, 0);
  EXPECT_EQ(var.st30_tx_sessions_cnt, expect_s30_tx_cnt);
}

static void st30_rx_assert_cnt(int expect_s30_rx_cnt) {
  auto ctx = st_test_ctx();
  struct st_var_info var;
  int ret = st_get_var_info(ctx->handle, &var);
  EXPECT_GE(ret, 0);
  EXPECT_EQ(var.st30_rx_sessions_cnt, expect_s30_rx_cnt);
}

TEST(St30_tx, create_free_single) {
  create_free_test(st30_tx, 0, 1, 1);
}
TEST(St30_tx, create_free_multi) {
  create_free_test(st30_tx, 0, 1, 6);
}
TEST(St30_tx, create_free_mix) {
  create_free_test(st30_tx, 2, 3, 4);
}
TEST(St30_tx, create_free_max) {
  create_free_max(st30_tx, TEST_CREATE_FREE_MAX);
}
TEST(St30_tx, create_expect_fail) {
  expect_fail_test(st30_tx);
}
TEST(St30_tx, create_expect_fail_ring_sz) {
  uint16_t ring_size = 0;
  expect_fail_test_rtp_ring(st30_tx, ST30_TYPE_RTP_LEVEL, ring_size);
  ring_size = 128 + 1;
  expect_fail_test_rtp_ring(st30_tx, ST30_TYPE_RTP_LEVEL, ring_size);
}
TEST(St30_tx, get_framebuffer) {
  uint16_t fbcnt = 3;
  test_get_framebuffer(st30_tx, fbcnt);
  fbcnt = 1000;
  test_get_framebuffer(st30_tx, fbcnt);
}
TEST(St30_tx, get_framebuffer_expect_fail) {
  uint16_t fbcnt = 3;
  expect_fail_test_get_framebuffer(st30_tx, fbcnt);
  fbcnt = 1000;
  expect_fail_test_get_framebuffer(st30_tx, fbcnt);
}

TEST(St30_rx, create_free_single) {
  create_free_test(st30_rx, 0, 1, 1);
}
TEST(St30_rx, create_free_multi) {
  create_free_test(st30_rx, 0, 1, 6);
}
TEST(St30_rx, create_free_mix) {
  create_free_test(st30_rx, 2, 3, 4);
}
TEST(St30_rx, create_free_max) {
  create_free_max(st30_rx, TEST_CREATE_FREE_MAX);
}
TEST(St30_rx, create_expect_fail) {
  expect_fail_test(st30_rx);
}
TEST(St30_rx, create_expect_fail_ring_sz) {
  uint16_t ring_size = 0;
  expect_fail_test_rtp_ring(st30_rx, ST30_TYPE_RTP_LEVEL, ring_size);
  ring_size = 128 + 1;
  expect_fail_test_rtp_ring(st30_rx, ST30_TYPE_RTP_LEVEL, ring_size);
}

namespace {

struct St30SessionSpec {
  enum st30_type type = ST30_TYPE_FRAME_LEVEL;
  enum st30_sampling sampling = ST30_SAMPLING_48K;
  enum st30_ptime ptime = ST30_PTIME_1MS;
  uint16_t channel = 2;
  enum st30_fmt fmt = ST30_FMT_PCM16;
};

template <typename Handle, int (*FreeFn)(Handle)>
struct St30Session {
  mtl_test::ScopedContext ctx;
  mtl_test::ScopedHandle<Handle, FreeFn> handle;
  std::thread worker;

  St30Session() = default;
  St30Session(St30Session&&) noexcept = default;
  St30Session& operator=(St30Session&& other) noexcept {
    if (this != &other) {
      stop();
      handle = std::move(other.handle);
      ctx = std::move(other.ctx);
      worker = std::move(other.worker);
    }
    return *this;
  }
  St30Session(const St30Session&) = delete;
  St30Session& operator=(const St30Session&) = delete;
  ~St30Session() {
    stop();
  }
  void stop() {
    mtl_test::stop_worker(ctx.get(), worker);
  }
};

using St30TxSession = St30Session<st30_tx_handle, st30_tx_free>;
using St30RxSession = St30Session<st30_rx_handle, st30_rx_free>;

double st30_expected_fps(enum st30_ptime ptime) {
  double pkt_time_ns = st30_get_packet_time(ptime);
  return (pkt_time_ns > 0.0) ? (static_cast<double>(NS_PER_S) / pkt_time_ns) : 1000.0;
}

template <typename SessionVec>
std::vector<tests_context*> collect_ctxs(const SessionVec& sessions) {
  std::vector<tests_context*> out;
  out.reserve(sessions.size());
  for (const auto& s : sessions) out.push_back(s.ctx.get());
  return out;
}

void fill_loopback_tx_ops(struct st30_tx_ops* ops, tests_context* st30,
                          const St30SessionSpec& spec, uint16_t udp_port,
                          uint32_t ssrc = 0, const char* name = "st30_test") {
  auto ctx = st30->ctx;
  memset(ops, 0, sizeof(*ops));
  ops->name = name;
  ops->priv = st30;
  ops->num_port = 1;
  const uint8_t* dip =
      ctx->mcast_only ? ctx->mcast_ip_addr[MTL_PORT_P] : ctx->para.sip_addr[MTL_PORT_R];
  memcpy(ops->dip_addr[MTL_SESSION_PORT_P], dip, MTL_IP_ADDR_LEN);
  snprintf(ops->port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
           ctx->para.port[MTL_PORT_P]);
  ops->udp_port[MTL_SESSION_PORT_P] = udp_port;
  ops->type = spec.type;
  ops->sampling = spec.sampling;
  ops->channel = spec.channel;
  ops->fmt = spec.fmt;
  ops->payload_type = ST30_TEST_PAYLOAD_TYPE;
  ops->ssrc = ssrc;
  ops->ptime = spec.ptime;
  ops->pacing_way = ctx->tx_audio_pacing_way;
  int pkt_size = st30_get_packet_size(ops->fmt, ops->ptime, ops->sampling, ops->channel);
  EXPECT_GE(pkt_size, 0);
  ops->framebuff_size = static_cast<uint32_t>(pkt_size);
  ops->framebuff_cnt = st30->fb_cnt;
  ops->get_next_frame = tx_audio_next_frame;
  ops->notify_rtp_done = tx_rtp_done;
  ops->rtp_ring_size = 1024;
  st30->pkt_data_len = pkt_size;
}

void fill_loopback_rx_ops(struct st30_rx_ops* ops, tests_context* st30,
                          const St30SessionSpec& spec, uint16_t udp_port,
                          uint32_t ssrc = 0, const char* name = "st30_test") {
  auto ctx = st30->ctx;
  memset(ops, 0, sizeof(*ops));
  ops->name = name;
  ops->priv = st30;
  ops->num_port = 1;
  const uint8_t* ip =
      ctx->mcast_only ? ctx->mcast_ip_addr[MTL_PORT_P] : ctx->para.sip_addr[MTL_PORT_P];
  memcpy(ops->ip_addr[MTL_SESSION_PORT_P], ip, MTL_IP_ADDR_LEN);
  snprintf(ops->port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
           ctx->para.port[MTL_PORT_R]);
  ops->udp_port[MTL_SESSION_PORT_P] = udp_port;
  ops->type = spec.type;
  ops->sampling = spec.sampling;
  ops->channel = spec.channel;
  ops->fmt = spec.fmt;
  ops->payload_type = ST30_TEST_PAYLOAD_TYPE;
  ops->ssrc = ssrc;
  ops->ptime = spec.ptime;
  int pkt_size = st30_get_packet_size(ops->fmt, ops->ptime, ops->sampling, ops->channel);
  EXPECT_GE(pkt_size, 0);
  ops->framebuff_size = static_cast<uint32_t>(pkt_size);
  ops->framebuff_cnt = st30->fb_cnt;
  ops->notify_frame_ready = st30_rx_frame_ready;
  ops->notify_rtp_ready = rx_rtp_ready;
  ops->rtp_ring_size = 1024;
}

}  // namespace

static void st30_tx_fps_test(const std::vector<St30SessionSpec>& specs,
                             enum st_test_level level) {
  auto ctx = st_test_ctx();
  auto m_handle = ctx->handle;
  MTL_TEST_REQUIRE_LEVEL(ctx, level);

  const size_t sessions = specs.size();
  mtl_test::ScopedMtlStart mtl_guard(m_handle);
  std::vector<St30TxSession> tx;
  tx.reserve(sessions);
  std::vector<double> expect_framerate(sessions, 0.0);

  for (size_t i = 0; i < sessions; i++) {
    const auto& spec = specs[i];
    expect_framerate[i] = st30_expected_fps(spec.ptime);

    tx.emplace_back();
    auto& s = tx.back();
    s.ctx = mtl_test::make_context(ctx, static_cast<int>(i), 3);

    struct st30_tx_ops ops;
    st30_tx_ops_init(s.ctx.get(), &ops);
    ops.type = spec.type;
    ops.sampling = spec.sampling;
    ops.channel = spec.channel;
    ops.fmt = spec.fmt;
    ops.payload_type = ST30_TEST_PAYLOAD_TYPE;
    ops.ptime = spec.ptime;
    ops.framebuff_size = st30_get_sample_size(ops.fmt) *
                         st30_get_sample_num(ops.ptime, ops.sampling) * ops.channel;
    s.ctx->pkt_data_len =
        st30_get_packet_size(ops.fmt, ops.ptime, ops.sampling, ops.channel);

    s.handle.reset(st30_tx_create(m_handle, &ops));
    ASSERT_TRUE(static_cast<bool>(s.handle));
    __atomic_store_n(&s.ctx->handle, s.handle.get(), __ATOMIC_RELEASE);

    if (spec.type == ST30_TYPE_RTP_LEVEL) {
      s.worker = std::thread(tx_feed_packet, s.ctx.get());
    }
  }

  EXPECT_GE(mtl_guard.start(), 0);
  auto tx_ctxs = collect_ctxs(tx);
  auto rate_res =
      mtl_test::measure_rate_window(tx_ctxs, mtl_test::tx_frames, kSt30RateWarmupFrames,
                                    kSt30RateMinDeltaFrames, kSt30RateMinWindowMs);

  for (auto& s : tx) s.stop();
  EXPECT_GE(mtl_guard.stop(), 0);

  for (size_t i = 0; i < sessions; i++) {
    const int fb_send = mtl_test::tx_frames(tx[i].ctx.get());
    EXPECT_GT(fb_send, 0);
    info("%s, session %zu fb_send %d framerate %f\n", __func__, i, fb_send,
         rate_res.rate[i]);
    EXPECT_NEAR(rate_res.rate[i], expect_framerate[i], expect_framerate[i] * 0.1);
  }
}

static void st30_tx_pcm_sweep_test(enum st30_type type, enum st30_sampling sampling,
                                   enum st30_ptime ptime, uint16_t channel,
                                   enum st_test_level level = ST_TEST_LEVEL_ALL) {
  for (enum st30_fmt fmt : {ST30_FMT_PCM8, ST30_FMT_PCM16, ST30_FMT_PCM24}) {
    st30_tx_fps_test({{type, sampling, ptime, channel, fmt}}, level);
    if (testing::Test::IsSkipped() || testing::Test::HasFatalFailure()) return;
  }
}

static void st30_rx_fps_test(const std::vector<St30SessionSpec>& specs,
                             enum st_test_level level, bool check_sha = false) {
  auto ctx = st_test_ctx();
  auto m_handle = ctx->handle;
  MTL_TEST_REQUIRE_PORTS(ctx, 2);
  MTL_TEST_REQUIRE_LEVEL(ctx, level);

  const size_t sessions = specs.size();
  mtl_test::ScopedMtlStart mtl_guard(m_handle);
  std::vector<St30TxSession> tx;
  std::vector<St30RxSession> rx;
  tx.reserve(sessions);
  rx.reserve(sessions);
  std::vector<double> expect_framerate(sessions, 0.0);

  for (size_t i = 0; i < sessions; i++) {
    const auto& spec = specs[i];
    const uint16_t udp_port = ST30_TEST_UDP_PORT + static_cast<uint16_t>(i * 2);
    const uint32_t ssrc = i ? static_cast<uint32_t>(i) + 0x66666666u : 0u;

    tx.emplace_back();
    auto& s = tx.back();
    const uint16_t fb_cnt = check_sha ? TEST_SHA_HIST_NUM : 3;
    s.ctx = mtl_test::make_context(ctx, static_cast<int>(i), fb_cnt, check_sha);

    struct st30_tx_ops ops_tx;
    fill_loopback_tx_ops(&ops_tx, s.ctx.get(), spec, udp_port, ssrc);
    s.handle.reset(st30_tx_create(m_handle, &ops_tx));
    ASSERT_TRUE(static_cast<bool>(s.handle));

    if (check_sha) {
      for (int frame = 0; frame < TEST_SHA_HIST_NUM; frame++) {
        uint8_t* fb = nullptr;
        if (spec.type == ST30_TYPE_FRAME_LEVEL) {
          fb = static_cast<uint8_t*>(st30_tx_get_framebuffer(s.handle.get(), frame));
        } else {
          s.ctx->frame_buf[frame] =
              static_cast<uint8_t*>(st_test_zmalloc(ops_tx.framebuff_size));
          fb = s.ctx->frame_buf[frame];
        }
        ASSERT_TRUE(fb != nullptr);
        st_test_rand_data(fb, ops_tx.framebuff_size, frame);
        unsigned char* result = s.ctx->shas[frame];
        SHA256(fb, ops_tx.framebuff_size, result);
        test_sha_dump("st30_rx", result);
      }
    }

    __atomic_store_n(&s.ctx->handle, s.handle.get(), __ATOMIC_RELEASE);
    if (spec.type == ST30_TYPE_RTP_LEVEL) {
      s.worker = std::thread(tx_feed_packet, s.ctx.get());
    }
  }

  for (size_t i = 0; i < sessions; i++) {
    const auto& spec = specs[i];
    const uint16_t udp_port = ST30_TEST_UDP_PORT + static_cast<uint16_t>(i * 2);
    const uint32_t ssrc = i ? static_cast<uint32_t>(i) + 0x66666666u : 0u;

    rx.emplace_back();
    auto& s = rx.back();
    s.ctx = mtl_test::make_context(ctx, static_cast<int>(i), 3, check_sha);

    struct st30_rx_ops ops_rx;
    fill_loopback_rx_ops(&ops_rx, s.ctx.get(), spec, udp_port, ssrc);
    expect_framerate[i] = st30_expected_fps(ops_rx.ptime);

    s.handle.reset(st30_rx_create(m_handle, &ops_rx));
    ASSERT_TRUE(static_cast<bool>(s.handle));

    s.ctx->frame_size = ops_rx.framebuff_size;
    if (check_sha) {
      memcpy(s.ctx->shas, tx[i].ctx->shas, TEST_SHA_HIST_NUM * SHA256_DIGEST_LENGTH);
    }
    __atomic_store_n(&s.ctx->handle, s.handle.get(), __ATOMIC_RELEASE);
    if (spec.type == ST30_TYPE_RTP_LEVEL) {
      s.worker = std::thread(rx_get_packet, s.ctx.get());
    }

    struct st_queue_meta q_meta;
    EXPECT_GE(st30_rx_get_queue_meta(s.handle.get(), &q_meta), 0);
  }

  EXPECT_GE(mtl_guard.start(), 0);
  auto rx_ctxs = collect_ctxs(rx);
  auto rate_res =
      mtl_test::measure_rate_window(rx_ctxs, mtl_test::rx_frames, kSt30RateWarmupFrames,
                                    kSt30RateMinDeltaFrames, kSt30RateMinWindowMs);

  for (auto& s : tx) s.stop();
  for (auto& s : rx) s.stop();
  EXPECT_GE(mtl_guard.stop(), 0);

  for (size_t i = 0; i < sessions; i++) {
    const int fb_rec = mtl_test::rx_frames(rx[i].ctx.get());
    EXPECT_GT(fb_rec, 0);
    EXPECT_LE(rx[i].ctx->sha_fail_cnt, 2);
    if (check_sha) {
      EXPECT_GT(rx[i].ctx->check_sha_frame_cnt, 0);
    }
    info("%s, session %zu fb_rec %d framerate %f\n", __func__, i, fb_rec,
         rate_res.rate[i]);
    EXPECT_NEAR(rate_res.rate[i], expect_framerate[i], expect_framerate[i] * 0.1);
  }
}

TEST(St30_tx, frame_48k_mono_s1) {
  st30_tx_pcm_sweep_test(ST30_TYPE_FRAME_LEVEL, ST30_SAMPLING_48K, ST30_PTIME_1MS, 1);
}
TEST(St30_tx, rtp_96k_mono_s1) {
  st30_tx_pcm_sweep_test(ST30_TYPE_RTP_LEVEL, ST30_SAMPLING_96K, ST30_PTIME_1MS, 1);
}
TEST(St30_tx, frame_48k_stereo_s1) {
  st30_tx_pcm_sweep_test(ST30_TYPE_FRAME_LEVEL, ST30_SAMPLING_48K, ST30_PTIME_1MS, 2);
}
TEST(St30_tx, frame_48k_stereo_125us_s1) {
  st30_tx_pcm_sweep_test(ST30_TYPE_FRAME_LEVEL, ST30_SAMPLING_48K, ST30_PTIME_125US, 2);
}
TEST(St30_tx, rtp_48k_stereo_125us_s1) {
  st30_tx_pcm_sweep_test(ST30_TYPE_RTP_LEVEL, ST30_SAMPLING_48K, ST30_PTIME_125US, 2);
}
TEST(St30_tx, rtp_96k_stereo_s1) {
  st30_tx_pcm_sweep_test(ST30_TYPE_RTP_LEVEL, ST30_SAMPLING_96K, ST30_PTIME_1MS, 2);
}
TEST(St30_tx, frame_48k_sgrp_s1) {
  st30_tx_pcm_sweep_test(ST30_TYPE_FRAME_LEVEL, ST30_SAMPLING_48K, ST30_PTIME_1MS, 4);
}
TEST(St30_tx, frame_96k_sgrp_s1) {
  st30_tx_pcm_sweep_test(ST30_TYPE_FRAME_LEVEL, ST30_SAMPLING_96K, ST30_PTIME_1MS, 4);
}
TEST(St30_tx, mix_96k_stereo_s3) {
  st30_tx_fps_test(
      {
          {ST30_TYPE_RTP_LEVEL, ST30_SAMPLING_96K, ST30_PTIME_1MS, 2, ST30_FMT_PCM8},
          {ST30_TYPE_FRAME_LEVEL, ST30_SAMPLING_96K, ST30_PTIME_1MS, 2, ST30_FMT_PCM16},
          {ST30_TYPE_RTP_LEVEL, ST30_SAMPLING_96K, ST30_PTIME_1MS, 2, ST30_FMT_PCM24},
      },
      ST_TEST_LEVEL_ALL);
}
TEST(St30_tx, mix_48k_96_mix) {
  st30_tx_fps_test(
      {
          {ST30_TYPE_FRAME_LEVEL, ST30_SAMPLING_96K, ST30_PTIME_1MS, 2, ST30_FMT_PCM8},
          {ST30_TYPE_RTP_LEVEL, ST30_SAMPLING_48K, ST30_PTIME_1MS, 1, ST30_FMT_PCM16},
          {ST30_TYPE_RTP_LEVEL, ST30_SAMPLING_48K, ST30_PTIME_1MS, 4, ST30_FMT_PCM24},
      },
      ST_TEST_LEVEL_ALL);
}
TEST(St30_rx, mix_48k_96_mix) {
  st30_rx_fps_test(
      {
          {ST30_TYPE_FRAME_LEVEL, ST30_SAMPLING_96K, ST30_PTIME_1MS, 2, ST30_FMT_PCM8},
          {ST30_TYPE_RTP_LEVEL, ST30_SAMPLING_48K, ST30_PTIME_1MS, 1, ST30_FMT_PCM16},
          {ST30_TYPE_RTP_LEVEL, ST30_SAMPLING_48K, ST30_PTIME_1MS, 4, ST30_FMT_PCM24},
      },
      ST_TEST_LEVEL_ALL);
}
TEST(St30_rx, frame_digest_48k_96_mix) {
  st30_rx_fps_test(
      {
          {ST30_TYPE_FRAME_LEVEL, ST30_SAMPLING_96K, ST30_PTIME_1MS, 2, ST30_FMT_PCM16},
          {ST30_TYPE_FRAME_LEVEL, ST30_SAMPLING_48K, ST30_PTIME_1MS, 1, ST30_FMT_PCM24},
      },
      ST_TEST_LEVEL_ALL, true);
}
TEST(St30_rx, rtp_digest_48k_96_mix) {
  st30_rx_fps_test(
      {
          {ST30_TYPE_RTP_LEVEL, ST30_SAMPLING_96K, ST30_PTIME_1MS, 1, ST30_FMT_PCM16},
          {ST30_TYPE_RTP_LEVEL, ST30_SAMPLING_48K, ST30_PTIME_1MS, 4, ST30_FMT_PCM8},
      },
      ST_TEST_LEVEL_ALL, true);
}
TEST(St30_rx, digest_mix) {
  st30_rx_fps_test(
      {
          {ST30_TYPE_RTP_LEVEL, ST30_SAMPLING_96K, ST30_PTIME_1MS, 1, ST30_FMT_PCM16},
          {ST30_TYPE_FRAME_LEVEL, ST30_SAMPLING_48K, ST30_PTIME_1MS, 4, ST30_FMT_PCM8},
      },
      ST_TEST_LEVEL_MANDATORY, true);
}
TEST(St30_rx, frame_digest_st31_mix) {
  st30_rx_fps_test(
      {
          {ST30_TYPE_FRAME_LEVEL, ST30_SAMPLING_96K, ST30_PTIME_1MS, 2, ST31_FMT_AM824},
          {ST30_TYPE_FRAME_LEVEL, ST30_SAMPLING_48K, ST30_PTIME_1MS, 2, ST31_FMT_AM824},
      },
      ST_TEST_LEVEL_MANDATORY, true);
}
TEST(St30_rx, rtp_digest_st31_mix) {
  st30_rx_fps_test(
      {
          {ST30_TYPE_RTP_LEVEL, ST30_SAMPLING_96K, ST30_PTIME_1MS, 2, ST31_FMT_AM824},
          {ST30_TYPE_RTP_LEVEL, ST30_SAMPLING_48K, ST30_PTIME_1MS, 2, ST31_FMT_AM824},
      },
      ST_TEST_LEVEL_ALL, true);
}
TEST(St30_rx, frame_digest_stereo_ptime_mix_s5) {
  st30_rx_fps_test(
      {
          {ST30_TYPE_FRAME_LEVEL, ST30_SAMPLING_48K, ST30_PTIME_125US, 2, ST30_FMT_PCM16},
          {ST30_TYPE_FRAME_LEVEL, ST30_SAMPLING_48K, ST30_PTIME_250US, 2, ST30_FMT_PCM16},
          {ST30_TYPE_FRAME_LEVEL, ST30_SAMPLING_48K, ST30_PTIME_333US, 2, ST30_FMT_PCM16},
          {ST30_TYPE_FRAME_LEVEL, ST30_SAMPLING_48K, ST30_PTIME_4MS, 2, ST30_FMT_PCM16},
          {ST30_TYPE_FRAME_LEVEL, ST30_SAMPLING_48K, ST31_PTIME_80US, 2, ST31_FMT_AM824},
      },
      ST_TEST_LEVEL_ALL, true);
}
TEST(St30_rx, frame_digest_max_channel_48k_16bit_ptime_mix_s5) {
  st30_rx_fps_test(
      {
          {ST30_TYPE_FRAME_LEVEL, ST30_SAMPLING_48K, ST30_PTIME_125US, 120,
           ST30_FMT_PCM16},
          {ST30_TYPE_FRAME_LEVEL, ST30_SAMPLING_48K, ST30_PTIME_250US, 60,
           ST30_FMT_PCM16},
          {ST30_TYPE_FRAME_LEVEL, ST30_SAMPLING_48K, ST30_PTIME_333US, 45,
           ST30_FMT_PCM16},
          {ST30_TYPE_FRAME_LEVEL, ST30_SAMPLING_48K, ST30_PTIME_1MS, 15, ST30_FMT_PCM16},
          {ST30_TYPE_FRAME_LEVEL, ST30_SAMPLING_48K, ST30_PTIME_4MS, 3, ST30_FMT_PCM16},
      },
      ST_TEST_LEVEL_ALL, true);
}
TEST(St30_rx, frame_digest_max_channel_48k_24bit_ptime_mix_s5) {
  st30_rx_fps_test(
      {
          {ST30_TYPE_FRAME_LEVEL, ST30_SAMPLING_48K, ST30_PTIME_125US, 80,
           ST30_FMT_PCM24},
          {ST30_TYPE_FRAME_LEVEL, ST30_SAMPLING_48K, ST30_PTIME_250US, 40,
           ST30_FMT_PCM24},
          {ST30_TYPE_FRAME_LEVEL, ST30_SAMPLING_48K, ST30_PTIME_333US, 30,
           ST30_FMT_PCM24},
          {ST30_TYPE_FRAME_LEVEL, ST30_SAMPLING_48K, ST30_PTIME_1MS, 10, ST30_FMT_PCM24},
          {ST30_TYPE_FRAME_LEVEL, ST30_SAMPLING_48K, ST30_PTIME_4MS, 2, ST30_FMT_PCM24},
      },
      ST_TEST_LEVEL_MANDATORY, true);
}
TEST(St30_rx, frame_digest_max_channel_96k_24bit_ptime_mix_s5) {
  st30_rx_fps_test(
      {
          {ST30_TYPE_FRAME_LEVEL, ST30_SAMPLING_96K, ST30_PTIME_125US, 40,
           ST30_FMT_PCM24},
          {ST30_TYPE_FRAME_LEVEL, ST30_SAMPLING_96K, ST30_PTIME_250US, 20,
           ST30_FMT_PCM24},
          {ST30_TYPE_FRAME_LEVEL, ST30_SAMPLING_96K, ST30_PTIME_333US, 15,
           ST30_FMT_PCM24},
          {ST30_TYPE_FRAME_LEVEL, ST30_SAMPLING_96K, ST30_PTIME_1MS, 5, ST30_FMT_PCM24},
          {ST30_TYPE_FRAME_LEVEL, ST30_SAMPLING_96K, ST30_PTIME_4MS, 1, ST30_FMT_PCM24},
      },
      ST_TEST_LEVEL_MANDATORY, true);
}

static void st30_rx_update_src_test(enum st30_type type, int tx_sessions,
                                    enum st_test_level level) {
  auto ctx = st_test_ctx();
  auto m_handle = ctx->handle;
  MTL_TEST_REQUIRE_PORTS(ctx, 2);
  MTL_TEST_REQUIRE_LEVEL(ctx, level);
  ASSERT_GE(tx_sessions, 1);

  const bool tx_update_dst = (tx_sessions == 1);
  const int rx_sessions = 1;
  const double expect_framerate = 1000.0;
  const St30SessionSpec spec{type, ST30_SAMPLING_48K, ST30_PTIME_1MS, 2, ST30_FMT_PCM24};

  mtl_test::ScopedMtlStart mtl_guard(m_handle);
  std::vector<St30TxSession> tx;
  std::vector<St30RxSession> rx;
  tx.reserve(tx_sessions);
  rx.reserve(rx_sessions);

  for (int i = 0; i < tx_sessions; i++) {
    tx.emplace_back();
    auto& s = tx.back();
    s.ctx = mtl_test::make_context(ctx, i, 3);

    struct st30_tx_ops ops_tx;
    fill_loopback_tx_ops(&ops_tx, s.ctx.get(), spec,
                         ST30_TEST_UDP_PORT + static_cast<uint16_t>(i * 2));
    if (2 == i) {
      memcpy(ops_tx.dip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_R],
             MTL_IP_ADDR_LEN);
    } else if (1 == i) {
      memcpy(ops_tx.dip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
             MTL_IP_ADDR_LEN);
    } else if (ctx->mcast_only) {
      memcpy(ops_tx.dip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_2],
             MTL_IP_ADDR_LEN);
    }

    s.handle.reset(st30_tx_create(m_handle, &ops_tx));
    ASSERT_TRUE(static_cast<bool>(s.handle));
    __atomic_store_n(&s.ctx->handle, s.handle.get(), __ATOMIC_RELEASE);
    if (type == ST30_TYPE_RTP_LEVEL) {
      s.worker = std::thread(tx_feed_packet, s.ctx.get());
    }
  }

  for (int i = 0; i < rx_sessions; i++) {
    rx.emplace_back();
    auto& s = rx.back();
    s.ctx = mtl_test::make_context(ctx, i, 3);

    struct st30_rx_ops ops_rx;
    fill_loopback_rx_ops(&ops_rx, s.ctx.get(), spec,
                         ST30_TEST_UDP_PORT + static_cast<uint16_t>(i * 2));
    if (ctx->mcast_only) {
      memcpy(ops_rx.ip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_2],
             MTL_IP_ADDR_LEN);
    }

    s.handle.reset(st30_rx_create(m_handle, &ops_rx));
    ASSERT_TRUE(static_cast<bool>(s.handle));
    __atomic_store_n(&s.ctx->handle, s.handle.get(), __ATOMIC_RELEASE);
    if (type == ST30_TYPE_RTP_LEVEL) {
      s.worker = std::thread(rx_get_packet, s.ctx.get());
    }
  }

  EXPECT_GE(mtl_guard.start(), 0);
  auto rx_ctxs = collect_ctxs(rx);

  /* Wait for initial stream establishment (~30ms at 1000 pps). */
  mtl_test::wait_for_all(rx_ctxs, mtl_test::rx_frames, kSt30PhaseTargetFrames,
                         kSt30PhaseTimeoutMs);

  auto verify_switched_source = [&](const char* label, const uint8_t* src_ip,
                                    uint16_t udp_port) {
    struct st_rx_source_info src;
    memset(&src, 0, sizeof(src));
    src.udp_port[MTL_SESSION_PORT_P] = udp_port;
    memcpy(src.ip_addr[MTL_SESSION_PORT_P], src_ip, MTL_IP_ADDR_LEN);

    std::vector<int> baselines(rx.size(), 0);
    for (size_t i = 0; i < rx.size(); i++) {
      EXPECT_GE(st30_rx_update_source(rx[i].handle.get(), &src), 0);
      baselines[i] = mtl_test::rx_frames(rx[i].ctx.get());
    }

    auto delta_frames = [&](const tests_context* c) {
      return mtl_test::rx_frames(c) - baselines[c->idx];
    };
    auto rate_res =
        mtl_test::measure_rate_window(rx_ctxs, delta_frames, kSt30RateWarmupFrames,
                                      kSt30RateMinDeltaFrames, kSt30RateMinWindowMs);
    for (size_t i = 0; i < rx.size(); i++) {
      const int fb_rec = mtl_test::rx_frames(rx[i].ctx.get());
      EXPECT_GT(fb_rec, baselines[i]);
      info("%s, session %zu fb_rec %d framerate %f for %s\n", __func__, i,
           fb_rec - baselines[i], rate_res.rate[i], label);
      EXPECT_NEAR(rate_res.rate[i], expect_framerate, expect_framerate * 0.1);
    }
  };

  /* Switch to mcast port P (tx_session:1). */
  if (tx_update_dst) {
    __atomic_store_n(&tx[0].ctx->seq_id, 0, __ATOMIC_RELAXED);
    struct st_tx_dest_info dst;
    memset(&dst, 0, sizeof(dst));
    dst.udp_port[MTL_SESSION_PORT_P] = ST30_TEST_UDP_PORT + 2;
    memcpy(dst.dip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
           MTL_IP_ADDR_LEN);
    EXPECT_GE(st30_tx_update_destination(tx[0].handle.get(), &dst), 0);
  } else {
    __atomic_store_n(&tx[1].ctx->seq_id, 0, __ATOMIC_RELAXED);
  }
  verify_switched_source("mcast 1", ctx->mcast_ip_addr[MTL_PORT_P],
                         ST30_TEST_UDP_PORT + 2);

  /* Switch to mcast port R (tx_session:2). */
  if (tx_sessions > 2) {
    __atomic_store_n(&tx[2].ctx->seq_id, rand(), __ATOMIC_RELAXED);
    verify_switched_source("mcast 2", ctx->mcast_ip_addr[MTL_PORT_R],
                           ST30_TEST_UDP_PORT + 4);
  }

  /* Switch to unicast (tx_session:0). */
  __atomic_store_n(&tx[0].ctx->seq_id, rand(), __ATOMIC_RELAXED);
  if (tx_update_dst) {
    struct st_tx_dest_info dst;
    memset(&dst, 0, sizeof(dst));
    dst.udp_port[MTL_SESSION_PORT_P] = ST30_TEST_UDP_PORT;
    memcpy(dst.dip_addr[MTL_SESSION_PORT_P], ctx->para.sip_addr[MTL_PORT_R],
           MTL_IP_ADDR_LEN);
    EXPECT_GE(st30_tx_update_destination(tx[0].handle.get(), &dst), 0);
  }
  verify_switched_source("unicast 0", ctx->para.sip_addr[MTL_PORT_P], ST30_TEST_UDP_PORT);

  for (auto& s : rx) s.stop();
  for (auto& s : tx) s.stop();
  EXPECT_GE(mtl_guard.stop(), 0);
}

TEST(St30_rx, update_source_frame) {
  st30_rx_update_src_test(ST30_TYPE_FRAME_LEVEL, 3, ST_TEST_LEVEL_ALL);
}
TEST(St30_rx, update_source_rtp) {
  st30_rx_update_src_test(ST30_TYPE_RTP_LEVEL, 2, ST_TEST_LEVEL_ALL);
}
TEST(St30_tx, update_dest_frame) {
  st30_rx_update_src_test(ST30_TYPE_FRAME_LEVEL, 1, ST_TEST_LEVEL_ALL);
}
TEST(St30_tx, update_dest_rtp) {
  st30_rx_update_src_test(ST30_TYPE_RTP_LEVEL, 1, ST_TEST_LEVEL_ALL);
}

static void st30_rx_meta_test(const std::vector<St30SessionSpec>& specs,
                              enum st_test_level level, bool user_timestamp = false) {
  auto ctx = st_test_ctx();
  auto m_handle = ctx->handle;
  MTL_TEST_REQUIRE_PORTS(ctx, 2);
  MTL_TEST_REQUIRE_LEVEL(ctx, level);

  const size_t sessions = specs.size();
  const double expect_framerate = user_timestamp ? 500.0 : 1000.0;

  mtl_test::ScopedMtlStart mtl_guard(m_handle);
  std::vector<St30TxSession> tx;
  std::vector<St30RxSession> rx;
  tx.reserve(sessions);
  rx.reserve(sessions);

  for (size_t i = 0; i < sessions; i++) {
    const auto& spec = specs[i];
    const uint16_t udp_port = ST30_TEST_UDP_PORT + static_cast<uint16_t>(i * 2);

    tx.emplace_back();
    auto& s = tx.back();
    s.ctx = mtl_test::make_context(ctx, static_cast<int>(i), 3);

    struct st30_tx_ops ops_tx;
    fill_loopback_tx_ops(&ops_tx, s.ctx.get(), spec, udp_port, 0, "st30_meta_test");
    if (user_timestamp) {
      ops_tx.get_next_frame = tx_audio_next_frame_timestamp;
      ops_tx.flags |= ST30_TX_FLAG_USER_PACING;
    }
    s.ctx->frame_time = st30_get_packet_time(ops_tx.ptime);

    s.handle.reset(st30_tx_create(m_handle, &ops_tx));
    ASSERT_TRUE(static_cast<bool>(s.handle));
    __atomic_store_n(&s.ctx->handle, s.handle.get(), __ATOMIC_RELEASE);
  }

  for (size_t i = 0; i < sessions; i++) {
    const auto& spec = specs[i];
    const uint16_t udp_port = ST30_TEST_UDP_PORT + static_cast<uint16_t>(i * 2);

    rx.emplace_back();
    auto& s = rx.back();
    s.ctx = mtl_test::make_context(ctx, static_cast<int>(i), 3);

    struct st30_rx_ops ops_rx;
    fill_loopback_rx_ops(&ops_rx, s.ctx.get(), spec, udp_port, 0, "st30_meta_test");
    ops_rx.notify_frame_ready = st30_rx_meta_frame_ready;

    s.handle.reset(st30_rx_create(m_handle, &ops_rx));
    ASSERT_TRUE(static_cast<bool>(s.handle));
    s.ctx->frame_size = tx[i].ctx->frame_size;

    auto meta = static_cast<struct st30_rx_frame_meta*>(
        st_test_zmalloc(sizeof(struct st30_rx_frame_meta)));
    ASSERT_TRUE(meta != nullptr);
    meta->channel = ops_rx.channel;
    meta->sampling = ops_rx.sampling;
    meta->fmt = ops_rx.fmt;
    s.ctx->priv = meta; /* Owned and freed by ScopedContext. */
    __atomic_store_n(&s.ctx->handle, s.handle.get(), __ATOMIC_RELEASE);
  }

  EXPECT_GE(mtl_guard.start(), 0);
  auto rx_ctxs = collect_ctxs(rx);
  auto rate_res =
      mtl_test::measure_rate_window(rx_ctxs, mtl_test::rx_frames, kSt30RateWarmupFrames,
                                    kSt30RateMinDeltaFrames, kSt30RateMinWindowMs);

  for (auto& s : tx) s.stop();
  for (auto& s : rx) s.stop();
  EXPECT_GE(mtl_guard.stop(), 0);

  for (size_t i = 0; i < sessions; i++) {
    const int fb_rec = mtl_test::rx_frames(rx[i].ctx.get());
    const int fb_send = mtl_test::tx_frames(tx[i].ctx.get());
    EXPECT_GT(fb_rec, 0);
    info("%s, session %zu fb_rec %d fail %d framerate %f, fb send %d\n", __func__, i,
         fb_rec, rx[i].ctx->rx_meta_fail_cnt, rate_res.rate[i], fb_send);
    EXPECT_LE(rx[i].ctx->rx_meta_fail_cnt, 2);
    EXPECT_NEAR(rate_res.rate[i], expect_framerate, expect_framerate * 0.1);
  }
}

TEST(St30_rx, frame_meta_pcm16_48k_2ch_s1) {
  st30_rx_meta_test(
      {{ST30_TYPE_FRAME_LEVEL, ST30_SAMPLING_48K, ST30_PTIME_1MS, 2, ST30_FMT_PCM16}},
      ST_TEST_LEVEL_ALL);
}

TEST(St30_rx, frame_user_timestamp) {
  st30_rx_meta_test(
      {{ST30_TYPE_FRAME_LEVEL, ST30_SAMPLING_48K, ST30_PTIME_1MS, 2, ST30_FMT_PCM16}},
      ST_TEST_LEVEL_MANDATORY, true);
}

static void st30_create_after_start_test(const std::vector<St30SessionSpec>& specs,
                                         int repeat, enum st_test_level level) {
  auto ctx = st_test_ctx();
  auto m_handle = ctx->handle;
  MTL_TEST_REQUIRE_PORTS(ctx, 2);
  MTL_TEST_REQUIRE_LEVEL(ctx, level);

  const size_t sessions = specs.size();
  const double expect_framerate = 1000.0;

  mtl_test::ScopedMtlStart mtl_guard(m_handle);
  EXPECT_GE(mtl_guard.start(), 0);

  for (int r = 0; r < repeat; r++) {
    std::vector<St30TxSession> tx;
    std::vector<St30RxSession> rx;
    tx.reserve(sessions);
    rx.reserve(sessions);

    for (size_t i = 0; i < sessions; i++) {
      const auto& spec = specs[i];
      const uint16_t udp_port = ST30_TEST_UDP_PORT + static_cast<uint16_t>(i * 2);

      tx.emplace_back();
      auto& s = tx.back();
      s.ctx = mtl_test::make_context(ctx, static_cast<int>(i), 3);

      struct st30_tx_ops ops_tx;
      fill_loopback_tx_ops(&ops_tx, s.ctx.get(), spec, udp_port);
      s.handle.reset(st30_tx_create(m_handle, &ops_tx));
      ASSERT_TRUE(static_cast<bool>(s.handle));
      __atomic_store_n(&s.ctx->handle, s.handle.get(), __ATOMIC_RELEASE);
      if (spec.type == ST30_TYPE_RTP_LEVEL) {
        s.worker = std::thread(tx_feed_packet, s.ctx.get());
      }
    }

    for (size_t i = 0; i < sessions; i++) {
      const auto& spec = specs[i];
      const uint16_t udp_port = ST30_TEST_UDP_PORT + static_cast<uint16_t>(i * 2);

      rx.emplace_back();
      auto& s = rx.back();
      s.ctx = mtl_test::make_context(ctx, static_cast<int>(i), 3);

      struct st30_rx_ops ops_rx;
      fill_loopback_rx_ops(&ops_rx, s.ctx.get(), spec, udp_port);
      s.handle.reset(st30_rx_create(m_handle, &ops_rx));
      ASSERT_TRUE(static_cast<bool>(s.handle));
      __atomic_store_n(&s.ctx->handle, s.handle.get(), __ATOMIC_RELEASE);
      if (spec.type == ST30_TYPE_RTP_LEVEL) {
        s.worker = std::thread(rx_get_packet, s.ctx.get());
      }
    }

    auto rx_ctxs = collect_ctxs(rx);
    auto rate_res =
        mtl_test::measure_rate_window(rx_ctxs, mtl_test::rx_frames, kSt30RateWarmupFrames,
                                      kSt30RateMinDeltaFrames, kSt30RateMinWindowMs);

    for (auto& s : tx) s.stop();
    for (auto& s : rx) s.stop();

    for (size_t i = 0; i < sessions; i++) {
      const int fb_rec = mtl_test::rx_frames(rx[i].ctx.get());
      EXPECT_GT(fb_rec, 0);
      info("%s, session %zu fb_rec %d framerate %f\n", __func__, i, fb_rec,
           rate_res.rate[i]);
      EXPECT_NEAR(rate_res.rate[i], expect_framerate, expect_framerate * 0.1);
    }
  }

  EXPECT_GE(mtl_guard.stop(), 0);
}

TEST(St30_rx, after_start_mix_s2_r1) {
  st30_create_after_start_test(
      {
          {ST30_TYPE_FRAME_LEVEL, ST30_SAMPLING_96K, ST30_PTIME_1MS, 1, ST30_FMT_PCM24},
          {ST30_TYPE_RTP_LEVEL, ST30_SAMPLING_48K, ST30_PTIME_1MS, 2, ST30_FMT_PCM16},
      },
      1, ST_TEST_LEVEL_MANDATORY);
}

TEST(St30_rx, after_start_frame_s1_r2) {
  st30_create_after_start_test(
      {
          {ST30_TYPE_FRAME_LEVEL, ST30_SAMPLING_96K, ST30_PTIME_1MS, 2, ST30_FMT_PCM16},
      },
      2, ST_TEST_LEVEL_ALL);
}
