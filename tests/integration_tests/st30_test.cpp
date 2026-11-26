/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include <algorithm>
#include <atomic>
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
    __atomic_fetch_add(&ctx->sha_fail_cnt, 1, __ATOMIC_RELAXED);
  }
  __atomic_fetch_add(&ctx->check_sha_frame_cnt, 1, __ATOMIC_RELAXED);
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

  /* rx_audio_session_put_frame unconditionally returns -EIO
   * (lib/src/st2110/st_rx_audio_session.c:91). */
  (void)st30_rx_put_framebuff(handle, frame);
  __atomic_fetch_add(&ctx->fb_rec, 1, __ATOMIC_RELAXED);
  return 0;
}

static int st30_rx_meta_frame_ready(void* priv, void* frame,
                                    struct st30_rx_frame_meta* meta) {
  auto ctx = static_cast<tests_context*>(priv);
  auto handle =
      static_cast<st30_rx_handle>(__atomic_load_n(&ctx->handle, __ATOMIC_ACQUIRE));

  if (!handle) return -EIO;

  auto expect_meta = static_cast<struct st30_rx_frame_meta*>(ctx->priv);
  if (!expect_meta || !meta) {
    __atomic_fetch_add(&ctx->rx_meta_fail_cnt, 1, __ATOMIC_RELAXED);
    /* rx_audio_session_put_frame unconditionally returns -EIO
     * (lib/src/st2110/st_rx_audio_session.c:91). */
    (void)st30_rx_put_framebuff(handle, frame);
    return 0;
  }

  const int prev_rec = __atomic_fetch_add(&ctx->fb_rec, 1, __ATOMIC_RELAXED);
  if (expect_meta->sampling != meta->sampling) {
    __atomic_fetch_add(&ctx->rx_meta_fail_cnt, 1, __ATOMIC_RELAXED);
  }
  if (expect_meta->channel != meta->channel) {
    __atomic_fetch_add(&ctx->rx_meta_fail_cnt, 1, __ATOMIC_RELAXED);
  }
  if (expect_meta->fmt != meta->fmt) {
    __atomic_fetch_add(&ctx->rx_meta_fail_cnt, 1, __ATOMIC_RELAXED);
  }
  if (meta->status != ST_FRAME_STATUS_COMPLETE) {
    __atomic_fetch_add(&ctx->rx_meta_fail_cnt, 1, __ATOMIC_RELAXED);
  }
  if (meta->frame_recv_size != ctx->frame_size) {
    __atomic_fetch_add(&ctx->rx_meta_fail_cnt, 1, __ATOMIC_RELAXED);
  }
  if (prev_rec > 0 && expect_meta->timestamp == meta->timestamp) {
    __atomic_fetch_add(&ctx->rx_meta_fail_cnt, 1, __ATOMIC_RELAXED);
  }
  expect_meta->timestamp = meta->timestamp;

  /* rx_audio_session_put_frame unconditionally returns -EIO
   * (lib/src/st2110/st_rx_audio_session.c:91). */
  (void)st30_rx_put_framebuff(handle, frame);
  return 0;
}

struct St30TpState {
  tests_context* ctx = nullptr;
  std::atomic<st30_rx_handle> rx_handle{nullptr};
  std::atomic<int> cb_cnt{0};
  std::atomic<int> bad_port_cnt{0};
  std::atomic<int> null_tp_cnt{0};
  std::atomic<int> zero_pkts_cnt{0};
};

static int st30_rx_tp_frame_ready(void* priv, void* frame,
                                  struct st30_rx_frame_meta* /*meta*/) {
  auto* state = static_cast<St30TpState*>(priv);
  if (!state) return -EIO;
  auto handle = state->rx_handle.load(std::memory_order_acquire);
  if (!handle) return -EIO;

  /* rx_audio_session_put_frame unconditionally returns -EIO
   * (lib/src/st2110/st_rx_audio_session.c:91). */
  (void)st30_rx_put_framebuff(handle, frame);
  __atomic_fetch_add(&state->ctx->fb_rec, 1, __ATOMIC_RELAXED);
  return 0;
}

static int st30_rx_tp_notify(void* priv, enum mtl_session_port port,
                             struct st30_rx_tp_meta* tp) {
  auto* state = static_cast<St30TpState*>(priv);
  if (!state) return -EIO;
  if (port != MTL_SESSION_PORT_P) {
    state->bad_port_cnt.fetch_add(1, std::memory_order_relaxed);
  }
  if (!tp) {
    state->null_tp_cnt.fetch_add(1, std::memory_order_relaxed);
  } else if (tp->pkts_cnt == 0) {
    state->zero_pkts_cnt.fetch_add(1, std::memory_order_relaxed);
  }
  state->cb_cnt.fetch_add(1, std::memory_order_relaxed);
  return 0;
}

