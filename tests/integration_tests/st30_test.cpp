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
static constexpr int kSt30RateWarmupFrames = 10;
static constexpr int kSt30RateMinDeltaFrames = 100;
static constexpr int kSt30RateMinWindowMs = 1500;
static constexpr int kSt30PhaseTargetFrames = 50;
static constexpr int kSt30PhaseTimeoutMs = 6000;

/* rx_audio_session_put_frame unconditionally returns -EIO
 * (lib/src/st2110/st_rx_audio_session.c:91). */
static void put_st30_rx_framebuff(st30_rx_handle handle, void* frame) {
  (void)st30_rx_put_framebuff(handle, frame);
}

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
  if (!__atomic_load_n(&ctx->handle, __ATOMIC_ACQUIRE)) return -EIO;
  if (!ctx->ptp_time_first_frame) {
    ctx->ptp_time_first_frame = mtl_ptp_read_time(ctx->ctx->handle);
  }

  const int sent = __atomic_fetch_add(&ctx->fb_send, 1, __ATOMIC_RELAXED);
  meta->tfmt = ST10_TIMESTAMP_FMT_TAI;
  meta->timestamp = ctx->ptp_time_first_frame + sent * ctx->frame_time * 2;
  *next_frame_idx = ctx->fb_idx;
  ctx->fb_idx = static_cast<uint16_t>((ctx->fb_idx + 1) % ctx->fb_cnt);
  return 0;
}

static int tx_audio_build_rtp_packet(tests_context* s, struct st_rfc3550_rtp_hdr* rtp,
                                     uint16_t* pkt_len) {
  memset(rtp, 0x0, sizeof(*rtp));
  rtp->version = 2;
  rtp->payload_type = ST30_TEST_PAYLOAD_TYPE;
  rtp->ssrc = htonl(0x66666666 + s->idx);
  rtp->tmstamp = s->rtp_tmstamp++;
  const uint16_t seq =
      static_cast<uint16_t>(__atomic_fetch_add(&s->seq_id, 1, __ATOMIC_RELAXED) & 0xFFFF);
  rtp->seq_number = htons(seq);
  if (s->check_sha) {
    uint8_t* payload = reinterpret_cast<uint8_t*>(rtp) + sizeof(*rtp);
    mtl_memcpy(payload, s->frame_buf[s->fb_idx], s->pkt_data_len);
    s->fb_idx = static_cast<uint16_t>((s->fb_idx + 1) % TEST_SHA_HIST_NUM);
  }
  *pkt_len = sizeof(struct st_rfc3550_rtp_hdr) + s->pkt_data_len;
  return 0;
}