static void st30_rx_ops_init(tests_context* st30, struct st30_rx_ops* ops) {
  auto ctx = st30->ctx;

  memset(ops, 0, sizeof(*ops));
  ops->name = "st30_test";
  ops->priv = st30;
  ops->num_port = ctx->same_dual_port ? 1
                                      : static_cast<uint8_t>(std::min<int>(
                                            ctx->para.num_ports, MTL_SESSION_PORT_MAX));
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
  ops->num_port = ctx->same_dual_port ? 1
                                      : static_cast<uint8_t>(std::min<int>(
                                            ctx->para.num_ports, MTL_SESSION_PORT_MAX));
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

namespace {

void st30_tx_verify_create_free(int base, int step, int repeat) {
  mtl_test::verify_create_free<struct st30_tx_ops, st30_tx_handle, st30_tx_create,
                               st30_tx_free>(st30_tx_ops_init, st30_tx_assert_cnt, base,
                                             step, repeat);
}

void st30_rx_verify_create_free(int base, int step, int repeat) {
  mtl_test::verify_create_free<struct st30_rx_ops, st30_rx_handle, st30_rx_create,
                               st30_rx_free>(st30_rx_ops_init, st30_rx_assert_cnt, base,
                                             step, repeat);
}

}  // namespace

TEST(St30_tx, create_free_single) {
  st30_tx_verify_create_free(0, 1, 1);
}
TEST(St30_tx, create_free_multi) {
  st30_tx_verify_create_free(0, 1, 6);
}
TEST(St30_tx, create_free_mix) {
  st30_tx_verify_create_free(2, 3, 4);
}
TEST(St30_tx, create_free_max) {
  mtl_test::verify_create_free_max<struct st30_tx_ops, st30_tx_handle, st30_tx_create,
                                   st30_tx_free>(st30_tx_ops_init, st30_tx_assert_cnt,
                                                 TEST_CREATE_FREE_MAX);
}
TEST(St30_tx, create_expect_fail) {
  mtl_test::verify_invalid_ports<struct st30_tx_ops, st30_tx_handle, st30_tx_create,
                                 st30_tx_free>(st30_tx_ops_init);
}
TEST(St30_tx, create_expect_fail_ring_sz) {
  for (uint16_t ring_size : {static_cast<uint16_t>(0), static_cast<uint16_t>(128 + 1)}) {
    mtl_test::verify_rtp_ring_invalid<struct st30_tx_ops, st30_tx_handle, st30_tx_create,
                                      st30_tx_free>(st30_tx_ops_init, ST30_TYPE_RTP_LEVEL,
                                                    ring_size);
  }
}
TEST(St30_tx, get_framebuffer) {
  for (uint16_t fbcnt : {static_cast<uint16_t>(3), static_cast<uint16_t>(1000)}) {
    mtl_test::verify_get_framebuffer<struct st30_tx_ops, st30_tx_handle, st30_tx_create,
                                     st30_tx_free>(st30_tx_ops_init,
                                                   st30_tx_get_framebuffer, fbcnt);
  }
}
TEST(St30_tx, get_framebuffer_expect_fail) {
  for (uint16_t fbcnt : {static_cast<uint16_t>(3), static_cast<uint16_t>(1000)}) {
    mtl_test::verify_get_framebuffer_oob<struct st30_tx_ops, st30_tx_handle,
                                         st30_tx_create, st30_tx_free>(
        st30_tx_ops_init, st30_tx_get_framebuffer, fbcnt);
  }
}

TEST(St30_rx, create_free_single) {
  st30_rx_verify_create_free(0, 1, 1);
}
TEST(St30_rx, create_free_multi) {
  st30_rx_verify_create_free(0, 1, 6);
}
TEST(St30_rx, create_free_mix) {
  st30_rx_verify_create_free(2, 3, 4);
}
TEST(St30_rx, create_free_max) {
  mtl_test::verify_create_free_max<struct st30_rx_ops, st30_rx_handle, st30_rx_create,
                                   st30_rx_free>(st30_rx_ops_init, st30_rx_assert_cnt,
                                                 TEST_CREATE_FREE_MAX);
}
TEST(St30_rx, create_expect_fail) {
  mtl_test::verify_invalid_ports<struct st30_rx_ops, st30_rx_handle, st30_rx_create,
                                 st30_rx_free>(st30_rx_ops_init);
}
TEST(St30_rx, create_expect_fail_ring_sz) {
  for (uint16_t ring_size : {static_cast<uint16_t>(0), static_cast<uint16_t>(128 + 1)}) {
    mtl_test::verify_rtp_ring_invalid<struct st30_rx_ops, st30_rx_handle, st30_rx_create,
                                      st30_rx_free>(st30_rx_ops_init, ST30_TYPE_RTP_LEVEL,
                                                    ring_size);
  }
}

namespace {

struct St30SessionSpec {
  enum st30_type type = ST30_TYPE_FRAME_LEVEL;
  enum st30_sampling sampling = ST30_SAMPLING_48K;
  enum st30_ptime ptime = ST30_PTIME_1MS;
  uint16_t channel = 2;
  enum st30_fmt fmt = ST30_FMT_PCM16;
};

using ScopedSt30Tx = mtl_test::ScopedHandle<st30_tx_handle, st30_tx_free>;
using ScopedSt30Rx = mtl_test::ScopedHandle<st30_rx_handle, st30_rx_free>;

double st30_expected_fps(enum st30_ptime ptime) {
  double pkt_time_ns = st30_get_packet_time(ptime);
  return (pkt_time_ns > 0.0) ? (static_cast<double>(NS_PER_S) / pkt_time_ns) : 1000.0;
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

static void st30_tx_fps_test(const std::vector<St30SessionSpec>& specs) {
  auto ctx = st_test_ctx();
  auto m_handle = ctx->handle;

  const size_t sessions = specs.size();
  std::vector<mtl_test::ScopedContext> tx_ctxs;
  std::vector<tests_context*> raw_tx;
  std::vector<ScopedSt30Tx> tx_handles;
  mtl_test::ScopedMtlStart mtl_guard(m_handle);
  std::vector<mtl_test::ScopedWorker> tx_workers;
  tx_ctxs.reserve(sessions);
  raw_tx.reserve(sessions);
  tx_handles.reserve(sessions);
  tx_workers.reserve(sessions);
  std::vector<double> expect_framerate(sessions, 0.0);

  for (size_t i = 0; i < sessions; i++) {
    const auto& spec = specs[i];
    expect_framerate[i] = st30_expected_fps(spec.ptime);

    tx_ctxs.push_back(mtl_test::make_context(ctx, static_cast<int>(i), 3));
    raw_tx.push_back(tx_ctxs.back().get());

    struct st30_tx_ops ops;
    st30_tx_ops_init(raw_tx[i], &ops);
    ops.type = spec.type;
    ops.sampling = spec.sampling;
    ops.channel = spec.channel;
    ops.fmt = spec.fmt;
    ops.payload_type = ST30_TEST_PAYLOAD_TYPE;
    ops.ptime = spec.ptime;
    const int pkt_size =
        st30_get_packet_size(ops.fmt, ops.ptime, ops.sampling, ops.channel);
    ASSERT_GT(pkt_size, 0);
    ops.framebuff_size = static_cast<uint32_t>(pkt_size);
    raw_tx[i]->pkt_data_len = pkt_size;

    ScopedSt30Tx handle(st30_tx_create(m_handle, &ops));
    ASSERT_TRUE(static_cast<bool>(handle));
    __atomic_store_n(&raw_tx[i]->handle, handle.get(), __ATOMIC_RELEASE);
    tx_handles.push_back(std::move(handle));

    if (spec.type == ST30_TYPE_RTP_LEVEL) {
      tx_workers.emplace_back(raw_tx[i], std::thread(tx_feed_packet, raw_tx[i]));
    }
  }

  EXPECT_GE(mtl_guard.start(), 0);
  auto rate_res =
      mtl_test::measure_rate_window(raw_tx, mtl_test::tx_frames, kSt30RateWarmupFrames,
                                    kSt30RateMinDeltaFrames, kSt30RateMinWindowMs);
  EXPECT_TRUE(rate_res.warmed_up);
  EXPECT_TRUE(rate_res.complete);

  for (auto& w : tx_workers) w.stop();
  EXPECT_GE(mtl_guard.stop(), 0);

  for (size_t i = 0; i < sessions; i++) {
    const int fb_send = mtl_test::tx_frames(raw_tx[i]);
    EXPECT_GT(fb_send, 0);
    info("%s, session %zu fb_send %d framerate %f\n", __func__, i, fb_send,
         rate_res.rate[i]);
    EXPECT_NEAR(rate_res.rate[i], expect_framerate[i], expect_framerate[i] * 0.1);
  }
}

static void st30_rx_fps_test(const std::vector<St30SessionSpec>& specs,
                             bool check_sha = false) {
  auto ctx = st_test_ctx();
  auto m_handle = ctx->handle;
  MTL_TEST_REQUIRE_PORTS(ctx, 2);

  const size_t sessions = specs.size();
  std::vector<mtl_test::ScopedContext> tx_ctxs, rx_ctxs;
  std::vector<tests_context*> raw_tx, raw_rx;
  std::vector<ScopedSt30Tx> tx_handles;
  std::vector<ScopedSt30Rx> rx_handles;
  mtl_test::ScopedMtlStart mtl_guard(m_handle);
  std::vector<mtl_test::ScopedWorker> tx_workers, rx_workers;
  tx_ctxs.reserve(sessions);
  rx_ctxs.reserve(sessions);
  raw_tx.reserve(sessions);
  raw_rx.reserve(sessions);
  tx_handles.reserve(sessions);
  rx_handles.reserve(sessions);
  tx_workers.reserve(sessions);
  rx_workers.reserve(sessions);
  std::vector<double> expect_framerate(sessions, 0.0);

  for (size_t i = 0; i < sessions; i++) {
    const auto& spec = specs[i];
    const uint16_t udp_port = ST30_TEST_UDP_PORT + static_cast<uint16_t>(i * 2);
    const uint32_t ssrc = i ? static_cast<uint32_t>(i) + 0x66666666u : 0u;

    const uint16_t fb_cnt = check_sha ? TEST_SHA_HIST_NUM : 3;
    tx_ctxs.push_back(
        mtl_test::make_context(ctx, static_cast<int>(i), fb_cnt, check_sha));
    raw_tx.push_back(tx_ctxs.back().get());

    struct st30_tx_ops ops_tx;
    fill_loopback_tx_ops(&ops_tx, raw_tx[i], spec, udp_port, ssrc);
    ScopedSt30Tx handle(st30_tx_create(m_handle, &ops_tx));
    ASSERT_TRUE(static_cast<bool>(handle));

    if (check_sha) {
      for (int frame = 0; frame < TEST_SHA_HIST_NUM; frame++) {
        uint8_t* fb = nullptr;
        if (spec.type == ST30_TYPE_FRAME_LEVEL) {
          fb = static_cast<uint8_t*>(st30_tx_get_framebuffer(handle.get(), frame));
        } else {
          raw_tx[i]->frame_buf[frame] =
              static_cast<uint8_t*>(st_test_zmalloc(ops_tx.framebuff_size));
          fb = raw_tx[i]->frame_buf[frame];
        }
        ASSERT_TRUE(fb != nullptr);
        st_test_rand_data(fb, ops_tx.framebuff_size, frame);
        unsigned char* result = raw_tx[i]->shas[frame];
        SHA256(fb, ops_tx.framebuff_size, result);
        test_sha_dump("st30_rx", result);
      }
    }

    __atomic_store_n(&raw_tx[i]->handle, handle.get(), __ATOMIC_RELEASE);
    tx_handles.push_back(std::move(handle));
    if (spec.type == ST30_TYPE_RTP_LEVEL) {
      tx_workers.emplace_back(raw_tx[i], std::thread(tx_feed_packet, raw_tx[i]));
    }
  }

  for (size_t i = 0; i < sessions; i++) {
    const auto& spec = specs[i];
    const uint16_t udp_port = ST30_TEST_UDP_PORT + static_cast<uint16_t>(i * 2);
    const uint32_t ssrc = i ? static_cast<uint32_t>(i) + 0x66666666u : 0u;

    rx_ctxs.push_back(mtl_test::make_context(ctx, static_cast<int>(i), 3, check_sha));
    raw_rx.push_back(rx_ctxs.back().get());

    struct st30_rx_ops ops_rx;
    fill_loopback_rx_ops(&ops_rx, raw_rx[i], spec, udp_port, ssrc);
    expect_framerate[i] = st30_expected_fps(ops_rx.ptime);

    ScopedSt30Rx handle(st30_rx_create(m_handle, &ops_rx));
    ASSERT_TRUE(static_cast<bool>(handle));

    raw_rx[i]->frame_size = ops_rx.framebuff_size;
    if (check_sha) {
      memcpy(raw_rx[i]->shas, raw_tx[i]->shas, TEST_SHA_HIST_NUM * SHA256_DIGEST_LENGTH);
    }
    __atomic_store_n(&raw_rx[i]->handle, handle.get(), __ATOMIC_RELEASE);

    struct st_queue_meta q_meta;
    memset(&q_meta, 0, sizeof(q_meta));
    EXPECT_GE(st30_rx_get_queue_meta(handle.get(), &q_meta), 0);
    EXPECT_EQ(q_meta.num_port, 1);

    rx_handles.push_back(std::move(handle));
    if (spec.type == ST30_TYPE_RTP_LEVEL) {
      rx_workers.emplace_back(raw_rx[i], std::thread(rx_get_packet, raw_rx[i]));
    }
  }

  EXPECT_GE(mtl_guard.start(), 0);
  auto rate_res =
      mtl_test::measure_rate_window(raw_rx, mtl_test::rx_frames, kSt30RateWarmupFrames,
                                    kSt30RateMinDeltaFrames, kSt30RateMinWindowMs);
  EXPECT_TRUE(rate_res.warmed_up);
  EXPECT_TRUE(rate_res.complete);

  for (auto& w : tx_workers) w.stop();
  for (auto& w : rx_workers) w.stop();
  EXPECT_GE(mtl_guard.stop(), 0);

  for (size_t i = 0; i < sessions; i++) {
    const int fb_rec = mtl_test::rx_frames(raw_rx[i]);
    EXPECT_GT(fb_rec, 0);
    EXPECT_LE(mtl_test::relaxed_load(&raw_rx[i]->sha_fail_cnt), 2);
    if (check_sha) {
      EXPECT_GT(mtl_test::sha_checked_frames(raw_rx[i]), 0);
    }
    info("%s, session %zu fb_rec %d framerate %f\n", __func__, i, fb_rec,
         rate_res.rate[i]);
    EXPECT_NEAR(rate_res.rate[i], expect_framerate[i], expect_framerate[i] * 0.1);
  }
}

TEST(St30_tx, mix_48k_96_mix) {
  st30_tx_fps_test({
      {ST30_TYPE_FRAME_LEVEL, ST30_SAMPLING_96K, ST30_PTIME_1MS, 2, ST30_FMT_PCM8},
      {ST30_TYPE_RTP_LEVEL, ST30_SAMPLING_48K, ST30_PTIME_1MS, 1, ST30_FMT_PCM16},
      {ST30_TYPE_RTP_LEVEL, ST30_SAMPLING_48K, ST30_PTIME_1MS, 4, ST30_FMT_PCM24},
  });
}
TEST(St30_rx, digest_mix) {
  st30_rx_fps_test(
      {
          {ST30_TYPE_RTP_LEVEL, ST30_SAMPLING_96K, ST30_PTIME_1MS, 1, ST30_FMT_PCM16},
          {ST30_TYPE_FRAME_LEVEL, ST30_SAMPLING_48K, ST30_PTIME_1MS, 4, ST30_FMT_PCM8},
      },
      true);
}
TEST(St30_rx, frame_digest_st31_mix) {
  st30_rx_fps_test(
      {
          {ST30_TYPE_FRAME_LEVEL, ST30_SAMPLING_96K, ST30_PTIME_1MS, 2, ST31_FMT_AM824},
          {ST30_TYPE_FRAME_LEVEL, ST30_SAMPLING_48K, ST30_PTIME_1MS, 2, ST31_FMT_AM824},
      },
      true);
}
TEST(St30_rx, rtp_digest_st31_mix) {
  st30_rx_fps_test(
      {
          {ST30_TYPE_RTP_LEVEL, ST30_SAMPLING_96K, ST30_PTIME_1MS, 2, ST31_FMT_AM824},
          {ST30_TYPE_RTP_LEVEL, ST30_SAMPLING_48K, ST30_PTIME_1MS, 2, ST31_FMT_AM824},
      },
      true);
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
      true);
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
      true);
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
      true);
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
      true);
}

static void st30_rx_update_src_test(enum st30_type type, int tx_sessions) {
  auto ctx = st_test_ctx();
  auto m_handle = ctx->handle;
  MTL_TEST_REQUIRE_PORTS(ctx, 2);
  ASSERT_GE(tx_sessions, 1);

  const bool tx_update_dst = (tx_sessions == 1);
  const int rx_sessions = 1;
  const double expect_framerate = 1000.0;
  const St30SessionSpec spec{type, ST30_SAMPLING_48K, ST30_PTIME_1MS, 2, ST30_FMT_PCM24};

  std::vector<mtl_test::ScopedContext> tx_ctxs, rx_ctxs;
  std::vector<tests_context*> raw_tx, raw_rx;
  std::vector<ScopedSt30Tx> tx_handles;
  std::vector<ScopedSt30Rx> rx_handles;
  mtl_test::ScopedMtlStart mtl_guard(m_handle);
  std::vector<mtl_test::ScopedWorker> tx_workers, rx_workers;
  tx_ctxs.reserve(tx_sessions);
  rx_ctxs.reserve(rx_sessions);
  raw_tx.reserve(tx_sessions);
  raw_rx.reserve(rx_sessions);
  tx_handles.reserve(tx_sessions);
  rx_handles.reserve(rx_sessions);
  tx_workers.reserve(tx_sessions);
  rx_workers.reserve(rx_sessions);

  for (int i = 0; i < tx_sessions; i++) {
    tx_ctxs.push_back(mtl_test::make_context(ctx, i, 3));
    raw_tx.push_back(tx_ctxs.back().get());

    struct st30_tx_ops ops_tx;
    fill_loopback_tx_ops(&ops_tx, raw_tx[i], spec,
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

    ScopedSt30Tx handle(st30_tx_create(m_handle, &ops_tx));
    ASSERT_TRUE(static_cast<bool>(handle));
    __atomic_store_n(&raw_tx[i]->handle, handle.get(), __ATOMIC_RELEASE);
    tx_handles.push_back(std::move(handle));
    if (type == ST30_TYPE_RTP_LEVEL) {
      tx_workers.emplace_back(raw_tx[i], std::thread(tx_feed_packet, raw_tx[i]));
    }
  }

  for (int i = 0; i < rx_sessions; i++) {
    rx_ctxs.push_back(mtl_test::make_context(ctx, i, 3));
    raw_rx.push_back(rx_ctxs.back().get());

    struct st30_rx_ops ops_rx;
    fill_loopback_rx_ops(&ops_rx, raw_rx[i], spec,
                         ST30_TEST_UDP_PORT + static_cast<uint16_t>(i * 2));
    if (ctx->mcast_only) {
      memcpy(ops_rx.ip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_2],
             MTL_IP_ADDR_LEN);
    }

    ScopedSt30Rx handle(st30_rx_create(m_handle, &ops_rx));
    ASSERT_TRUE(static_cast<bool>(handle));
    __atomic_store_n(&raw_rx[i]->handle, handle.get(), __ATOMIC_RELEASE);
    rx_handles.push_back(std::move(handle));
    if (type == ST30_TYPE_RTP_LEVEL) {
      rx_workers.emplace_back(raw_rx[i], std::thread(rx_get_packet, raw_rx[i]));
    }
  }

  EXPECT_GE(mtl_guard.start(), 0);

  /* Wait for initial stream establishment (~30ms at 1000 pps). */
  EXPECT_TRUE(mtl_test::wait_for_all(raw_rx, mtl_test::rx_frames, kSt30PhaseTargetFrames,
                                     kSt30PhaseTimeoutMs));

  auto verify_switched_source = [&](const char* label, const uint8_t* src_ip,
                                    uint16_t udp_port) {
    struct st_rx_source_info src;
    memset(&src, 0, sizeof(src));
    src.udp_port[MTL_SESSION_PORT_P] = udp_port;
    memcpy(src.ip_addr[MTL_SESSION_PORT_P], src_ip, MTL_IP_ADDR_LEN);

    std::vector<int> baselines(raw_rx.size(), 0);
    for (size_t i = 0; i < raw_rx.size(); i++) {
      EXPECT_GE(st30_rx_update_source(rx_handles[i].get(), &src), 0);
      baselines[i] = mtl_test::rx_frames(raw_rx[i]);
    }

    auto delta_frames = [&](const tests_context* c) {
      return mtl_test::rx_frames(c) - baselines[c->idx];
    };
    auto rate_res =
        mtl_test::measure_rate_window(raw_rx, delta_frames, kSt30RateWarmupFrames,
                                      kSt30RateMinDeltaFrames, kSt30RateMinWindowMs);
    EXPECT_TRUE(rate_res.warmed_up) << label;
    EXPECT_TRUE(rate_res.complete) << label;
    for (size_t i = 0; i < raw_rx.size(); i++) {
      const int fb_rec = mtl_test::rx_frames(raw_rx[i]);
      EXPECT_GT(fb_rec, baselines[i]);
      info("%s, session %zu fb_rec %d framerate %f for %s\n", __func__, i,
           fb_rec - baselines[i], rate_res.rate[i], label);
      EXPECT_NEAR(rate_res.rate[i], expect_framerate, expect_framerate * 0.1);
    }
  };

  /* Switch to mcast port P (tx_session:1). */
  if (tx_update_dst) {
    __atomic_store_n(&raw_tx[0]->seq_id, 0, __ATOMIC_RELAXED);
    struct st_tx_dest_info dst;
    memset(&dst, 0, sizeof(dst));
    dst.udp_port[MTL_SESSION_PORT_P] = ST30_TEST_UDP_PORT + 2;
    memcpy(dst.dip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
           MTL_IP_ADDR_LEN);
    EXPECT_GE(st30_tx_update_destination(tx_handles[0].get(), &dst), 0);
  } else {
    __atomic_store_n(&raw_tx[1]->seq_id, 0, __ATOMIC_RELAXED);
  }
  verify_switched_source("mcast 1", ctx->mcast_ip_addr[MTL_PORT_P],
                         ST30_TEST_UDP_PORT + 2);

  /* Switch to mcast port R (tx_session:2). */
  if (tx_sessions > 2) {
    __atomic_store_n(&raw_tx[2]->seq_id, rand(), __ATOMIC_RELAXED);
    verify_switched_source("mcast 2", ctx->mcast_ip_addr[MTL_PORT_R],
                           ST30_TEST_UDP_PORT + 4);
  }

  /* Switch back to initial stream (unicast or mcast port 2 on mcast-only hosts). */
  __atomic_store_n(&raw_tx[0]->seq_id, rand(), __ATOMIC_RELAXED);
  if (tx_update_dst) {
    struct st_tx_dest_info dst;
    memset(&dst, 0, sizeof(dst));
    dst.udp_port[MTL_SESSION_PORT_P] = ST30_TEST_UDP_PORT;
    const uint8_t* dip =
        ctx->mcast_only ? ctx->mcast_ip_addr[MTL_PORT_2] : ctx->para.sip_addr[MTL_PORT_R];
    memcpy(dst.dip_addr[MTL_SESSION_PORT_P], dip, MTL_IP_ADDR_LEN);
    EXPECT_GE(st30_tx_update_destination(tx_handles[0].get(), &dst), 0);
  }
  const uint8_t* back_ip =
      ctx->mcast_only ? ctx->mcast_ip_addr[MTL_PORT_2] : ctx->para.sip_addr[MTL_PORT_P];
  verify_switched_source(ctx->mcast_only ? "mcast 0" : "unicast 0", back_ip,
                         ST30_TEST_UDP_PORT);

  for (auto& w : rx_workers) w.stop();
  for (auto& w : tx_workers) w.stop();
  EXPECT_GE(mtl_guard.stop(), 0);
}