static void tx_feed_packet(void* args) {
  auto ctx = static_cast<tests_context*>(args);
  auto handle =
      static_cast<st30_tx_handle>(__atomic_load_n(&ctx->handle, __ATOMIC_ACQUIRE));
  void* usrptr = nullptr;
  uint16_t mbuf_len = 0;
  std::unique_lock<std::mutex> lck(ctx->mtx, std::defer_lock);
  while (!ctx->stop) {
    void* mbuf = st30_tx_get_mbuf(handle, &usrptr);
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
  if (!__atomic_load_n(&ctx->handle, __ATOMIC_ACQUIRE)) return -EIO;
  std::unique_lock<std::mutex> lck(ctx->mtx);
  ctx->cv.notify_all();
  __atomic_fetch_add(&ctx->fb_send, 1, __ATOMIC_RELAXED);
  return 0;
}

static int rx_rtp_ready(void* args) {
  auto ctx = static_cast<tests_context*>(args);
  if (!__atomic_load_n(&ctx->handle, __ATOMIC_ACQUIRE)) return -EIO;
  std::unique_lock<std::mutex> lck(ctx->mtx);
  ctx->cv.notify_all();
  return 0;
}

static void rx_get_packet(void* args) {
  auto ctx = static_cast<tests_context*>(args);
  auto handle =
      static_cast<st30_rx_handle>(__atomic_load_n(&ctx->handle, __ATOMIC_ACQUIRE));
  void* usrptr = nullptr;
  uint16_t mbuf_len = 0;
  std::unique_lock<std::mutex> lck(ctx->mtx, std::defer_lock);
  while (!ctx->stop) {
    void* mbuf = st30_rx_get_mbuf(handle, &usrptr, &mbuf_len);
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
    if (ctx->frame_size > 0 && static_cast<size_t>(mbuf_len) !=
                                   sizeof(struct st_rfc3550_rtp_hdr) + ctx->frame_size) {
      __atomic_fetch_add(&ctx->sha_fail_cnt, 1, __ATOMIC_RELAXED);
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
  if (ctx->check_sha) st30_verify_sha(ctx, frame);
  put_st30_rx_framebuff(handle, frame);
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
    put_st30_rx_framebuff(handle, frame);
    return 0;
  }

  const int prev_rec = __atomic_fetch_add(&ctx->fb_rec, 1, __ATOMIC_RELAXED);
  if (expect_meta->sampling != meta->sampling || expect_meta->channel != meta->channel ||
      expect_meta->fmt != meta->fmt || meta->status != ST_FRAME_STATUS_COMPLETE ||
      meta->frame_recv_size != ctx->frame_size ||
      meta->tfmt != ST10_TIMESTAMP_FMT_MEDIA_CLK ||
      meta->rtp_timestamp != meta->timestamp || meta->timestamp_first_pkt == 0 ||
      (prev_rec > 0 && expect_meta->timestamp == meta->timestamp)) {
    __atomic_fetch_add(&ctx->rx_meta_fail_cnt, 1, __ATOMIC_RELAXED);
  }
  expect_meta->timestamp = meta->timestamp;

  put_st30_rx_framebuff(handle, frame);
  return 0;
}

struct St30TpState {
  tests_context* ctx = nullptr;
  std::atomic<st30_rx_handle> rx_handle{nullptr};
  std::atomic<int> cb_cnt{0};
  std::atomic<int> steady_cnt{0};
  std::atomic<int> bad_port_cnt{0};
  std::atomic<int> null_tp_cnt{0};
  std::atomic<int> zero_pkts_cnt{0};
  std::atomic<int> pkts_range_fail_cnt{0};
  std::atomic<int> ipt_avg_fail_cnt{0};
  std::atomic<int> ipt_order_fail_cnt{0};
  std::atomic<int> dpvr_order_fail_cnt{0};
  std::atomic<int> tsdf_fail_cnt{0};
  std::atomic<int> non_compliant_cnt{0};
};

static int st30_rx_tp_frame_ready(void* priv, void* frame,
                                  struct st30_rx_frame_meta* /*meta*/) {
  auto* state = static_cast<St30TpState*>(priv);
  if (!state) return -EIO;
  auto handle = state->rx_handle.load(std::memory_order_acquire);
  if (!handle) return -EIO;
  put_st30_rx_framebuff(handle, frame);
  __atomic_fetch_add(&state->ctx->fb_rec, 1, __ATOMIC_RELAXED);
  return 0;
}

static int st30_rx_tp_notify(void* priv, enum mtl_session_port port,
                             struct st30_rx_tp_meta* tp) {
  auto* state = static_cast<St30TpState*>(priv);
  if (!state) return -EIO;
  const int report_num = state->cb_cnt.fetch_add(1, std::memory_order_relaxed) + 1;
  if (port != MTL_SESSION_PORT_P)
    state->bad_port_cnt.fetch_add(1, std::memory_order_relaxed);
  if (!tp) {
    state->null_tp_cnt.fetch_add(1, std::memory_order_relaxed);
    return 0;
  }
  if (tp->pkts_cnt == 0) state->zero_pkts_cnt.fetch_add(1, std::memory_order_relaxed);
  /* Skip report #1 due to pre-existing ra_tp_init ipt_min == 0 quirk. */
  if (report_num >= 2) {
    state->steady_cnt.fetch_add(1, std::memory_order_relaxed);
    if (tp->pkts_cnt < 150u || tp->pkts_cnt > 250u)
      state->pkts_range_fail_cnt.fetch_add(1, std::memory_order_relaxed);
    if (tp->ipt_avg < 800000.0f || tp->ipt_avg > 1200000.0f)
      state->ipt_avg_fail_cnt.fetch_add(1, std::memory_order_relaxed);
    if (tp->ipt_max < tp->ipt_min)
      state->ipt_order_fail_cnt.fetch_add(1, std::memory_order_relaxed);
    if (tp->dpvr_max < tp->dpvr_min)
      state->dpvr_order_fail_cnt.fetch_add(1, std::memory_order_relaxed);
    if (tp->tsdf < 0) state->tsdf_fail_cnt.fetch_add(1, std::memory_order_relaxed);
    if (tp->compliant == ST_RX_TP_COMPLIANT_FAILED)
      state->non_compliant_cnt.fetch_add(1, std::memory_order_relaxed);
  }
  return 0;
}

struct St30SwitchState {
  tests_context* ctx = nullptr;
  std::atomic<st30_rx_handle> rx_handle{nullptr};
  size_t frame_size = 0;
  std::atomic<int> rx_from_tx[2]{};
  std::atomic<int> rx_from_other{0};

  void reset() {
    rx_from_tx[0].store(0, std::memory_order_relaxed);
    rx_from_tx[1].store(0, std::memory_order_relaxed);
    rx_from_other.store(0, std::memory_order_relaxed);
  }
};

static constexpr uint8_t kSt30SwitchPattern[2] = {0xA0, 0xB1};

static int st30_switch_rx_frame_ready(void* priv, void* frame,
                                      struct st30_rx_frame_meta* /*meta*/) {
  auto* state = static_cast<St30SwitchState*>(priv);
  if (!state) return -EIO;
  auto handle = state->rx_handle.load(std::memory_order_acquire);
  if (!handle) return -EIO;
  const auto* bytes = static_cast<const uint8_t*>(frame);
  if (state->frame_size > 0) {
    bool match0 = true;
    bool match1 = true;
    for (size_t i = 0; i < state->frame_size && (match0 || match1); i++) {
      if (bytes[i] != kSt30SwitchPattern[0]) match0 = false;
      if (bytes[i] != kSt30SwitchPattern[1]) match1 = false;
    }
    if (match0)
      state->rx_from_tx[0].fetch_add(1, std::memory_order_relaxed);
    else if (match1)
      state->rx_from_tx[1].fetch_add(1, std::memory_order_relaxed);
    else
      state->rx_from_other.fetch_add(1, std::memory_order_relaxed);
  }
  put_st30_rx_framebuff(handle, frame);
  __atomic_fetch_add(&state->ctx->fb_rec, 1, __ATOMIC_RELAXED);
  return 0;
}

static void st30_switch_rx_get_packet(St30SwitchState* state) {
  auto* ctx = state->ctx;
  auto handle = state->rx_handle.load(std::memory_order_acquire);
  void* usrptr = nullptr;
  uint16_t mbuf_len = 0;
  std::unique_lock<std::mutex> lck(ctx->mtx, std::defer_lock);
  while (!ctx->stop) {
    void* mbuf = st30_rx_get_mbuf(handle, &usrptr, &mbuf_len);
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
    const size_t expect_len = sizeof(struct st_rfc3550_rtp_hdr) + state->frame_size;
    if ((state->frame_size > 0 && static_cast<size_t>(mbuf_len) != expect_len) ||
        static_cast<size_t>(mbuf_len) < sizeof(struct st_rfc3550_rtp_hdr)) {
      state->rx_from_other.fetch_add(1, std::memory_order_relaxed);
    } else {
      const auto* rtp = static_cast<const struct st_rfc3550_rtp_hdr*>(usrptr);
      const uint32_t ssrc = ntohl(rtp->ssrc);
      if (ssrc == 0x66666666u)
        state->rx_from_tx[0].fetch_add(1, std::memory_order_relaxed);
      else if (ssrc == 0x66666667u)
        state->rx_from_tx[1].fetch_add(1, std::memory_order_relaxed);
      else
        state->rx_from_other.fetch_add(1, std::memory_order_relaxed);
    }
    __atomic_fetch_add(&ctx->fb_rec, 1, __ATOMIC_RELAXED);
    st30_rx_put_mbuf(handle, mbuf);
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

struct St30RxOptions {
  bool check_sha = false;
  bool check_meta = false;
  bool user_timestamp = false;
  bool check_zero_loss = false;
};

int expected_st30_wire_pkt_size(const St30SessionSpec& spec) {
  int sample_bytes = 0;
  switch (spec.fmt) {
    case ST30_FMT_PCM8:
      sample_bytes = 1;
      break;
    case ST30_FMT_PCM16:
      sample_bytes = 2;
      break;
    case ST30_FMT_PCM24:
      sample_bytes = 3;
      break;
    case ST31_FMT_AM824:
      sample_bytes = 4;
      break;
    default:
      return -1;
  }
  int samples_per_ch = 0;
  if (spec.sampling == ST30_SAMPLING_48K) {
    switch (spec.ptime) {
      case ST30_PTIME_1MS:
        samples_per_ch = 48;
        break;
      case ST30_PTIME_125US:
        samples_per_ch = 6;
        break;
      case ST30_PTIME_250US:
        samples_per_ch = 12;
        break;
      case ST30_PTIME_333US:
        samples_per_ch = 16;
        break;
      case ST30_PTIME_4MS:
        samples_per_ch = 192;
        break;
      case ST31_PTIME_80US:
        samples_per_ch = 4;
        break;
      default:
        return -1;
    }
  } else if (spec.sampling == ST30_SAMPLING_96K) {
    switch (spec.ptime) {
      case ST30_PTIME_1MS:
        samples_per_ch = 96;
        break;
      case ST30_PTIME_125US:
        samples_per_ch = 12;
        break;
      case ST30_PTIME_250US:
        samples_per_ch = 24;
        break;
      case ST30_PTIME_333US:
        samples_per_ch = 32;
        break;
      case ST30_PTIME_4MS:
        samples_per_ch = 384;
        break;
      case ST31_PTIME_80US:
        samples_per_ch = 8;
        break;
      default:
        return -1;
    }
  } else if (spec.sampling == ST31_SAMPLING_44K) {
    switch (spec.ptime) {
      case ST31_PTIME_1_09MS:
        samples_per_ch = 48;
        break;
      case ST31_PTIME_0_14MS:
        samples_per_ch = 6;
        break;
      case ST31_PTIME_0_09MS:
        samples_per_ch = 4;
        break;
      default:
        return -1;
    }
  } else {
    return -1;
  }
  return samples_per_ch * static_cast<int>(spec.channel) * sample_bytes;
}

using ScopedSt30Tx = mtl_test::ScopedHandle<st30_tx_handle, st30_tx_free>;
using ScopedSt30Rx = mtl_test::ScopedHandle<st30_rx_handle, st30_rx_free>;

double st30_expected_fps(enum st30_ptime ptime) {
  double pkt_time_ns = st30_get_packet_time(ptime);
  return (pkt_time_ns > 0.0) ? (static_cast<double>(NS_PER_S) / pkt_time_ns) : 1000.0;
}

void st30_tx_ops_init(tests_context* st30, struct st30_tx_ops* ops,
                      const St30SessionSpec& spec = {}, bool loopback = false,
                      uint16_t udp_port = 0, uint32_t ssrc = 0,
                      const char* name = "st30_test") {
  auto ctx = st30->ctx;
  const uint16_t port =
      udp_port ? udp_port : static_cast<uint16_t>(ST30_TEST_UDP_PORT + st30->idx);
  memset(ops, 0, sizeof(*ops));
  ops->name = name;
  ops->priv = st30;
  if (loopback) {
    mtl_test::fill_raw_tx_port(*ops, ctx, port, ST30_TEST_PAYLOAD_TYPE, ssrc);
  } else {
    ops->num_port = ctx->same_dual_port ? 1
                                        : static_cast<uint8_t>(std::min<int>(
                                              ctx->para.num_ports, MTL_SESSION_PORT_MAX));
    memcpy(ops->dip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
           MTL_IP_ADDR_LEN);
    snprintf(ops->port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
             ctx->para.port[MTL_PORT_P]);
    ops->udp_port[MTL_SESSION_PORT_P] = port;
    if (ops->num_port == 2) {
      memcpy(ops->dip_addr[MTL_SESSION_PORT_R], ctx->mcast_ip_addr[MTL_PORT_R],
             MTL_IP_ADDR_LEN);
      snprintf(ops->port[MTL_SESSION_PORT_R], MTL_PORT_MAX_LEN, "%s",
               ctx->para.port[MTL_PORT_R]);
      ops->udp_port[MTL_SESSION_PORT_R] = port;
    }
    ops->payload_type = ST30_TEST_PAYLOAD_TYPE;
    ops->ssrc = ssrc;
  }
  ops->type = spec.type;
  ops->channel = spec.channel;
  ops->fmt = spec.fmt;
  ops->sampling = spec.sampling;
  ops->ptime = spec.ptime;
  ops->framebuff_cnt = st30->fb_cnt;
  const int pkt_size =
      st30_get_packet_size(ops->fmt, ops->ptime, ops->sampling, ops->channel);
  EXPECT_GE(pkt_size, 0);
  st30->pkt_data_len = pkt_size;
  ops->framebuff_size = static_cast<uint32_t>(pkt_size);
  ops->get_next_frame = tx_audio_next_frame;
  ops->notify_rtp_done = tx_rtp_done;
  ops->rtp_ring_size = 1024;
}

void st30_rx_ops_init(tests_context* st30, struct st30_rx_ops* ops,
                      const St30SessionSpec& spec = {}, bool loopback = false,
                      uint16_t udp_port = 0, uint32_t ssrc = 0,
                      const char* name = "st30_test") {
  auto ctx = st30->ctx;
  const uint16_t port =
      udp_port ? udp_port : static_cast<uint16_t>(ST30_TEST_UDP_PORT + st30->idx);
  memset(ops, 0, sizeof(*ops));
  ops->name = name;
  ops->priv = st30;
  if (loopback) {
    ops->num_port = 1;
    memcpy(ops->ip_addr[MTL_SESSION_PORT_P], ctx->para.sip_addr[MTL_PORT_P],
           MTL_IP_ADDR_LEN);
    snprintf(ops->port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
             ctx->para.port[MTL_PORT_R]);
    ops->udp_port[MTL_SESSION_PORT_P] = port;
  } else {
    ops->num_port = ctx->same_dual_port ? 1
                                        : static_cast<uint8_t>(std::min<int>(
                                              ctx->para.num_ports, MTL_SESSION_PORT_MAX));
    memcpy(ops->ip_addr[MTL_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P], MTL_IP_ADDR_LEN);
    snprintf(ops->port[MTL_PORT_P], MTL_PORT_MAX_LEN, "%s", ctx->para.port[MTL_PORT_P]);
    ops->udp_port[MTL_PORT_P] = port;
    if (ops->num_port == 2) {
      memcpy(ops->ip_addr[MTL_PORT_R], ctx->mcast_ip_addr[MTL_PORT_R], MTL_IP_ADDR_LEN);
      snprintf(ops->port[MTL_PORT_R], MTL_PORT_MAX_LEN, "%s", ctx->para.port[MTL_PORT_R]);
      ops->udp_port[MTL_PORT_R] = port;
    }
  }
  ops->type = spec.type;
  ops->channel = spec.channel;
  ops->fmt = spec.fmt;
  ops->payload_type = ST30_TEST_PAYLOAD_TYPE;
  ops->ssrc = ssrc;
  ops->sampling = spec.sampling;
  ops->ptime = spec.ptime;
  ops->framebuff_cnt = st30->fb_cnt;
  const int pkt_size =
      st30_get_packet_size(ops->fmt, ops->ptime, ops->sampling, ops->channel);
  EXPECT_GE(pkt_size, 0);
  ops->framebuff_size = static_cast<uint32_t>(pkt_size);
  ops->notify_frame_ready = st30_rx_frame_ready;
  ops->notify_rtp_ready = rx_rtp_ready;
  ops->rtp_ring_size = 1024;
}

void st30_tx_assert_cnt(int expect_cnt) {
  auto ctx = st_test_ctx();
  struct st_var_info var;
  EXPECT_GE(st_get_var_info(ctx->handle, &var), 0);
  EXPECT_EQ(var.st30_tx_sessions_cnt, expect_cnt);
}

void st30_rx_assert_cnt(int expect_cnt) {
  auto ctx = st_test_ctx();
  struct st_var_info var;
  EXPECT_GE(st_get_var_info(ctx->handle, &var), 0);
  EXPECT_EQ(var.st30_rx_sessions_cnt, expect_cnt);
}

const auto kTxOpsInit = [](tests_context* c, struct st30_tx_ops* o) {
  st30_tx_ops_init(c, o);
};
const auto kRxOpsInit = [](tests_context* c, struct st30_rx_ops* o) {
  st30_rx_ops_init(c, o);
};

}  // namespace

TEST(St30_tx, create_free_mix) {
  mtl_test::verify_create_free<struct st30_tx_ops, st30_tx_handle, st30_tx_create,
                               st30_tx_free>(kTxOpsInit, st30_tx_assert_cnt, 2, 3, 4);
}
TEST(St30_tx, create_free_max) {
  mtl_test::verify_create_free_max<struct st30_tx_ops, st30_tx_handle, st30_tx_create,
                                   st30_tx_free>(kTxOpsInit, st30_tx_assert_cnt,
                                                 TEST_CREATE_FREE_MAX);
}
TEST(St30_tx, create_expect_fail) {
  mtl_test::verify_invalid_ports<struct st30_tx_ops, st30_tx_handle, st30_tx_create,
                                 st30_tx_free>(kTxOpsInit);
}
TEST(St30_tx, create_expect_fail_ring_sz) {
  for (uint16_t ring_size : {static_cast<uint16_t>(0), static_cast<uint16_t>(128 + 1)}) {
    mtl_test::verify_rtp_ring_invalid<struct st30_tx_ops, st30_tx_handle, st30_tx_create,
                                      st30_tx_free>(kTxOpsInit, ST30_TYPE_RTP_LEVEL,
                                                    ring_size);
  }
}
TEST(St30_tx, get_framebuffer) {
  for (uint16_t fbcnt : {static_cast<uint16_t>(3), static_cast<uint16_t>(1000)}) {
    mtl_test::verify_get_framebuffer<struct st30_tx_ops, st30_tx_handle, st30_tx_create,
                                     st30_tx_free>(kTxOpsInit, st30_tx_get_framebuffer,
                                                   fbcnt);
  }
}
TEST(St30_tx, get_framebuffer_expect_fail) {
  for (uint16_t fbcnt : {static_cast<uint16_t>(3), static_cast<uint16_t>(1000)}) {
    mtl_test::verify_get_framebuffer_oob<struct st30_tx_ops, st30_tx_handle,
                                         st30_tx_create, st30_tx_free>(
        kTxOpsInit, st30_tx_get_framebuffer, fbcnt);
  }
}

TEST(St30_rx, create_free_mix) {
  mtl_test::verify_create_free<struct st30_rx_ops, st30_rx_handle, st30_rx_create,
                               st30_rx_free>(kRxOpsInit, st30_rx_assert_cnt, 2, 3, 4);
}
TEST(St30_rx, create_free_max) {
  mtl_test::verify_create_free_max<struct st30_rx_ops, st30_rx_handle, st30_rx_create,
                                   st30_rx_free>(kRxOpsInit, st30_rx_assert_cnt,
                                                 TEST_CREATE_FREE_MAX);
}
TEST(St30_rx, create_expect_fail) {
  mtl_test::verify_invalid_ports<struct st30_rx_ops, st30_rx_handle, st30_rx_create,
                                 st30_rx_free>(kRxOpsInit);
}
TEST(St30_rx, create_expect_fail_ring_sz) {
  for (uint16_t ring_size : {static_cast<uint16_t>(0), static_cast<uint16_t>(128 + 1)}) {
    mtl_test::verify_rtp_ring_invalid<struct st30_rx_ops, st30_rx_handle, st30_rx_create,
                                      st30_rx_free>(kRxOpsInit, ST30_TYPE_RTP_LEVEL,
                                                    ring_size);
  }
}

static int tx_audio_frame_done(void* priv, uint16_t /*frame_idx*/,
                               struct st30_tx_frame_meta* meta) {
  auto ctx = static_cast<tests_context*>(priv);
  if (!ctx || !__atomic_load_n(&ctx->handle, __ATOMIC_ACQUIRE)) return -EIO;
  if (!meta || meta->tfmt != ST10_TIMESTAMP_FMT_TAI || meta->timestamp == 0) {
    __atomic_fetch_add(&ctx->incomplete_frame_cnt, 1, __ATOMIC_RELAXED);
  }
  __atomic_fetch_add(&ctx->fb_send_done, 1, __ATOMIC_RELAXED);
  return 0;
}

static void st30_tx_fps_test(const std::vector<St30SessionSpec>& specs) {
  auto ctx = st_test_ctx();
  auto m_handle = ctx->handle;
  const size_t sessions = specs.size();

  std::vector<mtl_test::ScopedContext> tx_ctxs;
  std::vector<tests_context*> raw_tx;
  std::vector<ScopedSt30Tx> tx_handles;
  mtl_test::ScopedMtlStart mtl_guard(m_handle);
  std::vector<mtl_test::ScopedWorker> tx_workers;
  std::vector<double> expect_framerate(sessions, 0.0);

  for (size_t i = 0; i < sessions; i++) {
    expect_framerate[i] = st30_expected_fps(specs[i].ptime);
    tx_ctxs.push_back(mtl_test::make_context(ctx, static_cast<int>(i), 3));
    raw_tx.push_back(tx_ctxs.back().get());

    struct st30_tx_ops ops;
    st30_tx_ops_init(raw_tx[i], &ops, specs[i]);
    if (specs[i].type == ST30_TYPE_FRAME_LEVEL) {
      ops.notify_frame_done = tx_audio_frame_done;
    }
    ASSERT_GT(ops.framebuff_size, 0u);

    ScopedSt30Tx handle(st30_tx_create(m_handle, &ops));
    ASSERT_TRUE(static_cast<bool>(handle));
    __atomic_store_n(&raw_tx[i]->handle, handle.get(), __ATOMIC_RELEASE);
    tx_handles.push_back(std::move(handle));
    if (specs[i].type == ST30_TYPE_RTP_LEVEL) {
      tx_workers.emplace_back(raw_tx[i], std::thread(tx_feed_packet, raw_tx[i]));
    }
  }

  struct mtl_port_status port_before;
  memset(&port_before, 0, sizeof(port_before));
  EXPECT_EQ(mtl_get_port_stats(m_handle, MTL_PORT_P, &port_before), 0);

  EXPECT_GE(mtl_guard.start(), 0);
  auto rate_res =
      mtl_test::measure_rate_window(raw_tx, mtl_test::tx_frames, kSt30RateWarmupFrames,
                                    kSt30RateMinDeltaFrames, kSt30RateMinWindowMs);
  EXPECT_TRUE(rate_res.warmed_up);
  EXPECT_TRUE(rate_res.complete);

  for (auto& w : tx_workers) w.stop();
  EXPECT_GE(mtl_guard.stop(), 0);

  struct mtl_port_status port_after;
  memset(&port_after, 0, sizeof(port_after));
  EXPECT_EQ(mtl_get_port_stats(m_handle, MTL_PORT_P, &port_after), 0);
  EXPECT_GT(port_after.tx_packets, port_before.tx_packets + 100u);
  EXPECT_GT(port_after.tx_bytes, port_before.tx_bytes + 10000u);

  for (size_t i = 0; i < sessions; i++) {
    const int fb_send = mtl_test::tx_frames(raw_tx[i]);
    EXPECT_GT(fb_send, 0);
    if (specs[i].type == ST30_TYPE_FRAME_LEVEL) {
      const int fb_done = mtl_test::relaxed_load(&raw_tx[i]->fb_send_done);
      EXPECT_GT(fb_done, 0);
      EXPECT_LE(fb_done, fb_send);
      EXPECT_EQ(mtl_test::relaxed_load(&raw_tx[i]->incomplete_frame_cnt), 0);
    }
    struct st30_tx_user_stats tx_stats;
    memset(&tx_stats, 0, sizeof(tx_stats));
    EXPECT_EQ(st30_tx_get_session_stats(tx_handles[i].get(), &tx_stats), 0);
    EXPECT_GT(tx_stats.common.port[MTL_SESSION_PORT_P].packets, 0u);
    info("%s, session %zu fb_send %d framerate %f\n", __func__, i, fb_send,
         rate_res.rate[i]);
    EXPECT_NEAR(rate_res.rate[i], expect_framerate[i], expect_framerate[i] * 0.1);
  }
}

static void st30_rx_fps_test(const std::vector<St30SessionSpec>& specs,
                             const St30RxOptions& opts = {}) {
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
  std::vector<double> expect_framerate(sessions, 0.0);

  for (size_t i = 0; i < sessions; i++) {
    const auto& spec = specs[i];
    const uint16_t udp_port = ST30_TEST_UDP_PORT + static_cast<uint16_t>(i * 2);
    const uint32_t ssrc =
        (!opts.check_meta && i) ? static_cast<uint32_t>(i) + 0x66666666u : 0u;
    const uint16_t fb_cnt = opts.check_sha ? TEST_SHA_HIST_NUM : 3;

    tx_ctxs.push_back(
        mtl_test::make_context(ctx, static_cast<int>(i), fb_cnt, opts.check_sha));
    raw_tx.push_back(tx_ctxs.back().get());

    struct st30_tx_ops ops_tx;
    st30_tx_ops_init(raw_tx[i], &ops_tx, spec, /*loopback=*/true, udp_port, ssrc);
    const int expect_pkt_sz = expected_st30_wire_pkt_size(spec);
    ASSERT_GT(expect_pkt_sz, 0);
    EXPECT_EQ(ops_tx.framebuff_size, static_cast<uint32_t>(expect_pkt_sz));
    if (opts.user_timestamp) {
      ops_tx.get_next_frame = tx_audio_next_frame_timestamp;
      ops_tx.flags |= ST30_TX_FLAG_USER_PACING;
    }
    raw_tx[i]->frame_time = st30_get_packet_time(ops_tx.ptime);

    ScopedSt30Tx handle(st30_tx_create(m_handle, &ops_tx));
    ASSERT_TRUE(static_cast<bool>(handle));

    if (opts.check_sha) {
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
        SHA256(fb, ops_tx.framebuff_size, raw_tx[i]->shas[frame]);
        test_sha_dump("st30_rx", raw_tx[i]->shas[frame]);
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
    const uint32_t ssrc =
        (!opts.check_meta && i) ? static_cast<uint32_t>(i) + 0x66666666u : 0u;

    rx_ctxs.push_back(
        mtl_test::make_context(ctx, static_cast<int>(i), 3, opts.check_sha));
    raw_rx.push_back(rx_ctxs.back().get());

    struct st30_rx_ops ops_rx;
    st30_rx_ops_init(raw_rx[i], &ops_rx, spec, /*loopback=*/true, udp_port, ssrc);
    if (opts.check_meta) ops_rx.notify_frame_ready = st30_rx_meta_frame_ready;
    expect_framerate[i] =
        st30_expected_fps(ops_rx.ptime) / (opts.user_timestamp ? 2.0 : 1.0);

    ScopedSt30Rx handle(st30_rx_create(m_handle, &ops_rx));
    ASSERT_TRUE(static_cast<bool>(handle));

    raw_rx[i]->frame_size = ops_rx.framebuff_size;
    if (opts.check_sha) {
      memcpy(raw_rx[i]->shas, raw_tx[i]->shas, TEST_SHA_HIST_NUM * SHA256_DIGEST_LENGTH);
    }
    if (opts.check_meta) {
      auto meta = static_cast<struct st30_rx_frame_meta*>(
          st_test_zmalloc(sizeof(struct st30_rx_frame_meta)));
      ASSERT_TRUE(meta != nullptr);
      meta->channel = ops_rx.channel;
      meta->sampling = ops_rx.sampling;
      meta->fmt = ops_rx.fmt;
      raw_rx[i]->priv = meta; /* Owned and freed by ScopedContext. */
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
    EXPECT_EQ(mtl_test::relaxed_load(&raw_rx[i]->sha_fail_cnt), 0);
    if (opts.check_sha) {
      EXPECT_GT(mtl_test::sha_checked_frames(raw_rx[i]), 0);
    }
    if (opts.check_meta) {
      EXPECT_EQ(mtl_test::relaxed_load(&raw_rx[i]->rx_meta_fail_cnt), 0);
    }
    if (opts.check_zero_loss) {
      struct st30_rx_user_stats rx_stats;
      memset(&rx_stats, 0, sizeof(rx_stats));
      EXPECT_EQ(st30_rx_get_session_stats(rx_handles[i].get(), &rx_stats), 0);
      EXPECT_EQ(rx_stats.common.stat_lost_packets, 0u);
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
  EXPECT_EQ(st30_get_packet_size(ST30_FMT_PCM16, ST30_PTIME_1MS, ST30_SAMPLING_48K, 2),
            192);
  EXPECT_EQ(st30_get_packet_size(ST30_FMT_PCM24, ST30_PTIME_1MS, ST30_SAMPLING_48K, 2),
            288);
  st30_rx_fps_test(
      {
          {ST30_TYPE_RTP_LEVEL, ST30_SAMPLING_96K, ST30_PTIME_1MS, 1, ST30_FMT_PCM16},
          {ST30_TYPE_FRAME_LEVEL, ST30_SAMPLING_48K, ST30_PTIME_1MS, 4, ST30_FMT_PCM8},
      },
      {/*check_sha=*/true, /*check_meta=*/false, /*user_timestamp=*/false,
       /*check_zero_loss=*/true});
}
TEST(St30_rx, digest_st31_mix) {
  EXPECT_EQ(st30_get_packet_size(ST31_FMT_AM824, ST30_PTIME_1MS, ST30_SAMPLING_96K, 2),
            768);
  EXPECT_EQ(st30_get_packet_size(ST31_FMT_AM824, ST30_PTIME_1MS, ST30_SAMPLING_48K, 2),
            384);
  st30_rx_fps_test(
      {
          {ST30_TYPE_FRAME_LEVEL, ST30_SAMPLING_96K, ST30_PTIME_1MS, 2, ST31_FMT_AM824},
          {ST30_TYPE_FRAME_LEVEL, ST30_SAMPLING_48K, ST30_PTIME_1MS, 2, ST31_FMT_AM824},
          {ST30_TYPE_RTP_LEVEL, ST30_SAMPLING_96K, ST30_PTIME_1MS, 2, ST31_FMT_AM824},
          {ST30_TYPE_RTP_LEVEL, ST30_SAMPLING_48K, ST30_PTIME_1MS, 2, ST31_FMT_AM824},
      },
      {/*check_sha=*/true, /*check_meta=*/false, /*user_timestamp=*/false,
       /*check_zero_loss=*/true});
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
      {/*check_sha=*/true});
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
      {/*check_sha=*/true});
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
      {/*check_sha=*/true});
}
/* ST 2110-30:2017 §7 Table 2 (p7): 96 kHz at 125 us is required for levels BX and CX. */
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
      {/*check_sha=*/true});
}
/* Sub-0.5ms RTP-level audio exercises the RTP tasklet together with RL pacing
 * (ST30_TX_PACING_WAY_AUTO selects RL below 0.5 ms). */
TEST(St30_rx, rtp_digest_sub_ms_ptime_mix_s2) {
  st30_rx_fps_test(
      {
          {ST30_TYPE_RTP_LEVEL, ST30_SAMPLING_96K, ST30_PTIME_125US, 2, ST30_FMT_PCM24},
          {ST30_TYPE_RTP_LEVEL, ST30_SAMPLING_48K, ST30_PTIME_250US, 2, ST30_FMT_PCM16},
      },
      {/*check_sha=*/true});
}
TEST(St30_rx, frame_meta_pcm16_48k_2ch_s1) {
  st30_rx_fps_test(
      {{ST30_TYPE_FRAME_LEVEL, ST30_SAMPLING_48K, ST30_PTIME_1MS, 2, ST30_FMT_PCM16}},
      {/*check_sha=*/false, /*check_meta=*/true, /*user_timestamp=*/false});
}
TEST(St30_rx, frame_user_timestamp) {
  st30_rx_fps_test(
      {{ST30_TYPE_FRAME_LEVEL, ST30_SAMPLING_48K, ST30_PTIME_1MS, 2, ST30_FMT_PCM16}},
      {/*check_sha=*/false, /*check_meta=*/true, /*user_timestamp=*/true});
}

/* Exercises both st30_rx_update_source (across two TX sessions) and
 * st30_tx_update_destination (relocating TX0 to multicast and back). */
static void st30_update_src_and_dst_test(enum st30_type type) {
  auto ctx = st_test_ctx();
  auto m_handle = ctx->handle;
  MTL_TEST_REQUIRE_PORTS(ctx, 2);

  const double expect_framerate = 1000.0;
  const St30SessionSpec spec{type, ST30_SAMPLING_48K, ST30_PTIME_1MS, 2, ST30_FMT_PCM24};

  std::vector<mtl_test::ScopedContext> tx_ctxs;
  std::vector<tests_context*> raw_tx;
  auto rx_ctx = mtl_test::make_context(ctx, 0, 3);
  St30SwitchState switch_state;
  switch_state.ctx = rx_ctx.get();
  std::vector<ScopedSt30Tx> tx_handles;
  ScopedSt30Rx rx_handle;
  mtl_test::ScopedMtlStart mtl_guard(m_handle);
  std::vector<mtl_test::ScopedWorker> tx_workers;
  mtl_test::ScopedWorker rx_worker;

  for (int i = 0; i < 2; i++) {
    tx_ctxs.push_back(mtl_test::make_context(ctx, i, 3));
    raw_tx.push_back(tx_ctxs.back().get());

    struct st30_tx_ops ops_tx;
    st30_tx_ops_init(raw_tx[i], &ops_tx, spec, /*loopback=*/true,
                     ST30_TEST_UDP_PORT + static_cast<uint16_t>(i * 2),
                     0x66666666u + static_cast<uint32_t>(i));
    if (i == 1) {
      memcpy(ops_tx.dip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
             MTL_IP_ADDR_LEN);
    }

    ScopedSt30Tx handle(st30_tx_create(m_handle, &ops_tx));
    ASSERT_TRUE(static_cast<bool>(handle));
    if (type == ST30_TYPE_FRAME_LEVEL) {
      for (uint16_t fb_i = 0; fb_i < ops_tx.framebuff_cnt; fb_i++) {
        void* fb = st30_tx_get_framebuffer(handle.get(), fb_i);
        ASSERT_NE(fb, nullptr);
        memset(fb, kSt30SwitchPattern[i], ops_tx.framebuff_size);
      }
    }
    __atomic_store_n(&raw_tx[i]->handle, handle.get(), __ATOMIC_RELEASE);
    tx_handles.push_back(std::move(handle));
    if (type == ST30_TYPE_RTP_LEVEL) {
      tx_workers.emplace_back(raw_tx[i], std::thread(tx_feed_packet, raw_tx[i]));
    }
  }

  struct st30_rx_ops ops_rx;
  st30_rx_ops_init(rx_ctx.get(), &ops_rx, spec, /*loopback=*/true, ST30_TEST_UDP_PORT);
  switch_state.frame_size = ops_rx.framebuff_size;
  if (type == ST30_TYPE_FRAME_LEVEL) {
    ops_rx.priv = &switch_state;
    ops_rx.notify_frame_ready = st30_switch_rx_frame_ready;
  }
  rx_handle.reset(st30_rx_create(m_handle, &ops_rx));
  ASSERT_TRUE(static_cast<bool>(rx_handle));
  __atomic_store_n(&rx_ctx->handle, rx_handle.get(), __ATOMIC_RELEASE);
  switch_state.rx_handle.store(rx_handle.get(), std::memory_order_release);
  if (type == ST30_TYPE_RTP_LEVEL) {
    rx_worker = mtl_test::ScopedWorker(
        rx_ctx.get(), std::thread(st30_switch_rx_get_packet, &switch_state));
  }

  EXPECT_GE(mtl_guard.start(), 0);
  std::vector<tests_context*> raw_rx{rx_ctx.get()};
  EXPECT_TRUE(mtl_test::wait_for_all(raw_rx, mtl_test::rx_frames, kSt30PhaseTargetFrames,
                                     kSt30PhaseTimeoutMs));
  EXPECT_GT(switch_state.rx_from_tx[0].load(std::memory_order_relaxed), 5) << "initial";
  EXPECT_EQ(switch_state.rx_from_tx[1].load(std::memory_order_relaxed), 0) << "initial";
  EXPECT_EQ(switch_state.rx_from_other.load(std::memory_order_relaxed), 0) << "initial";

  auto verify_switched = [&](const char* label, const uint8_t* src_ip, uint16_t udp_port,
                             int expected_tx_idx) {
    struct st_rx_source_info src;
    memset(&src, 0, sizeof(src));
    src.udp_port[MTL_SESSION_PORT_P] = udp_port;
    memcpy(src.ip_addr[MTL_SESSION_PORT_P], src_ip, MTL_IP_ADDR_LEN);
    EXPECT_GE(st30_rx_update_source(rx_handle.get(), &src), 0);

    st_usleep(150 * 1000);
    switch_state.reset();
    const int base = mtl_test::rx_frames(rx_ctx.get());
    auto delta = [base](const tests_context* c) { return mtl_test::rx_frames(c) - base; };
    auto rate_res =
        mtl_test::measure_rate_window(raw_rx, delta, kSt30RateWarmupFrames,
                                      kSt30RateMinDeltaFrames, kSt30RateMinWindowMs);
    EXPECT_TRUE(rate_res.warmed_up) << label;
    EXPECT_TRUE(rate_res.complete) << label;
    EXPECT_GT(mtl_test::rx_frames(rx_ctx.get()), base);
    EXPECT_NEAR(rate_res.rate[0], expect_framerate, expect_framerate * 0.1) << label;
    EXPECT_GT(switch_state.rx_from_tx[expected_tx_idx].load(std::memory_order_relaxed),
              10)
        << label;
    EXPECT_EQ(
        switch_state.rx_from_tx[1 - expected_tx_idx].load(std::memory_order_relaxed), 0)
        << label;
    EXPECT_EQ(switch_state.rx_from_other.load(std::memory_order_relaxed), 0) << label;
  };

  /* Phase 1: RX switches source to separate TX1 multicast stream. */
  __atomic_store_n(&raw_tx[1]->seq_id, 0, __ATOMIC_RELAXED);
  verify_switched("rx_src_mcast1", ctx->mcast_ip_addr[MTL_PORT_P], ST30_TEST_UDP_PORT + 2,
                  /*expected_tx_idx=*/1);

  /* Phase 2: TX0 updates destination to mcast R and RX follows. */
  __atomic_store_n(&raw_tx[0]->seq_id, 0, __ATOMIC_RELAXED);
  struct st_tx_dest_info dst;
  memset(&dst, 0, sizeof(dst));
  dst.udp_port[MTL_SESSION_PORT_P] = ST30_TEST_UDP_PORT + 4;
  memcpy(dst.dip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_R],
         MTL_IP_ADDR_LEN);
  EXPECT_GE(st30_tx_update_destination(tx_handles[0].get(), &dst), 0);
  verify_switched("tx_dst_mcast2", ctx->mcast_ip_addr[MTL_PORT_R], ST30_TEST_UDP_PORT + 4,
                  /*expected_tx_idx=*/0);

  /* Phase 3: TX0 and RX return to initial unicast. */
  __atomic_store_n(&raw_tx[0]->seq_id, rand(), __ATOMIC_RELAXED);
  memset(&dst, 0, sizeof(dst));
  dst.udp_port[MTL_SESSION_PORT_P] = ST30_TEST_UDP_PORT;
  memcpy(dst.dip_addr[MTL_SESSION_PORT_P], ctx->para.sip_addr[MTL_PORT_R],
         MTL_IP_ADDR_LEN);
  EXPECT_GE(st30_tx_update_destination(tx_handles[0].get(), &dst), 0);
  verify_switched("back_to_init", ctx->para.sip_addr[MTL_PORT_P], ST30_TEST_UDP_PORT,
                  /*expected_tx_idx=*/0);

  rx_worker.stop();
  for (auto& w : tx_workers) w.stop();
  EXPECT_GE(mtl_guard.stop(), 0);
}

TEST(St30_rx, update_source_and_dest_frame) {
  st30_update_src_and_dst_test(ST30_TYPE_FRAME_LEVEL);
}
TEST(St30_rx, update_source_and_dest_rtp) {
  st30_update_src_and_dst_test(ST30_TYPE_RTP_LEVEL);
}

TEST(St30_rx, after_start_mix_s2_r2) {
  auto ctx = st_test_ctx();
  auto m_handle = ctx->handle;
  MTL_TEST_REQUIRE_PORTS(ctx, 2);

  const std::vector<St30SessionSpec> specs = {
      {ST30_TYPE_FRAME_LEVEL, ST30_SAMPLING_96K, ST30_PTIME_1MS, 1, ST30_FMT_PCM24},
      {ST30_TYPE_RTP_LEVEL, ST30_SAMPLING_48K, ST30_PTIME_1MS, 2, ST30_FMT_PCM16},
  };
  const size_t sessions = specs.size();

  mtl_test::ScopedMtlStart mtl_guard(m_handle);
  EXPECT_GE(mtl_guard.start(), 0);

  for (int r = 0; r < 2; r++) {
    std::vector<mtl_test::ScopedContext> tx_ctxs, rx_ctxs;
    std::vector<tests_context*> raw_tx, raw_rx;
    std::vector<ScopedSt30Tx> tx_handles;
    std::vector<ScopedSt30Rx> rx_handles;
    std::vector<mtl_test::ScopedWorker> tx_workers, rx_workers;

    for (size_t i = 0; i < sessions; i++) {
      const uint16_t udp_port = ST30_TEST_UDP_PORT + static_cast<uint16_t>(i * 2);
      tx_ctxs.push_back(mtl_test::make_context(ctx, static_cast<int>(i), 3));
      raw_tx.push_back(tx_ctxs.back().get());
      struct st30_tx_ops ops_tx;
      st30_tx_ops_init(raw_tx[i], &ops_tx, specs[i], /*loopback=*/true, udp_port);
      ScopedSt30Tx handle(st30_tx_create(m_handle, &ops_tx));
      ASSERT_TRUE(static_cast<bool>(handle));
      __atomic_store_n(&raw_tx[i]->handle, handle.get(), __ATOMIC_RELEASE);
      tx_handles.push_back(std::move(handle));
      if (specs[i].type == ST30_TYPE_RTP_LEVEL) {
        tx_workers.emplace_back(raw_tx[i], std::thread(tx_feed_packet, raw_tx[i]));
      }
    }

    for (size_t i = 0; i < sessions; i++) {
      const uint16_t udp_port = ST30_TEST_UDP_PORT + static_cast<uint16_t>(i * 2);
      rx_ctxs.push_back(mtl_test::make_context(ctx, static_cast<int>(i), 3));
      raw_rx.push_back(rx_ctxs.back().get());
      struct st30_rx_ops ops_rx;
      st30_rx_ops_init(raw_rx[i], &ops_rx, specs[i], /*loopback=*/true, udp_port);
      ScopedSt30Rx handle(st30_rx_create(m_handle, &ops_rx));
      ASSERT_TRUE(static_cast<bool>(handle));
      __atomic_store_n(&raw_rx[i]->handle, handle.get(), __ATOMIC_RELEASE);
      rx_handles.push_back(std::move(handle));
      if (specs[i].type == ST30_TYPE_RTP_LEVEL) {
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
      const double expect_fps = st30_expected_fps(specs[i].ptime);
      EXPECT_GT(mtl_test::rx_frames(raw_rx[i]), 0);
      EXPECT_NEAR(rate_res.rate[i], expect_fps, expect_fps * 0.1);
    }
  }

  EXPECT_GE(mtl_guard.stop(), 0);
}

TEST(St30_rx, session_stats_reset) {
  auto ctx = st_test_ctx();
  auto m_handle = ctx->handle;
  MTL_TEST_REQUIRE_PORTS(ctx, 2);

  const St30SessionSpec spec;
  const uint16_t udp_port = ST30_TEST_UDP_PORT + 50;
  auto tx_ctx = mtl_test::make_context(ctx, 0, 3);
  auto rx_ctx = mtl_test::make_context(ctx, 0, 3);
  ScopedSt30Tx tx_handle;
  ScopedSt30Rx rx_handle;
  mtl_test::ScopedMtlStart mtl_guard(m_handle);

  struct st30_tx_ops ops_tx;
  st30_tx_ops_init(tx_ctx.get(), &ops_tx, spec, /*loopback=*/true, udp_port, 0,
                   "st30_stats_tx");
  tx_handle.reset(st30_tx_create(m_handle, &ops_tx));
  ASSERT_TRUE(static_cast<bool>(tx_handle));
  __atomic_store_n(&tx_ctx->handle, tx_handle.get(), __ATOMIC_RELEASE);

  struct st30_rx_ops ops_rx;
  st30_rx_ops_init(rx_ctx.get(), &ops_rx, spec, /*loopback=*/true, udp_port, 0,
                   "st30_stats_rx");
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
  EXPECT_TRUE(mtl_test::wait_until(
      [&]() { return mtl_test::rx_frames(rx_ctx.get()) >= kSt30PhaseTargetFrames; },
      kSt30PhaseTimeoutMs));
  EXPECT_GE(mtl_guard.stop(), 0);

  /* Free TX before resetting stats so no packets arrive under DEV_AUTO_START_STOP.
   * Note: st30_rx_put_framebuff() is not asserted on invalid inputs because
   * rx_audio_session_put_frame() unconditionally returns -EIO in the library. */
  EXPECT_EQ(st30_tx_get_session_stats(tx_handle.get(), &tx_stats), 0);
  EXPECT_GT(tx_stats.common.port[MTL_SESSION_PORT_P].packets, 0u);
  EXPECT_GT(tx_stats.common.port[MTL_SESSION_PORT_P].bytes, 0u);
  EXPECT_GT(tx_stats.common.port[MTL_SESSION_PORT_P].frames, 0u);
  EXPECT_EQ(st30_tx_reset_session_stats(tx_handle.get()), 0);
  EXPECT_EQ(st30_tx_get_session_stats(tx_handle.get(), &tx_stats), 0);
  EXPECT_EQ(tx_stats.common.port[MTL_SESSION_PORT_P].packets, 0u);
  EXPECT_EQ(tx_stats.common.port[MTL_SESSION_PORT_P].bytes, 0u);
  EXPECT_EQ(tx_stats.common.port[MTL_SESSION_PORT_P].frames, 0u);

  tx_handle.reset();
  st_usleep(20 * 1000);

  EXPECT_EQ(st30_rx_get_session_stats(rx_handle.get(), &rx_stats), 0);
  EXPECT_GT(rx_stats.common.port[MTL_SESSION_PORT_P].packets, 0u);
  EXPECT_GT(rx_stats.common.port[MTL_SESSION_PORT_P].bytes, 0u);
  EXPECT_GT(rx_stats.common.port[MTL_SESSION_PORT_P].frames, 0u);
  EXPECT_GT(rx_stats.common.stat_pkts_received, 0u);

  EXPECT_EQ(st30_rx_reset_session_stats(rx_handle.get()), 0);
  EXPECT_EQ(st30_rx_get_session_stats(rx_handle.get(), &rx_stats), 0);
  EXPECT_EQ(rx_stats.common.port[MTL_SESSION_PORT_P].packets, 0u);
  EXPECT_EQ(rx_stats.common.port[MTL_SESSION_PORT_P].bytes, 0u);
  EXPECT_EQ(rx_stats.common.port[MTL_SESSION_PORT_P].frames, 0u);
  EXPECT_EQ(rx_stats.common.stat_pkts_received, 0u);
}

TEST(St30_rx, timing_parser_meta) {
  auto ctx = st_test_ctx();
  auto m_handle = ctx->handle;
  MTL_TEST_REQUIRE_PORTS(ctx, 2);

  const St30SessionSpec spec;
  const uint16_t udp_port = ST30_TEST_UDP_PORT + 60;

  /* Negative check: TIMING_PARSER_META requires notify_timing_parser_result. */
  {
    auto neg_ctx = mtl_test::make_context(ctx, 0, 3);
    struct st30_rx_ops neg_ops;
    st30_rx_ops_init(neg_ctx.get(), &neg_ops, spec, /*loopback=*/true, udp_port, 0,
                     "st30_tp_neg");
    neg_ops.flags |= ST30_RX_FLAG_TIMING_PARSER_META;
    neg_ops.notify_timing_parser_result = nullptr;
    EXPECT_FALSE(static_cast<bool>(ScopedSt30Rx(st30_rx_create(m_handle, &neg_ops))));
  }

  St30TpState tp_state;
  auto tx_ctx = mtl_test::make_context(ctx, 0, 3);
  auto rx_ctx = mtl_test::make_context(ctx, 0, 3);
  ScopedSt30Tx tx_handle;
  ScopedSt30Rx rx_handle;
  mtl_test::ScopedMtlStart mtl_guard(m_handle);
  tp_state.ctx = rx_ctx.get();

  struct st30_tx_ops ops_tx;
  st30_tx_ops_init(tx_ctx.get(), &ops_tx, spec, /*loopback=*/true, udp_port, 0,
                   "st30_tp_tx");
  tx_handle.reset(st30_tx_create(m_handle, &ops_tx));
  ASSERT_TRUE(static_cast<bool>(tx_handle));
  __atomic_store_n(&tx_ctx->handle, tx_handle.get(), __ATOMIC_RELEASE);

  struct st30_rx_ops ops_rx;
  st30_rx_ops_init(rx_ctx.get(), &ops_rx, spec, /*loopback=*/true, udp_port, 0,
                   "st30_tp_rx");
  ops_rx.priv = &tp_state;
  ops_rx.notify_frame_ready = st30_rx_tp_frame_ready;
  ops_rx.notify_timing_parser_result = st30_rx_tp_notify;
  ops_rx.flags |= ST30_RX_FLAG_TIMING_PARSER_META;
  rx_handle.reset(st30_rx_create(m_handle, &ops_rx));
  ASSERT_TRUE(static_cast<bool>(rx_handle));
  tp_state.rx_handle.store(rx_handle.get(), std::memory_order_release);

  EXPECT_GE(mtl_guard.start(), 0);
  /* Timing parser reports every 200ms; wait for >= 3 reports (~600ms) for steady state.
   */
  EXPECT_TRUE(mtl_test::wait_until(
      [&]() { return tp_state.cb_cnt.load(std::memory_order_relaxed) >= 3; },
      kSt30PhaseTimeoutMs));
  EXPECT_GE(mtl_guard.stop(), 0);

  EXPECT_GE(tp_state.cb_cnt.load(std::memory_order_relaxed), 3);
  EXPECT_GE(tp_state.steady_cnt.load(std::memory_order_relaxed), 2);
  EXPECT_EQ(tp_state.bad_port_cnt.load(std::memory_order_relaxed), 0);
  EXPECT_EQ(tp_state.null_tp_cnt.load(std::memory_order_relaxed), 0);
  EXPECT_EQ(tp_state.zero_pkts_cnt.load(std::memory_order_relaxed), 0);
  EXPECT_EQ(tp_state.pkts_range_fail_cnt.load(std::memory_order_relaxed), 0);
  EXPECT_EQ(tp_state.ipt_avg_fail_cnt.load(std::memory_order_relaxed), 0);
  EXPECT_EQ(tp_state.ipt_order_fail_cnt.load(std::memory_order_relaxed), 0);
  EXPECT_EQ(tp_state.dpvr_order_fail_cnt.load(std::memory_order_relaxed), 0);
  EXPECT_EQ(tp_state.tsdf_fail_cnt.load(std::memory_order_relaxed), 0);
  EXPECT_EQ(tp_state.non_compliant_cnt.load(std::memory_order_relaxed), 0);
  EXPECT_GT(mtl_test::rx_frames(rx_ctx.get()), 0);
}

/* Exercises all ST 2110-30 TX pacing ways (AUTO, RL, TSC) in real loopback without
 * needing a global CLI option. */
TEST(St30_tx, audio_pacing_ways) {
  auto ctx = st_test_ctx();
  auto m_handle = ctx->handle;
  MTL_TEST_REQUIRE_PORTS(ctx, 2);

  struct PacingCase {
    enum st30_tx_pacing_way way;
    const char* name;
    uint16_t udp_port;
  };
  const PacingCase cases[] = {
      {ST30_TX_PACING_WAY_AUTO, "auto", ST30_TEST_UDP_PORT + 70},
      {ST30_TX_PACING_WAY_TSC, "tsc", ST30_TEST_UDP_PORT + 72},
      {ST30_TX_PACING_WAY_RL, "rl", ST30_TEST_UDP_PORT + 74},
  };

  const St30SessionSpec spec;
  const double expect_fps = st30_expected_fps(spec.ptime);

  for (const auto& pc : cases) {
    SCOPED_TRACE(pc.name);
    auto tx_ctx = mtl_test::make_context(ctx, 0, 3);
    auto rx_ctx = mtl_test::make_context(ctx, 0, 3);
    const uint8_t pattern = static_cast<uint8_t>(0x30 + static_cast<int>(pc.way));
    std::atomic<int> payload_mismatch{0};

    struct st30_tx_ops ops_tx;
    st30_tx_ops_init(tx_ctx.get(), &ops_tx, spec, /*loopback=*/true, pc.udp_port, 0,
                     pc.name);
    ops_tx.pacing_way = pc.way;

    ScopedSt30Tx tx_handle(st30_tx_create(m_handle, &ops_tx));
    ASSERT_TRUE(static_cast<bool>(tx_handle)) << "tx create failed for " << pc.name;
    for (uint16_t fb_i = 0; fb_i < ops_tx.framebuff_cnt; fb_i++) {
      void* fb = st30_tx_get_framebuffer(tx_handle.get(), fb_i);
      ASSERT_NE(fb, nullptr);
      memset(fb, pattern, ops_tx.framebuff_size);
    }
    __atomic_store_n(&tx_ctx->handle, tx_handle.get(), __ATOMIC_RELEASE);

    struct RxCheckPriv {
      tests_context* ctx;
      st30_rx_handle rx_handle;
      uint8_t pattern;
      uint32_t frame_size;
      std::atomic<int>* mismatch;
    } rx_priv{rx_ctx.get(), nullptr, pattern, ops_tx.framebuff_size, &payload_mismatch};

    struct st30_rx_ops ops_rx;
    st30_rx_ops_init(rx_ctx.get(), &ops_rx, spec, /*loopback=*/true, pc.udp_port, 0,
                     pc.name);
    ops_rx.priv = &rx_priv;
    ops_rx.notify_frame_ready = [](void* priv, void* frame,
                                   struct st30_rx_frame_meta* meta) -> int {
      auto* p = static_cast<RxCheckPriv*>(priv);
      auto* bytes = static_cast<const uint8_t*>(frame);
      if (!meta || meta->frame_recv_size != p->frame_size) {
        p->mismatch->fetch_add(1, std::memory_order_relaxed);
      } else {
        for (uint32_t b = 0; b < p->frame_size; b++) {
          if (bytes[b] != p->pattern) {
            p->mismatch->fetch_add(1, std::memory_order_relaxed);
            break;
          }
        }
      }
      if (p->rx_handle) (void)st30_rx_put_framebuff(p->rx_handle, frame);
      __atomic_fetch_add(&p->ctx->fb_rec, 1, __ATOMIC_RELAXED);
      return 0;
    };

    ScopedSt30Rx rx_handle(st30_rx_create(m_handle, &ops_rx));
    ASSERT_TRUE(static_cast<bool>(rx_handle)) << "rx create failed for " << pc.name;
    rx_priv.rx_handle = rx_handle.get();
    __atomic_store_n(&rx_ctx->handle, rx_handle.get(), __ATOMIC_RELEASE);

    mtl_test::ScopedMtlStart mtl_guard(m_handle);
    ASSERT_GE(mtl_guard.start(), 0);
    std::vector<tests_context*> raw_rx{rx_ctx.get()};
    auto rate_res =
        mtl_test::measure_rate_window(raw_rx, mtl_test::rx_frames, kSt30RateWarmupFrames,
                                      kSt30RateMinDeltaFrames, kSt30RateMinWindowMs);
    EXPECT_TRUE(rate_res.warmed_up);
    EXPECT_TRUE(rate_res.complete);
    EXPECT_GE(mtl_guard.stop(), 0);

    struct st30_tx_user_stats tx_stats;
    memset(&tx_stats, 0, sizeof(tx_stats));
    EXPECT_EQ(st30_tx_get_session_stats(tx_handle.get(), &tx_stats), 0);
    if (pc.way == ST30_TX_PACING_WAY_RL) {
      EXPECT_GT(tx_stats.stat_pkts_burst, 0u);
      EXPECT_GT(tx_stats.stat_pad_pkts_burst, 0u);
      EXPECT_GT(tx_stats.stat_warmup_pkts_burst, 0u);
    } else {
      EXPECT_EQ(tx_stats.stat_pkts_burst, 0u);
      EXPECT_EQ(tx_stats.stat_pad_pkts_burst, 0u);
      EXPECT_EQ(tx_stats.stat_warmup_pkts_burst, 0u);
    }

    rx_handle.reset();
    tx_handle.reset();

    EXPECT_GT(mtl_test::rx_frames(rx_ctx.get()), 0);
    EXPECT_EQ(payload_mismatch.load(std::memory_order_relaxed), 0);
    EXPECT_NEAR(rate_res.rate[0], expect_fps, expect_fps * 0.15);
  }
}