TEST(St30_rx, update_source_frame) {
  st30_rx_update_src_test(ST30_TYPE_FRAME_LEVEL, 3);
}
TEST(St30_rx, update_source_rtp) {
  st30_rx_update_src_test(ST30_TYPE_RTP_LEVEL, 2);
}
TEST(St30_tx, update_dest_frame) {
  st30_rx_update_src_test(ST30_TYPE_FRAME_LEVEL, 1);
}
TEST(St30_tx, update_dest_rtp) {
  st30_rx_update_src_test(ST30_TYPE_RTP_LEVEL, 1);
}

static void st30_rx_meta_test(const std::vector<St30SessionSpec>& specs,
                              bool user_timestamp = false) {
  auto ctx = st_test_ctx();
  auto m_handle = ctx->handle;
  MTL_TEST_REQUIRE_PORTS(ctx, 2);

  const size_t sessions = specs.size();
  const double expect_framerate = user_timestamp ? 500.0 : 1000.0;

  std::vector<mtl_test::ScopedContext> tx_ctxs, rx_ctxs;
  std::vector<tests_context*> raw_tx, raw_rx;
  std::vector<ScopedSt30Tx> tx_handles;
  std::vector<ScopedSt30Rx> rx_handles;
  mtl_test::ScopedMtlStart mtl_guard(m_handle);
  tx_ctxs.reserve(sessions);
  rx_ctxs.reserve(sessions);
  raw_tx.reserve(sessions);
  raw_rx.reserve(sessions);
  tx_handles.reserve(sessions);
  rx_handles.reserve(sessions);

  for (size_t i = 0; i < sessions; i++) {
    const auto& spec = specs[i];
    const uint16_t udp_port = ST30_TEST_UDP_PORT + static_cast<uint16_t>(i * 2);

    tx_ctxs.push_back(mtl_test::make_context(ctx, static_cast<int>(i), 3));
    raw_tx.push_back(tx_ctxs.back().get());

    struct st30_tx_ops ops_tx;
    fill_loopback_tx_ops(&ops_tx, raw_tx[i], spec, udp_port, 0, "st30_meta_test");
    if (user_timestamp) {
      ops_tx.get_next_frame = tx_audio_next_frame_timestamp;
      ops_tx.flags |= ST30_TX_FLAG_USER_PACING;
    }
    raw_tx[i]->frame_time = st30_get_packet_time(ops_tx.ptime);

    ScopedSt30Tx handle(st30_tx_create(m_handle, &ops_tx));
    ASSERT_TRUE(static_cast<bool>(handle));
    __atomic_store_n(&raw_tx[i]->handle, handle.get(), __ATOMIC_RELEASE);
    tx_handles.push_back(std::move(handle));
  }

  for (size_t i = 0; i < sessions; i++) {
    const auto& spec = specs[i];
    const uint16_t udp_port = ST30_TEST_UDP_PORT + static_cast<uint16_t>(i * 2);

    rx_ctxs.push_back(mtl_test::make_context(ctx, static_cast<int>(i), 3));
    raw_rx.push_back(rx_ctxs.back().get());

    struct st30_rx_ops ops_rx;
    fill_loopback_rx_ops(&ops_rx, raw_rx[i], spec, udp_port, 0, "st30_meta_test");
    ops_rx.notify_frame_ready = st30_rx_meta_frame_ready;

    ScopedSt30Rx handle(st30_rx_create(m_handle, &ops_rx));
    ASSERT_TRUE(static_cast<bool>(handle));
    raw_rx[i]->frame_size = ops_rx.framebuff_size;

    auto meta = static_cast<struct st30_rx_frame_meta*>(
        st_test_zmalloc(sizeof(struct st30_rx_frame_meta)));
    ASSERT_TRUE(meta != nullptr);
    meta->channel = ops_rx.channel;
    meta->sampling = ops_rx.sampling;
    meta->fmt = ops_rx.fmt;
    raw_rx[i]->priv = meta; /* Owned and freed by ScopedContext. */
    __atomic_store_n(&raw_rx[i]->handle, handle.get(), __ATOMIC_RELEASE);
    rx_handles.push_back(std::move(handle));
  }

  EXPECT_GE(mtl_guard.start(), 0);
  auto rate_res =
      mtl_test::measure_rate_window(raw_rx, mtl_test::rx_frames, kSt30RateWarmupFrames,
                                    kSt30RateMinDeltaFrames, kSt30RateMinWindowMs);
  EXPECT_TRUE(rate_res.warmed_up);
  EXPECT_TRUE(rate_res.complete);

  EXPECT_GE(mtl_guard.stop(), 0);

  for (size_t i = 0; i < sessions; i++) {
    const int fb_rec = mtl_test::rx_frames(raw_rx[i]);
    const int fb_send = mtl_test::tx_frames(raw_tx[i]);
    const int meta_fail = mtl_test::relaxed_load(&raw_rx[i]->rx_meta_fail_cnt);
    EXPECT_GT(fb_rec, 0);
    info("%s, session %zu fb_rec %d fail %d framerate %f, fb send %d\n", __func__, i,
         fb_rec, meta_fail, rate_res.rate[i], fb_send);
    EXPECT_LE(meta_fail, 2);
    EXPECT_NEAR(rate_res.rate[i], expect_framerate, expect_framerate * 0.1);
  }
}

TEST(St30_rx, frame_meta_pcm16_48k_2ch_s1) {
  st30_rx_meta_test(
      {{ST30_TYPE_FRAME_LEVEL, ST30_SAMPLING_48K, ST30_PTIME_1MS, 2, ST30_FMT_PCM16}});
}

TEST(St30_rx, frame_user_timestamp) {
  st30_rx_meta_test(
      {{ST30_TYPE_FRAME_LEVEL, ST30_SAMPLING_48K, ST30_PTIME_1MS, 2, ST30_FMT_PCM16}},
      true);
}

static void st30_create_after_start_test(const std::vector<St30SessionSpec>& specs,
                                         int repeat) {
  auto ctx = st_test_ctx();
  auto m_handle = ctx->handle;
  MTL_TEST_REQUIRE_PORTS(ctx, 2);

  const size_t sessions = specs.size();
  std::vector<double> expect_framerate(sessions, 0.0);
  for (size_t i = 0; i < sessions; i++) {
    expect_framerate[i] = st30_expected_fps(specs[i].ptime);
  }

  /*
   * Documented after_start exception: ScopedMtlStart lives outside the repeat
   * loop because this test specifically creates and frees sessions while MTL is
   * running. Inside each iteration, declaration order (ScopedContext ->
   * ScopedHandle -> ScopedWorker) guarantees workers stop before handles free
   * before contexts free.
   */
  mtl_test::ScopedMtlStart mtl_guard(m_handle);
  EXPECT_GE(mtl_guard.start(), 0);

  for (int r = 0; r < repeat; r++) {
    std::vector<mtl_test::ScopedContext> tx_ctxs, rx_ctxs;
    std::vector<tests_context*> raw_tx, raw_rx;
    std::vector<ScopedSt30Tx> tx_handles;
    std::vector<ScopedSt30Rx> rx_handles;
    std::vector<mtl_test::ScopedWorker> tx_workers, rx_workers;
    tx_ctxs.reserve(sessions);
    rx_ctxs.reserve(sessions);
    raw_tx.reserve(sessions);
    raw_rx.reserve(sessions);
    tx_handles.reserve(sessions);
    rx_handles.reserve(sessions);
    tx_workers.reserve(sessions);
    rx_workers.reserve(sessions);

    for (size_t i = 0; i < sessions; i++) {
      const auto& spec = specs[i];
      const uint16_t udp_port = ST30_TEST_UDP_PORT + static_cast<uint16_t>(i * 2);

      tx_ctxs.push_back(mtl_test::make_context(ctx, static_cast<int>(i), 3));
      raw_tx.push_back(tx_ctxs.back().get());

      struct st30_tx_ops ops_tx;
      fill_loopback_tx_ops(&ops_tx, raw_tx[i], spec, udp_port);
      ScopedSt30Tx handle(st30_tx_create(m_handle, &ops_tx));
      ASSERT_TRUE(static_cast<bool>(handle));
      __atomic_store_n(&raw_tx[i]->handle, handle.get(), __ATOMIC_RELEASE);
      tx_handles.push_back(std::move(handle));
      if (spec.type == ST30_TYPE_RTP_LEVEL) {
        tx_workers.emplace_back(raw_tx[i], std::thread(tx_feed_packet, raw_tx[i]));
      }
    }

    for (size_t i = 0; i < sessions; i++) {
      const auto& spec = specs[i];
      const uint16_t udp_port = ST30_TEST_UDP_PORT + static_cast<uint16_t>(i * 2);

      rx_ctxs.push_back(mtl_test::make_context(ctx, static_cast<int>(i), 3));
      raw_rx.push_back(rx_ctxs.back().get());

      struct st30_rx_ops ops_rx;
      fill_loopback_rx_ops(&ops_rx, raw_rx[i], spec, udp_port);
      ScopedSt30Rx handle(st30_rx_create(m_handle, &ops_rx));
      ASSERT_TRUE(static_cast<bool>(handle));
      __atomic_store_n(&raw_rx[i]->handle, handle.get(), __ATOMIC_RELEASE);
      rx_handles.push_back(std::move(handle));
      if (spec.type == ST30_TYPE_RTP_LEVEL) {
        rx_workers.emplace_back(raw_rx[i], std::thread(rx_get_packet, raw_rx[i]));
      }
    }

    auto rate_res =
        mtl_test::measure_rate_window(raw_rx, mtl_test::rx_frames, kSt30RateWarmupFrames,
                                      kSt30RateMinDeltaFrames, kSt30RateMinWindowMs);
    EXPECT_TRUE(rate_res.warmed_up);
    EXPECT_TRUE(rate_res.complete);

    for (auto& w : tx_workers) w.stop();
    for (auto& w : rx_workers) w.stop();

    for (size_t i = 0; i < sessions; i++) {
      const int fb_rec = mtl_test::rx_frames(raw_rx[i]);
      EXPECT_GT(fb_rec, 0);
      info("%s, session %zu fb_rec %d framerate %f\n", __func__, i, fb_rec,
           rate_res.rate[i]);
      EXPECT_NEAR(rate_res.rate[i], expect_framerate[i], expect_framerate[i] * 0.1);
    }
  }

  EXPECT_GE(mtl_guard.stop(), 0);
}

TEST(St30_rx, after_start_mix_s2_r2) {
  st30_create_after_start_test(
      {
          {ST30_TYPE_FRAME_LEVEL, ST30_SAMPLING_96K, ST30_PTIME_1MS, 1, ST30_FMT_PCM24},
          {ST30_TYPE_RTP_LEVEL, ST30_SAMPLING_48K, ST30_PTIME_1MS, 2, ST30_FMT_PCM16},
      },
      2);
}

TEST(St30_rx, session_stats_reset) {
  auto ctx = st_test_ctx();
  auto m_handle = ctx->handle;
  MTL_TEST_REQUIRE_PORTS(ctx, 2);

  const St30SessionSpec spec{ST30_TYPE_FRAME_LEVEL, ST30_SAMPLING_48K, ST30_PTIME_1MS, 2,
                             ST30_FMT_PCM16};
  const uint16_t udp_port = ST30_TEST_UDP_PORT + 50;

  auto tx_ctx = mtl_test::make_context(ctx, 0, 3);
  auto rx_ctx = mtl_test::make_context(ctx, 0, 3);
  ScopedSt30Tx tx_handle;
  ScopedSt30Rx rx_handle;
  mtl_test::ScopedMtlStart mtl_guard(m_handle);

  struct st30_tx_ops ops_tx;
  fill_loopback_tx_ops(&ops_tx, tx_ctx.get(), spec, udp_port, 0, "st30_stats_tx");
  tx_handle.reset(st30_tx_create(m_handle, &ops_tx));
  ASSERT_TRUE(static_cast<bool>(tx_handle));
  __atomic_store_n(&tx_ctx->handle, tx_handle.get(), __ATOMIC_RELEASE);

  struct st30_rx_ops ops_rx;
  fill_loopback_rx_ops(&ops_rx, rx_ctx.get(), spec, udp_port, 0, "st30_stats_rx");
  rx_handle.reset(st30_rx_create(m_handle, &ops_rx));
  ASSERT_TRUE(static_cast<bool>(rx_handle));
  __atomic_store_n(&rx_ctx->handle, rx_handle.get(), __ATOMIC_RELEASE);

  struct st30_tx_user_stats tx_stats;
  struct st30_rx_user_stats rx_stats;
  EXPECT_LT(st30_tx_get_session_stats(nullptr, &tx_stats), 0);
  EXPECT_LT(st30_tx_get_session_stats(tx_handle.get(), nullptr), 0);
  EXPECT_LT(st30_tx_reset_session_stats(nullptr), 0);
  EXPECT_LT(st30_rx_get_session_stats(nullptr, &rx_stats), 0);
  EXPECT_LT(st30_rx_get_session_stats(rx_handle.get(), nullptr), 0);
  EXPECT_LT(st30_rx_reset_session_stats(nullptr), 0);

  EXPECT_GE(mtl_guard.start(), 0);
  /* 30 frames (~30ms at 1ms ptime) populates TX and RX traffic counters. */
  EXPECT_TRUE(mtl_test::wait_until(
      [&]() { return mtl_test::rx_frames(rx_ctx.get()) >= kSt30PhaseTargetFrames; },
      kSt30PhaseTimeoutMs));

  EXPECT_GE(mtl_guard.stop(), 0);

  uint8_t bogus = 0;
  EXPECT_LT(st30_rx_put_framebuff(rx_handle.get(), nullptr), 0);
  EXPECT_LT(st30_rx_put_framebuff(rx_handle.get(), &bogus), 0);

  EXPECT_EQ(st30_tx_get_session_stats(tx_handle.get(), &tx_stats), 0);
  EXPECT_GT(tx_stats.common.port[MTL_SESSION_PORT_P].packets, static_cast<uint64_t>(0));
  EXPECT_GT(tx_stats.common.port[MTL_SESSION_PORT_P].bytes, static_cast<uint64_t>(0));
  EXPECT_GT(tx_stats.common.port[MTL_SESSION_PORT_P].frames, static_cast<uint64_t>(0));

  EXPECT_EQ(st30_rx_get_session_stats(rx_handle.get(), &rx_stats), 0);
  EXPECT_GT(rx_stats.common.port[MTL_SESSION_PORT_P].packets, static_cast<uint64_t>(0));
  EXPECT_GT(rx_stats.common.port[MTL_SESSION_PORT_P].bytes, static_cast<uint64_t>(0));
  EXPECT_GT(rx_stats.common.port[MTL_SESSION_PORT_P].frames, static_cast<uint64_t>(0));
  EXPECT_GT(rx_stats.common.stat_pkts_received, static_cast<uint64_t>(0));

  EXPECT_EQ(st30_tx_reset_session_stats(tx_handle.get()), 0);
  EXPECT_EQ(st30_rx_reset_session_stats(rx_handle.get()), 0);

  EXPECT_EQ(st30_tx_get_session_stats(tx_handle.get(), &tx_stats), 0);
  EXPECT_EQ(tx_stats.common.port[MTL_SESSION_PORT_P].packets, static_cast<uint64_t>(0));
  EXPECT_EQ(tx_stats.common.port[MTL_SESSION_PORT_P].bytes, static_cast<uint64_t>(0));
  EXPECT_EQ(tx_stats.common.port[MTL_SESSION_PORT_P].frames, static_cast<uint64_t>(0));

  EXPECT_EQ(st30_rx_get_session_stats(rx_handle.get(), &rx_stats), 0);
  EXPECT_EQ(rx_stats.common.port[MTL_SESSION_PORT_P].packets, static_cast<uint64_t>(0));
  EXPECT_EQ(rx_stats.common.port[MTL_SESSION_PORT_P].bytes, static_cast<uint64_t>(0));
  EXPECT_EQ(rx_stats.common.port[MTL_SESSION_PORT_P].frames, static_cast<uint64_t>(0));
  EXPECT_EQ(rx_stats.common.stat_pkts_received, static_cast<uint64_t>(0));
}

TEST(St30_rx, timing_parser_meta) {
  auto ctx = st_test_ctx();
  auto m_handle = ctx->handle;
  MTL_TEST_REQUIRE_PORTS(ctx, 2);

  const St30SessionSpec spec{ST30_TYPE_FRAME_LEVEL, ST30_SAMPLING_48K, ST30_PTIME_1MS, 2,
                             ST30_FMT_PCM16};
  const uint16_t udp_port = ST30_TEST_UDP_PORT + 60;

  /* Negative check: TIMING_PARSER_META requires notify_timing_parser_result. */
  {
    auto neg_ctx = mtl_test::make_context(ctx, 0, 3);
    struct st30_rx_ops neg_ops;
    fill_loopback_rx_ops(&neg_ops, neg_ctx.get(), spec, udp_port, 0, "st30_tp_neg");
    neg_ops.flags |= ST30_RX_FLAG_TIMING_PARSER_META;
    neg_ops.notify_timing_parser_result = nullptr;
    ScopedSt30Rx neg_rx(st30_rx_create(m_handle, &neg_ops));
    EXPECT_FALSE(static_cast<bool>(neg_rx));
  }

  St30TpState tp_state;
  auto tx_ctx = mtl_test::make_context(ctx, 0, 3);
  auto rx_ctx = mtl_test::make_context(ctx, 0, 3);
  ScopedSt30Tx tx_handle;
  ScopedSt30Rx rx_handle;
  mtl_test::ScopedMtlStart mtl_guard(m_handle);
  tp_state.ctx = rx_ctx.get();

  struct st30_tx_ops ops_tx;
  fill_loopback_tx_ops(&ops_tx, tx_ctx.get(), spec, udp_port, 0, "st30_tp_tx");
  tx_handle.reset(st30_tx_create(m_handle, &ops_tx));
  ASSERT_TRUE(static_cast<bool>(tx_handle));
  __atomic_store_n(&tx_ctx->handle, tx_handle.get(), __ATOMIC_RELEASE);

  struct st30_rx_ops ops_rx;
  fill_loopback_rx_ops(&ops_rx, rx_ctx.get(), spec, udp_port, 0, "st30_tp_rx");
  ops_rx.priv = &tp_state;
  ops_rx.notify_frame_ready = st30_rx_tp_frame_ready;
  ops_rx.notify_timing_parser_result = st30_rx_tp_notify;
  ops_rx.flags |= ST30_RX_FLAG_TIMING_PARSER_META;
  rx_handle.reset(st30_rx_create(m_handle, &ops_rx));
  ASSERT_TRUE(static_cast<bool>(rx_handle));
  tp_state.rx_handle.store(rx_handle.get(), std::memory_order_release);

  EXPECT_GE(mtl_guard.start(), 0);
  /* Timing parser reports every 200ms; wait for >= 2 reports (~400ms). */
  EXPECT_TRUE(mtl_test::wait_until(
      [&]() { return tp_state.cb_cnt.load(std::memory_order_relaxed) >= 2; },
      kSt30PhaseTimeoutMs));
  EXPECT_GE(mtl_guard.stop(), 0);

  EXPECT_GE(tp_state.cb_cnt.load(std::memory_order_relaxed), 2);
  EXPECT_EQ(tp_state.bad_port_cnt.load(std::memory_order_relaxed), 0);
  EXPECT_EQ(tp_state.null_tp_cnt.load(std::memory_order_relaxed), 0);
  EXPECT_EQ(tp_state.zero_pkts_cnt.load(std::memory_order_relaxed), 0);
  EXPECT_GT(mtl_test::rx_frames(rx_ctx.get()), 0);
}
