/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <new>
#include <thread>
#include <vector>

#include "log.h"
#include "test_harness.hpp"
#include "tests.hpp"

#define ST40_TEST_PAYLOAD_TYPE (113)
#define ST40_TEST_FMD_PAYLOAD_TYPE (115) /* the library default for fast metadata */
#define ST40_TEST_FMD_DIT (0x2a5a5a)
#define ST40_TEST_FMD_K_BIT (1)
#define ST40_TEST_FMD_PADDED(size) (((size) + 3) & ~3)
/* ST 2110-10 Standard UDP Size Limit minus the UDP header */
#define ST40_TEST_FMD_MAX_RTP_BYTES (1452)
#define ST40_TEST_FMD_MAX_GAP_NS (500 * (uint64_t)NS_PER_MS)

namespace {

using ScopedSt40Tx = mtl_test::ScopedHandle<st40_tx_handle, st40_tx_free>;
using ScopedSt40Rx = mtl_test::ScopedHandle<st40_rx_handle, st40_rx_free>;

/* Frees all frame_buf slots (including fb_cnt == 3) and drains buf_q before delete. */
struct St40ContextDeleter {
  void operator()(tests_context* c) const {
    if (!c) return;
    for (int i = 0; i < TEST_MAX_SHA_HIST_NUM; i++) {
      if (c->frame_buf[i]) {
        st_test_free(c->frame_buf[i]);
        c->frame_buf[i] = nullptr;
      }
    }
    while (!c->buf_q.empty()) {
      st_test_free(c->buf_q.front());
      c->buf_q.pop();
    }
    tests_context_unit(c);
    delete c;
  }
};
using ScopedSt40Ctx = std::unique_ptr<tests_context, St40ContextDeleter>;

ScopedSt40Ctx make_st40_ctx(struct st_tests_context* global_ctx, int idx = 0,
                            uint16_t fb_cnt = 3, bool check_sha = false) {
  ScopedSt40Ctx c(new tests_context());
  c->ctx = global_ctx;
  c->idx = idx;
  c->fb_cnt = fb_cnt;
  c->fb_idx = 0;
  c->check_sha = check_sha;
  return c;
}

struct St40SessionSpec {
  enum st40_type type = ST40_TYPE_FRAME_LEVEL;
  enum st_fps fps = ST_FPS_P59_94;
  bool fmd = false;
  uint32_t fmd_data_size = 240;
};

struct St40RxScenario {
  std::vector<St40SessionSpec> sessions;
  bool check_sha = false;
  bool user_timestamp = false;
  bool empty_frame = false;
  bool interlaced = false;
  bool dedicate_tx_queue = false;
};

} /* namespace */

static int tx_anc_next_frame(void* priv, uint16_t* next_frame_idx,
                             struct st40_tx_frame_meta* /*meta*/) {
  auto ctx = (tests_context*)priv;

  if (ctx->stop) return -EIO;
  return tx_next_frame(priv, next_frame_idx);
}

static int tx_anc_next_frame_timestamp(void* priv, uint16_t* next_frame_idx,
                                       struct st40_tx_frame_meta* meta) {
  auto ctx = (tests_context*)priv;

  if (!__atomic_load_n(&ctx->handle, __ATOMIC_ACQUIRE)) return -EIO; /* not ready */

  meta->tfmt = ST10_TIMESTAMP_FMT_TAI;
  meta->timestamp = mtl_ptp_read_time(ctx->ctx->handle) + 40 * 1000 * 1000;
  *next_frame_idx = ctx->fb_idx;
  dbg("%s, next_frame_idx %d\n", __func__, *next_frame_idx);
  ctx->fb_idx++;
  if (ctx->fb_idx >= ctx->fb_cnt) ctx->fb_idx = 0;
  __atomic_fetch_add(&ctx->fb_send, 1, __ATOMIC_RELAXED);
  if (!ctx->start_time) ctx->start_time = st_test_get_monotonic_time();
  return 0;
}

static int tx_fmd_build_rtp_packet(tests_context* s, struct st40_fmd_rtp_hdr* rtp,
                                   uint16_t* pkt_len) {
  size_t padded_size = ST40_TEST_FMD_PADDED(s->frame_size);
  uint32_t seq_id =
      static_cast<uint32_t>(__atomic_fetch_add(&s->seq_id, 1, __ATOMIC_RELAXED));

  memset(rtp, 0x0, sizeof(*rtp) + padded_size);
  rtp->base.payload_type = ST40_TEST_FMD_PAYLOAD_TYPE;
  rtp->base.version = 2;
  rtp->base.tmstamp = s->rtp_tmstamp;
  rtp->base.ssrc = htonl(0x88888888 + s->idx);
  rtp->base.seq_number = htons((uint16_t)seq_id);
  s->rtp_tmstamp++;
  rtp->fmd_hdr_chunk.data_item_type = ST40_TEST_FMD_DIT;
  rtp->fmd_hdr_chunk.data_item_k_bit = ST40_TEST_FMD_K_BIT;
  rtp->fmd_hdr_chunk.data_item_length = padded_size / 4;
  rtp->swapped_fmd_hdr_chunk = htonl(rtp->swapped_fmd_hdr_chunk);
  if (s->frame_size > 0) {
    memcpy(&rtp[1], s->frame_buf[(seq_id + 1) % TEST_SHA_HIST_NUM], s->frame_size);
  }
  *pkt_len = s->frame_size ? sizeof(*rtp) + padded_size : sizeof(rtp->base);
  return 0;
}

static void fmd_put_be32(uint8_t** p, uint32_t word) {
  word = htonl(word);
  memcpy(*p, &word, sizeof(word));
  *p += sizeof(word);
}

static uint32_t fmd_be32(const uint8_t* p) {
  uint32_t word;
  memcpy(&word, p, sizeof(word));
  return ntohl(word);
}

struct fmd_segment {
  int obj;
  uint32_t offset; /* words */
  uint32_t words;
};

/* ST 2110-41 Annex A: two objects, one per DIT, sent as segments over RTP level */
struct fmd_annex_a {
  uint32_t dit[2] = {0x000001, 0x200002};
  size_t obj_size[2] = {4099, 1501};
  std::vector<uint8_t> obj[2]; /* zero padded to a word */
  unsigned char sha[2][SHA256_DIGEST_LENGTH];
  /* one cycle of frames of packets of segments; an empty packet has zero items */
  std::vector<std::vector<std::vector<fmd_segment>>> frames;
  size_t tx_frame = 0, tx_pkt = 0;
  std::vector<uint8_t> rx_obj[2];
  int64_t rx_next[2] = {-1, -1}; /* word offset expected next, -1 until offset 0 */
  std::atomic<int> rx_done[2]{};
  std::atomic<int> rx_zero_item_pkts{0};
  std::atomic<int> rx_multi_item_pkts{0};
};

static void fmd_annex_a_init(fmd_annex_a* a) {
  /* three items of 2 + 109 words fill an MTL_PKT_MAX_RTP_BYTES packet */
  const uint32_t seg_words = 109;
  std::vector<fmd_segment> segs[2], order;

  for (int o = 0; o < 2; o++) {
    a->obj[o].assign(ST40_TEST_FMD_PADDED(a->obj_size[o]), 0);
    st_test_rand_data(a->obj[o].data(), a->obj_size[o], o);
    SHA256(a->obj[o].data(), a->obj_size[o], a->sha[o]);
    a->rx_obj[o].resize(a->obj[o].size());
    uint32_t total = a->obj[o].size() / 4;
    for (uint32_t off = 0; off < total; off += seg_words)
      segs[o].push_back({o, off, std::min(seg_words, total - off)});
  }
  /* alternate DITs, 3 items a packet, 3 packets and a zero-item one per RTP timestamp */
  for (size_t i = 0; i < std::max(segs[0].size(), segs[1].size()); i++)
    for (int o = 0; o < 2; o++)
      if (i < segs[o].size()) order.push_back(segs[o][i]);
  std::vector<std::vector<fmd_segment>> pkts;
  for (size_t i = 0; i < order.size(); i += 3)
    pkts.emplace_back(order.begin() + (ptrdiff_t)i,
                      order.begin() + (ptrdiff_t)std::min(i + 3, order.size()));
  for (size_t i = 0; i < pkts.size(); i += 3) {
    a->frames.emplace_back(pkts.begin() + (ptrdiff_t)i,
                           pkts.begin() + (ptrdiff_t)std::min(i + 3, pkts.size()));
    a->frames.back().emplace_back();
  }
}

static int tx_fmd_annex_a_build_rtp_packet(tests_context* s, uint8_t* rtp,
                                           uint16_t* pkt_len) {
  auto a = s->st40_annex_a;
  auto& frame = a->frames[a->tx_frame];
  auto hdr = (struct st_rfc3550_rtp_hdr*)rtp;
  uint8_t* p = (uint8_t*)&hdr[1];

  uint32_t seq_id =
      static_cast<uint32_t>(__atomic_fetch_add(&s->seq_id, 1, __ATOMIC_RELAXED));
  memset(hdr, 0, sizeof(*hdr));
  hdr->version = 2;
  hdr->payload_type = ST40_TEST_FMD_PAYLOAD_TYPE;
  hdr->seq_number = htons((uint16_t)seq_id);
  hdr->tmstamp = s->rtp_tmstamp; /* a new value starts a new frame in the lib */
  hdr->ssrc = htonl(0x88888888 + s->idx);
  for (auto& seg : frame[a->tx_pkt]) {
    /* Annex A says DIL - 2 segment words, §5.4 DIL excludes this word: DIL - 1 here */
    bool last = seg.offset + seg.words == a->obj[seg.obj].size() / 4;
    fmd_put_be32(&p, (a->dit[seg.obj] << 10) | ((uint32_t)last << 9) | (seg.words + 1));
    fmd_put_be32(&p, seg.offset);
    memcpy(p, &a->obj[seg.obj][(size_t)seg.offset * 4], (size_t)seg.words * 4);
    p += (size_t)seg.words * 4;
  }
  if (++a->tx_pkt == frame.size()) {
    a->tx_pkt = 0;
    a->tx_frame = (a->tx_frame + 1) % a->frames.size();
    s->rtp_tmstamp++;
  }
  *pkt_len = p - rtp;
  return 0;
}

static int tx_anc_build_rtp_packet(tests_context* s, struct st40_rfc8331_rtp_hdr* rtp,
                                   uint16_t* pkt_len) {
  if (s->st40_annex_a) return tx_fmd_annex_a_build_rtp_packet(s, (uint8_t*)rtp, pkt_len);
  if (s->st40_fmd)
    return tx_fmd_build_rtp_packet(s, (struct st40_fmd_rtp_hdr*)rtp, pkt_len);

  /* rtp hdr */
  memset(rtp, 0x0, sizeof(*rtp));
  rtp->base.marker = 1;
  rtp->first_hdr_chunk.anc_count = 0;
  rtp->base.payload_type = ST40_TEST_PAYLOAD_TYPE;
  rtp->base.version = 2;
  rtp->base.extension = 0;
  rtp->base.padding = 0;
  rtp->base.csrc_count = 0;
  rtp->first_hdr_chunk.f = 0b00;
  rtp->base.tmstamp = s->rtp_tmstamp;
  rtp->base.ssrc = htonl(0x88888888 + s->idx);
  /* update rtp seq*/
  uint32_t seq_id =
      static_cast<uint32_t>(__atomic_fetch_add(&s->seq_id, 1, __ATOMIC_RELAXED));
  rtp->base.seq_number = htons((uint16_t)seq_id);
  rtp->seq_number_ext = htons((uint16_t)(seq_id >> 16));
  s->rtp_tmstamp++;
  if (s->check_sha) {
    struct st40_rfc8331_payload_hdr* payload_hdr =
        (struct st40_rfc8331_payload_hdr*)(&rtp[1]);
    int udw_size = s->frame_size;
    payload_hdr->first_hdr_chunk.c = 0;
    payload_hdr->first_hdr_chunk.line_number = 10;
    payload_hdr->first_hdr_chunk.horizontal_offset = 0;
    payload_hdr->first_hdr_chunk.s = 0;
    payload_hdr->first_hdr_chunk.stream_num = 0;
    payload_hdr->second_hdr_chunk.did = st40_add_parity_bits(0x43);
    payload_hdr->second_hdr_chunk.sdid = st40_add_parity_bits(0x02);
    payload_hdr->second_hdr_chunk.data_count = st40_add_parity_bits(udw_size);
    st40_rfc8331_payload_hdr_bswap(payload_hdr);
    rtp->first_hdr_chunk.anc_count = 1;
    for (int i = 0; i < udw_size; i++) {
      st40_set_udw(
          i + 3, st40_add_parity_bits(s->frame_buf[(seq_id + 1) % TEST_SHA_HIST_NUM][i]),
          (uint8_t*)&payload_hdr->second_hdr_chunk);
    }
    uint16_t check_sum =
        st40_calc_checksum(3 + udw_size, (uint8_t*)&payload_hdr->second_hdr_chunk);
    st40_set_udw(udw_size + 3, check_sum, (uint8_t*)&payload_hdr->second_hdr_chunk);
    uint32_t payload_len = st40_rfc8331_payload_bytes(udw_size);
    rtp->length = htons(payload_len);
    *pkt_len = payload_len + sizeof(struct st40_rfc8331_rtp_hdr);
  } else {
    *pkt_len = sizeof(struct st40_rfc8331_rtp_hdr);
  }
  return 0;
}

static void tx_feed_packet(void* args) {
  auto ctx = (tests_context*)args;
  auto handle = (st40_tx_handle)__atomic_load_n(&ctx->handle, __ATOMIC_ACQUIRE);
  if (!handle) return;
  void* mbuf;
  void* usrptr = NULL;
  uint16_t mbuf_len = 0;
  std::unique_lock<std::mutex> lck(ctx->mtx, std::defer_lock);
  while (!ctx->stop) {
    /* get available buffer*/
    mbuf = st40_tx_get_mbuf(handle, &usrptr);
    if (!mbuf) {
      lck.lock();
      /* try again */
      mbuf = st40_tx_get_mbuf(handle, &usrptr);
      if (mbuf) {
        lck.unlock();
      } else {
        if (!ctx->stop) ctx->cv.wait(lck);
        lck.unlock();
        continue;
      }
    }

    /* build the rtp pkt */
    tx_anc_build_rtp_packet(ctx, (struct st40_rfc8331_rtp_hdr*)usrptr, &mbuf_len);
    st40_tx_put_mbuf(handle, mbuf, mbuf_len);
  }
}

static int tx_rtp_done(void* args) {
  auto ctx = (tests_context*)args;

  if (!__atomic_load_n(&ctx->handle, __ATOMIC_ACQUIRE)) return -EIO; /* not ready */

  std::unique_lock<std::mutex> lck(ctx->mtx);
  ctx->cv.notify_all();
  if (!ctx->start_time) ctx->start_time = st_test_get_monotonic_time();
  __atomic_fetch_add(&ctx->fb_send, 1, __ATOMIC_RELAXED);
  return 0;
}

static void rx_handle_rtp(tests_context* s, struct st40_rfc8331_rtp_hdr* hdr) {
  struct st40_rfc8331_payload_hdr* payload_hdr =
      (struct st40_rfc8331_payload_hdr*)(&hdr[1]);
  int anc_count = hdr->first_hdr_chunk.anc_count;

  for (int idx = 0; idx < anc_count; idx++) {
    st40_rfc8331_payload_hdr_bswap(payload_hdr);
    if (!st40_check_parity_bits(payload_hdr->second_hdr_chunk.did) ||
        !st40_check_parity_bits(payload_hdr->second_hdr_chunk.sdid) ||
        !st40_check_parity_bits(payload_hdr->second_hdr_chunk.data_count)) {
      err("anc RTP checkParityBits for payload hdr error\n");
      __atomic_fetch_add(&s->rx_meta_fail_cnt, 1, __ATOMIC_RELAXED);
      return;
    }
    int udw_size = payload_hdr->second_hdr_chunk.data_count & 0xff;

    /* verify checksum */
    uint16_t checksum =
        st40_get_udw(udw_size + 3, (uint8_t*)&payload_hdr->second_hdr_chunk);
    payload_hdr->swapped_second_hdr_chunk = htonl(payload_hdr->swapped_second_hdr_chunk);
    if (checksum !=
        st40_calc_checksum(3 + udw_size, (uint8_t*)&payload_hdr->second_hdr_chunk)) {
      __atomic_fetch_add(&s->rx_meta_fail_cnt, 1, __ATOMIC_RELAXED);
      return;
    }
    /* get payload */
    uint8_t* udw = (uint8_t*)st_test_zmalloc(udw_size);
    if (!udw) {
      __atomic_fetch_add(&s->rx_meta_fail_cnt, 1, __ATOMIC_RELAXED);
      return;
    }
    for (int i = 0; i < udw_size; i++) {
      uint16_t data = st40_get_udw(i + 3, (uint8_t*)&payload_hdr->second_hdr_chunk);
      if (!st40_check_parity_bits(data)) {
        err("anc RTP checkParityBits for udw error\n");
        __atomic_fetch_add(&s->rx_meta_fail_cnt, 1, __ATOMIC_RELAXED);
      }
      udw[i] = data & 0xff;
    }
    {
      std::unique_lock<std::mutex> lck(s->mtx);
      s->buf_q.push(udw);
      s->cv.notify_all();
    }

    payload_hdr =
        (struct st40_rfc8331_payload_hdr*)((uint8_t*)payload_hdr +
                                           st40_rfc8331_payload_bytes(udw_size));
  }
}

/* §5.2: V=2, M=0, the UDP size limit and the payload type */
static bool rx_fmd_rtp_hdr_ok(const struct st_rfc3550_rtp_hdr* rtp, uint16_t len) {
  return len <= ST40_TEST_FMD_MAX_RTP_BYTES && rtp->version == 2 && !rtp->marker &&
         rtp->payload_type == ST40_TEST_FMD_PAYLOAD_TYPE;
}

/* seq gaps, MTL 90 kHz pacing (frame_time set) and the max gap; false after a gap */
static bool rx_fmd_track(tests_context* s, const struct st_rfc3550_rtp_hdr* rtp) {
  uint16_t seq = ntohs(rtp->seq_number);
  uint32_t tmstamp = ntohl(rtp->tmstamp);
  uint64_t now = st_test_get_monotonic_time();
  bool in_seq = true;

  if (s->last_rx_time) {
    uint16_t prev_seq =
        static_cast<uint16_t>(__atomic_load_n(&s->seq_id, __ATOMIC_RELAXED));
    int16_t lost = (int16_t)(seq - (uint16_t)(prev_seq + 1));
    int32_t step = (int32_t)(tmstamp - s->pre_timestamp);
    double period = s->frame_time * 90000 / NS_PER_S;
    s->max_rx_gap = std::max(s->max_rx_gap, now - s->last_rx_time);
    s->last_rx_time = now;
    /* a reordered packet is no loss, and must not move the stream state back */
    if (lost < 0) return false;
    if (lost) {
      s->rx_seq_lost += lost;
      in_seq = false;
    }
    /* a late frame skips epochs; MTL rounds each frame to the nearest tick */
    if (period != 0 && step != 0) {
      double periods = round(step / period);
      double tolerance = fabs(period - round(period)) < 1e-6 ? 1e-6 : 1;
      if (periods < 1 || fabs(step - periods * period) > tolerance) {
        err("%s(%d), ts %u after %u is not whole frame periods\n", __func__, s->idx,
            tmstamp, s->pre_timestamp);
        __atomic_fetch_add(&s->rx_meta_fail_cnt, 1, __ATOMIC_RELAXED);
      }
    }
  }
  s->last_rx_time = now;
  __atomic_store_n(&s->seq_id, seq, __ATOMIC_RELAXED);
  s->pre_timestamp = tmstamp;
  return in_seq;
}

/* checks the header and zero padding, sha_frame_check() then checks the data item */
static void rx_handle_fmd(tests_context* s, struct st40_fmd_rtp_hdr* rtp, uint16_t len) {
  struct st40_fmd_rtp_hdr hdr;
  uint8_t* item = (uint8_t*)&rtp[1];
  size_t padded_size = ST40_TEST_FMD_PADDED(s->frame_size);
  /* an empty frame is a zero-item packet, the bare RTP header */
  size_t expect_len = s->frame_size ? sizeof(hdr) + padded_size : sizeof(hdr.base);

  rx_fmd_track(s, &rtp->base);
  bool valid = rx_fmd_rtp_hdr_ok(&rtp->base, len) && len == expect_len;
  if (valid && s->frame_size) {
    memcpy(&hdr, rtp, sizeof(hdr));
    hdr.swapped_fmd_hdr_chunk = ntohl(hdr.swapped_fmd_hdr_chunk);
    valid = hdr.fmd_hdr_chunk.data_item_type == ST40_TEST_FMD_DIT &&
            hdr.fmd_hdr_chunk.data_item_k_bit == ST40_TEST_FMD_K_BIT &&
            (size_t)hdr.fmd_hdr_chunk.data_item_length * 4 == padded_size;
  }
  for (size_t i = s->frame_size; valid && i < padded_size; i++) valid = !item[i];
  if (!valid) {
    err("%s(%d), invalid fast metadata packet\n", __func__, s->idx);
    __atomic_fetch_add(&s->rx_meta_fail_cnt, 1, __ATOMIC_RELAXED);
    return;
  }
  if (!s->check_sha) return;

  uint8_t* data = (uint8_t*)st_test_zmalloc(s->frame_size);
  if (!data) {
    __atomic_fetch_add(&s->rx_meta_fail_cnt, 1, __ATOMIC_RELAXED);
    return;
  }
  memcpy(data, item, s->frame_size);
  std::unique_lock<std::mutex> lck(s->mtx);
  s->buf_q.push(data);
  s->cv.notify_all();
}

/* walks every item: segments reassemble by offset, K=1 exactly on the last one */
static void rx_handle_fmd_annex_a(tests_context* s, uint8_t* rtp, uint16_t len) {
  auto a = s->st40_annex_a;
  auto base = (struct st_rfc3550_rtp_hdr*)rtp;
  bool valid = rx_fmd_rtp_hdr_ok(base, len);
  size_t off = sizeof(*base);
  int items = 0;

  /* a lost packet breaks the objects in flight, resync on their next offset 0 */
  if (!rx_fmd_track(s, base)) a->rx_next[0] = a->rx_next[1] = -1;
  for (; valid && off + 4 <= len; items++) {
    uint32_t word = fmd_be32(rtp + off);
    uint32_t dit = word >> 10, length = word & 0x1ff;
    bool k = word & (1 << 9);
    int o = dit == a->dit[0] ? 0 : dit == a->dit[1] ? 1 : -1;
    /* Data Item Length 0 is illegal, and a segment needs its offset and a word */
    if (o < 0 || length < 2 || off + 4 + (size_t)length * 4 > len) {
      valid = false;
      break;
    }
    uint32_t seg_off = fmd_be32(rtp + off + 4), words = length - 1;
    uint32_t total = a->obj[o].size() / 4;
    if (!seg_off) a->rx_next[o] = 0;
    if (a->rx_next[o] >= 0) {
      if (seg_off != a->rx_next[o] || seg_off + words > total ||
          k != (seg_off + words == total)) {
        valid = false;
        break;
      }
      memcpy(&a->rx_obj[o][(size_t)seg_off * 4], rtp + off + 8, (size_t)words * 4);
      a->rx_next[o] += words;
      if (k) {
        unsigned char result[SHA256_DIGEST_LENGTH];
        SHA256(a->rx_obj[o].data(), a->obj_size[o], result);
        if (memcmp(result, a->sha[o], sizeof(result)))
          __atomic_fetch_add(&s->sha_fail_cnt, 1, __ATOMIC_RELAXED);
        for (size_t i = a->obj_size[o]; i < a->rx_obj[o].size(); i++)
          if (a->rx_obj[o][i]) __atomic_fetch_add(&s->sha_fail_cnt, 1, __ATOMIC_RELAXED);
        a->rx_done[o].fetch_add(1, std::memory_order_relaxed);
        a->rx_next[o] = -1;
      }
    }
    off += 4 + length * 4;
  }
  if (!valid || off != len) {
    err("%s(%d), invalid Annex A packet\n", __func__, s->idx);
    __atomic_fetch_add(&s->rx_meta_fail_cnt, 1, __ATOMIC_RELAXED);
    return;
  }
  if (!items) a->rx_zero_item_pkts.fetch_add(1, std::memory_order_relaxed);
  if (items > 1) a->rx_multi_item_pkts.fetch_add(1, std::memory_order_relaxed);
}

static int rx_rtp_ready(void* priv) {
  auto ctx = (tests_context*)priv;
  void* useptr;
  void* mbuf;
  uint16_t len;

  auto handle = (st40_rx_handle)__atomic_load_n(&ctx->handle, __ATOMIC_ACQUIRE);
  if (!handle) return -EIO;

  while (1) {
    mbuf = st40_rx_get_mbuf(handle, &useptr, &len);
    if (!mbuf) break; /* no mbuf */
    if (ctx->st40_annex_a)
      rx_handle_fmd_annex_a(ctx, (uint8_t*)useptr, len);
    else if (ctx->st40_fmd)
      rx_handle_fmd(ctx, (struct st40_fmd_rtp_hdr*)useptr, len);
    else if (ctx->check_sha)
      rx_handle_rtp(ctx, (struct st40_rfc8331_rtp_hdr*)useptr);
    st40_rx_put_mbuf(handle, mbuf);
    __atomic_fetch_add(&ctx->fb_rec, 1, __ATOMIC_RELAXED);
  }

  if (!ctx->start_time) ctx->start_time = st_test_get_monotonic_time();

  return 0;
}

static void st40_rx_ops_init(tests_context* st40, struct st40_rx_ops* ops) {
  auto ctx = st40->ctx;

  memset(ops, 0, sizeof(*ops));
  ops->name = "st40_test";
  ops->priv = st40;
  ops->num_port = ctx->same_dual_port ? 1
                                      : static_cast<uint8_t>(std::min<int>(
                                            ctx->para.num_ports, MTL_SESSION_PORT_MAX));
  memcpy(ops->ip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
         MTL_IP_ADDR_LEN);
  snprintf(ops->port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
           ctx->para.port[MTL_PORT_P]);
  ops->udp_port[MTL_SESSION_PORT_P] = 30000 + st40->idx * 2;
  if (ops->num_port == 2) {
    memcpy(ops->ip_addr[MTL_SESSION_PORT_R], ctx->mcast_ip_addr[MTL_PORT_R],
           MTL_IP_ADDR_LEN);
    snprintf(ops->port[MTL_SESSION_PORT_R], MTL_PORT_MAX_LEN, "%s",
             ctx->para.port[MTL_PORT_R]);
    ops->udp_port[MTL_SESSION_PORT_R] = 30000 + st40->idx * 2;
  }
  ops->type = ST40_TYPE_RTP_LEVEL;
  ops->notify_rtp_ready = rx_rtp_ready;
  ops->rtp_ring_size = 1024;
  ops->payload_type = ST40_TEST_PAYLOAD_TYPE;
}

static void st40_tx_ops_init(tests_context* st40, struct st40_tx_ops* ops) {
  auto ctx = st40->ctx;

  memset(ops, 0, sizeof(*ops));
  ops->name = "st40_test";
  ops->priv = st40;
  ops->num_port = ctx->same_dual_port ? 1
                                      : static_cast<uint8_t>(std::min<int>(
                                            ctx->para.num_ports, MTL_SESSION_PORT_MAX));
  memcpy(ops->dip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
         MTL_IP_ADDR_LEN);
  snprintf(ops->port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
           ctx->para.port[MTL_PORT_P]);
  ops->udp_port[MTL_SESSION_PORT_P] = 30000 + st40->idx * 2;
  if (ops->num_port == 2) {
    memcpy(ops->dip_addr[MTL_SESSION_PORT_R], ctx->mcast_ip_addr[MTL_PORT_R],
           MTL_IP_ADDR_LEN);
    snprintf(ops->port[MTL_SESSION_PORT_R], MTL_PORT_MAX_LEN, "%s",
             ctx->para.port[MTL_PORT_R]);
    ops->udp_port[MTL_SESSION_PORT_R] = 30000 + st40->idx * 2;
  }
  ops->type = ST40_TYPE_FRAME_LEVEL;
  ops->fps = ST_FPS_P59_94;
  ops->payload_type = ST40_TEST_PAYLOAD_TYPE;
  ops->framebuff_cnt = st40->fb_cnt;
  ops->get_next_frame = tx_anc_next_frame;
  ops->rtp_ring_size = 1024;
  ops->notify_rtp_done = tx_rtp_done;
}

/* Single-port loopback TX ops: sends on MTL_PORT_P toward MTL_PORT_R (or mcast). */
static void st40_tx_loopback_ops_init(tests_context* st40, struct st40_tx_ops* ops,
                                      enum st40_type type, enum st_fps fps,
                                      uint16_t udp_port) {
  auto ctx = st40->ctx;

  st40_tx_ops_init(st40, ops);
  ops->num_port = 1;
  memcpy(
      ops->dip_addr[MTL_SESSION_PORT_P],
      ctx->mcast_only ? ctx->mcast_ip_addr[MTL_PORT_P] : ctx->para.sip_addr[MTL_PORT_R],
      MTL_IP_ADDR_LEN);
  snprintf(ops->port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
           ctx->para.port[MTL_PORT_P]);
  ops->udp_port[MTL_SESSION_PORT_P] = udp_port;
  ops->type = type;
  ops->fps = fps;
}

/* Single-port loopback RX ops: receives on MTL_PORT_R from MTL_PORT_P (or mcast). */
static void st40_rx_loopback_ops_init(tests_context* st40, struct st40_rx_ops* ops,
                                      uint16_t udp_port) {
  auto ctx = st40->ctx;

  st40_rx_ops_init(st40, ops);
  ops->num_port = 1;
  memcpy(
      ops->ip_addr[MTL_SESSION_PORT_P],
      ctx->mcast_only ? ctx->mcast_ip_addr[MTL_PORT_P] : ctx->para.sip_addr[MTL_PORT_P],
      MTL_IP_ADDR_LEN);
  snprintf(ops->port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
           ctx->para.port[MTL_PORT_R]);
  ops->udp_port[MTL_SESSION_PORT_P] = udp_port;
}

static void st40_tx_assert_cnt(int expect_s40_tx_cnt) {
  auto ctx = st_test_ctx();
  struct st_var_info var;

  EXPECT_GE(st_get_var_info(ctx->handle, &var), 0);
  EXPECT_EQ(var.st40_tx_sessions_cnt, expect_s40_tx_cnt);
}

static void st40_rx_assert_cnt(int expect_s40_rx_cnt) {
  auto ctx = st_test_ctx();
  struct st_var_info var;

  EXPECT_GE(st_get_var_info(ctx->handle, &var), 0);
  EXPECT_EQ(var.st40_rx_sessions_cnt, expect_s40_rx_cnt);
}

static void st40_fmd_tx_ops_init(tests_context* st40, struct st40_tx_ops* ops) {
  st40_tx_ops_init(st40, ops);
  ops->flags |= ST40_TX_FLAG_FAST_METADATA;
  ops->fmd_dit = ST40_TEST_FMD_DIT;
  ops->fmd_k_bit = ST40_TEST_FMD_K_BIT;
}

static void st40_fmd_rx_ops_init(tests_context* st40, struct st40_rx_ops* ops) {
  st40_rx_ops_init(st40, ops);
  ops->flags |= ST40_RX_FLAG_FAST_METADATA;
  ops->payload_type = ST40_TEST_FMD_PAYLOAD_TYPE;
}

static void st40_fmd_expect_stats(tests_context* rx, st40_tx_handle tx_handle,
                                  st40_rx_handle rx_handle, uint64_t stop_time) {
  struct st40_tx_user_stats tx_stats;
  struct st40_rx_user_stats rx_stats;

  if (rx->last_rx_time && stop_time > rx->last_rx_time)
    rx->max_rx_gap = std::max(rx->max_rx_gap, stop_time - rx->last_rx_time);
  EXPECT_LT(rx->max_rx_gap, ST40_TEST_FMD_MAX_GAP_NS);
  ASSERT_GE(st40_tx_get_session_stats(tx_handle, &tx_stats), 0);
  ASSERT_GE(st40_rx_get_session_stats(rx_handle, &rx_stats), 0);
  EXPECT_GT(tx_stats.common.port[MTL_SESSION_PORT_P].packets, 0u);
  EXPECT_GT(rx_stats.common.port[MTL_SESSION_PORT_P].packets, 0u);
  EXPECT_EQ(rx_stats.common.stat_pkts_wrong_pt_dropped, 0u);
  EXPECT_EQ(rx_stats.stat_pkts_wrong_interlace_dropped, 0u);
  EXPECT_LE(rx->rx_seq_lost, 2u);
  /* the lib counts wire gaps, the app also sees a ring overflow as one */
  EXPECT_EQ(rx->rx_seq_lost,
            rx_stats.common.stat_lost_packets + rx_stats.stat_pkts_enqueue_fail);
}

TEST(St40_tx, create_free_single) {
  mtl_test::verify_create_free<struct st40_tx_ops, st40_tx_handle, st40_tx_create,
                               st40_tx_free>(st40_tx_ops_init, st40_tx_assert_cnt, 0, 1,
                                             1);
}
TEST(St40_tx, create_free_multi) {
  mtl_test::verify_create_free<struct st40_tx_ops, st40_tx_handle, st40_tx_create,
                               st40_tx_free>(st40_tx_ops_init, st40_tx_assert_cnt, 0, 1,
                                             6);
}
TEST(St40_tx, create_free_mix) {
  mtl_test::verify_create_free<struct st40_tx_ops, st40_tx_handle, st40_tx_create,
                               st40_tx_free>(st40_tx_ops_init, st40_tx_assert_cnt, 2, 3,
                                             4);
}
TEST(St40_tx, create_free_max) {
  mtl_test::verify_create_free_max<struct st40_tx_ops, st40_tx_handle, st40_tx_create,
                                   st40_tx_free>(st40_tx_ops_init, st40_tx_assert_cnt,
                                                 TEST_CREATE_FREE_MAX);
}
TEST(St40_tx, create_expect_fail) {
  mtl_test::verify_invalid_ports<struct st40_tx_ops, st40_tx_handle, st40_tx_create,
                                 st40_tx_free>(st40_tx_ops_init);
}
TEST(St40_tx, create_expect_fail_ring_sz) {
  mtl_test::verify_rtp_ring_invalid<struct st40_tx_ops, st40_tx_handle, st40_tx_create,
                                    st40_tx_free>(st40_tx_ops_init, ST40_TYPE_RTP_LEVEL,
                                                  0);
  mtl_test::verify_rtp_ring_invalid<struct st40_tx_ops, st40_tx_handle, st40_tx_create,
                                    st40_tx_free>(st40_tx_ops_init, ST40_TYPE_RTP_LEVEL,
                                                  128 + 1);
}
TEST(St40_tx, get_framebuffer) {
  mtl_test::verify_get_framebuffer<struct st40_tx_ops, st40_tx_handle, st40_tx_create,
                                   st40_tx_free>(st40_tx_ops_init,
                                                 st40_tx_get_framebuffer, 3);
  mtl_test::verify_get_framebuffer<struct st40_tx_ops, st40_tx_handle, st40_tx_create,
                                   st40_tx_free>(st40_tx_ops_init,
                                                 st40_tx_get_framebuffer, 1000);
}
TEST(St40_tx, get_framebuffer_expect_fail) {
  mtl_test::verify_get_framebuffer_oob<struct st40_tx_ops, st40_tx_handle, st40_tx_create,
                                       st40_tx_free>(st40_tx_ops_init,
                                                     st40_tx_get_framebuffer, 3);
  mtl_test::verify_get_framebuffer_oob<struct st40_tx_ops, st40_tx_handle, st40_tx_create,
                                       st40_tx_free>(st40_tx_ops_init,
                                                     st40_tx_get_framebuffer, 1000);
}

TEST(St40_rx, create_free_single) {
  mtl_test::verify_create_free<struct st40_rx_ops, st40_rx_handle, st40_rx_create,
                               st40_rx_free>(st40_rx_ops_init, st40_rx_assert_cnt, 0, 1,
                                             1);
}
TEST(St40_rx, create_free_multi) {
  mtl_test::verify_create_free<struct st40_rx_ops, st40_rx_handle, st40_rx_create,
                               st40_rx_free>(st40_rx_ops_init, st40_rx_assert_cnt, 0, 1,
                                             6);
}
TEST(St40_rx, create_free_mix) {
  mtl_test::verify_create_free<struct st40_rx_ops, st40_rx_handle, st40_rx_create,
                               st40_rx_free>(st40_rx_ops_init, st40_rx_assert_cnt, 2, 3,
                                             4);
}
TEST(St40_rx, create_free_max) {
  mtl_test::verify_create_free_max<struct st40_rx_ops, st40_rx_handle, st40_rx_create,
                                   st40_rx_free>(st40_rx_ops_init, st40_rx_assert_cnt,
                                                 TEST_CREATE_FREE_MAX);
}
TEST(St40_rx, create_expect_fail) {
  mtl_test::verify_invalid_ports<struct st40_rx_ops, st40_rx_handle, st40_rx_create,
                                 st40_rx_free>(st40_rx_ops_init);
}
TEST(St40_rx, create_expect_fail_ring_sz) {
  mtl_test::verify_rtp_ring_invalid<struct st40_rx_ops, st40_rx_handle, st40_rx_create,
                                    st40_rx_free>(st40_rx_ops_init, 0);
  mtl_test::verify_rtp_ring_invalid<struct st40_rx_ops, st40_rx_handle, st40_rx_create,
                                    st40_rx_free>(st40_rx_ops_init, 128 + 1);
}

TEST(St40_tx, fmd_create_free_single) {
  mtl_test::verify_create_free<struct st40_tx_ops, st40_tx_handle, st40_tx_create,
                               st40_tx_free>(st40_fmd_tx_ops_init, st40_tx_assert_cnt, 0,
                                             1, 1);
}
TEST(St40_tx, fmd_create_free_multi) {
  mtl_test::verify_create_free<struct st40_tx_ops, st40_tx_handle, st40_tx_create,
                               st40_tx_free>(st40_fmd_tx_ops_init, st40_tx_assert_cnt, 0,
                                             4, 1);
}

TEST(St40_rx, fmd_create_free_single) {
  mtl_test::verify_create_free<struct st40_rx_ops, st40_rx_handle, st40_rx_create,
                               st40_rx_free>(st40_fmd_rx_ops_init, st40_rx_assert_cnt, 0,
                                             1, 1);
}
TEST(St40_rx, fmd_create_free_multi) {
  mtl_test::verify_create_free<struct st40_rx_ops, st40_rx_handle, st40_rx_create,
                               st40_rx_free>(st40_fmd_rx_ops_init, st40_rx_assert_cnt, 0,
                                             4, 1);
}

static void st40_tx_frame_init(tests_context* st40, st40_tx_handle handle,
                               enum st40_type type, size_t frame_size = 240) {
  if (st40->st40_empty_frame) frame_size = 0;

  st40->pkt_data_len = frame_size;
  st40->frame_size = frame_size;

  for (int frame = 0; frame < st40->fb_cnt; frame++) {
    if (frame_size > 0) {
      st40->frame_buf[frame] = (uint8_t*)st_test_zmalloc(frame_size);
      ASSERT_TRUE(st40->frame_buf[frame] != NULL);
    } else {
      st40->frame_buf[frame] = nullptr;
    }

    if (ST40_TYPE_FRAME_LEVEL == type) {
      struct st40_frame* dst = (struct st40_frame*)st40_tx_get_framebuffer(handle, frame);
      ASSERT_TRUE(dst != NULL);

      dst->data_size = dst->meta[0].udw_size = frame_size;
      dst->meta[0].udw_offset = 0;
      dst->meta[0].c = 0;
      dst->meta[0].line_number = 10;
      dst->meta[0].hori_offset = 0;
      dst->meta[0].s = 0;
      dst->meta[0].stream_num = 0;
      dst->meta[0].did = 0x43;
      dst->meta[0].sdid = 0x02;
      dst->meta_num = (st40->st40_empty_frame || st40->st40_fmd) ? 0 : 1;
      dst->data = st40->frame_buf[frame];
    }
  }
}

static void st40_tx_fps_test(const std::vector<St40SessionSpec>& specs) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto m_handle = ctx->handle;
  const int sessions = static_cast<int>(specs.size());

  std::vector<ScopedSt40Ctx> tx_ctxs;
  std::vector<tests_context*> raw_tx;
  std::vector<ScopedSt40Tx> tx_handles;
  std::vector<double> expect_framerate(sessions);
  tx_ctxs.reserve(sessions);
  raw_tx.reserve(sessions);
  tx_handles.reserve(sessions);

  mtl_test::ScopedMtlStart mtl_run(m_handle);
  std::vector<mtl_test::ScopedWorker> tx_workers;

  for (int i = 0; i < sessions; i++) {
    expect_framerate[i] = st_frame_rate(specs[i].fps);
    tx_ctxs.push_back(make_st40_ctx(ctx, i, 3));
    raw_tx.push_back(tx_ctxs.back().get());

    struct st40_tx_ops ops;
    st40_tx_ops_init(raw_tx[i], &ops);
    ops.type = specs[i].type;
    ops.fps = specs[i].fps;

    tx_handles.emplace_back(st40_tx_create(m_handle, &ops));
    ASSERT_TRUE(tx_handles[i]);

    st40_tx_frame_init(raw_tx[i], tx_handles[i].get(), specs[i].type);
    __atomic_store_n(&raw_tx[i]->handle, tx_handles[i].get(), __ATOMIC_RELEASE);

    if (specs[i].type == ST40_TYPE_RTP_LEVEL) {
      raw_tx[i]->stop = false;
      tx_workers.emplace_back(raw_tx[i], std::thread(tx_feed_packet, raw_tx[i]));
    }
  }

  EXPECT_GE(mtl_run.start(), 0);
  /* 1.0 s window with >= 25 frames after 3-frame warmup: <= 1/25 (4%) edge
   * error at 29.97 fps and <= 1/59 (1.7%) at 59.94 fps vs +/-10% tolerance. */
  auto win = mtl_test::measure_rate_window(raw_tx, mtl_test::tx_frames,
                                           /*warmup_frames=*/3,
                                           /*min_delta_frames=*/25,
                                           /*min_window_ms=*/1000);
  EXPECT_TRUE(win.warmed_up);
  EXPECT_TRUE(win.complete);

  for (auto& w : tx_workers) w.stop();
  EXPECT_GE(mtl_run.stop(), 0);

  for (int i = 0; i < sessions; i++) {
    int sent = mtl_test::tx_frames(raw_tx[i]);
    EXPECT_GT(sent, 0);
    info("%s, session %d fb_send %d framerate %f\n", __func__, i, sent, win.rate[i]);
    EXPECT_NEAR(win.rate[i], expect_framerate[i], expect_framerate[i] * 0.1);
  }
}

static void st40_rx_fps_test(const St40RxScenario& s) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto m_handle = ctx->handle;
  const int sessions = static_cast<int>(s.sessions.size());

  MTL_TEST_REQUIRE_PORTS(ctx, 2);

  /* Declare contexts -> handles -> ScopedMtlStart -> workers so RAII tears down
   * in strict reverse order even on early ASSERT failure. */
  std::vector<ScopedSt40Ctx> tx_ctxs, rx_ctxs;
  std::vector<tests_context*> raw_tx, raw_rx;
  std::vector<ScopedSt40Tx> tx_handles;
  std::vector<ScopedSt40Rx> rx_handles;
  std::vector<double> expect_framerate(sessions);
  tx_ctxs.reserve(sessions);
  rx_ctxs.reserve(sessions);
  raw_tx.reserve(sessions);
  raw_rx.reserve(sessions);
  tx_handles.reserve(sessions);
  rx_handles.reserve(sessions);

  mtl_test::ScopedMtlStart mtl_run(m_handle);
  std::vector<mtl_test::ScopedWorker> tx_workers, sha_workers;

  for (int i = 0; i < sessions; i++) {
    const auto& spec = s.sessions[i];
    expect_framerate[i] = st_frame_rate(spec.fps);
    if (s.user_timestamp) expect_framerate[i] /= 2;

    tx_ctxs.push_back(make_st40_ctx(ctx, i, TEST_SHA_HIST_NUM, s.check_sha));
    raw_tx.push_back(tx_ctxs.back().get());
    raw_tx[i]->st40_empty_frame = s.empty_frame;
    raw_tx[i]->st40_fmd = spec.fmd;

    struct st40_tx_ops ops_tx;
    st40_tx_loopback_ops_init(raw_tx[i], &ops_tx, spec.type, spec.fps, 30000 + i * 2);
    ops_tx.interlaced = s.interlaced;
    ops_tx.ssrc = i ? i + 0x88888888 : 0;
    if (s.user_timestamp) {
      ops_tx.get_next_frame = tx_anc_next_frame_timestamp;
      ops_tx.flags |= ST40_TX_FLAG_USER_PACING;
    }
    if (s.dedicate_tx_queue) ops_tx.flags |= ST40_TX_FLAG_DEDICATE_QUEUE;
    if (spec.fmd) {
      ops_tx.flags |= ST40_TX_FLAG_FAST_METADATA;
      ops_tx.fmd_dit = ST40_TEST_FMD_DIT;
      ops_tx.fmd_k_bit = ST40_TEST_FMD_K_BIT;
      ops_tx.payload_type = 0; /* frame level: RX filters on the lib default */
    }

    tx_handles.emplace_back(st40_tx_create(m_handle, &ops_tx));
    ASSERT_TRUE(tx_handles[i]);

    st40_tx_frame_init(raw_tx[i], tx_handles[i].get(), spec.type,
                       spec.fmd ? spec.fmd_data_size : 240);
    if (s.check_sha && raw_tx[i]->frame_size > 0) {
      for (int frame = 0; frame < raw_tx[i]->fb_cnt; frame++) {
        uint8_t* fb = raw_tx[i]->frame_buf[frame];
        st_test_rand_data(fb, raw_tx[i]->frame_size, frame);
        /* RFC 8331 RX reads F from item[1] >> 6 and drops 0b01 */
        if (spec.fmd && raw_tx[i]->frame_size > 1) fb[1] = 0x40 | (fb[1] & 0x3f);
        unsigned char* result = raw_tx[i]->shas[frame];
        SHA256((unsigned char*)fb, raw_tx[i]->frame_size, result);
        test_sha_dump("st40_rx", result);
      }
    }

    __atomic_store_n(&raw_tx[i]->handle, tx_handles[i].get(), __ATOMIC_RELEASE);
    if (spec.type == ST40_TYPE_RTP_LEVEL) {
      raw_tx[i]->stop = false;
      tx_workers.emplace_back(raw_tx[i], std::thread(tx_feed_packet, raw_tx[i]));
    }
  }

  for (int i = 0; i < sessions; i++) {
    const auto& spec = s.sessions[i];
    rx_ctxs.push_back(make_st40_ctx(ctx, i, 3, s.check_sha));
    raw_rx.push_back(rx_ctxs.back().get());
    raw_rx[i]->st40_fmd = spec.fmd;

    struct st40_rx_ops ops_rx;
    st40_rx_loopback_ops_init(raw_rx[i], &ops_rx, 30000 + i * 2);
    ops_rx.interlaced = s.interlaced;
    ops_rx.ssrc = i ? i + 0x88888888 : 0;
    if (spec.fmd) {
      ops_rx.flags |= ST40_RX_FLAG_FAST_METADATA;
      ops_rx.payload_type = ST40_TEST_FMD_PAYLOAD_TYPE;
      raw_rx[i]->frame_size = raw_tx[i]->frame_size;
      raw_rx[i]->frame_time = NS_PER_S / st_frame_rate(spec.fps);
    }

    rx_handles.emplace_back(st40_rx_create(m_handle, &ops_rx));
    ASSERT_TRUE(rx_handles[i]);

    if (s.check_sha) {
      raw_rx[i]->pkt_data_len = raw_tx[i]->pkt_data_len;
      raw_rx[i]->frame_size = raw_rx[i]->pkt_data_len;
      memcpy(raw_rx[i]->shas, raw_tx[i]->shas, TEST_SHA_HIST_NUM * SHA256_DIGEST_LENGTH);
      sha_workers.emplace_back(raw_rx[i], std::thread(sha_frame_check, raw_rx[i]));
    }

    __atomic_store_n(&raw_rx[i]->handle, rx_handles[i].get(), __ATOMIC_RELEASE);

    struct st_queue_meta meta;
    memset(&meta, 0, sizeof(meta));
    EXPECT_GE(st40_rx_get_queue_meta(rx_handles[i].get(), &meta), 0);
    EXPECT_EQ(meta.num_port, 1);
  }

  EXPECT_GE(mtl_run.start(), 0);
  /* 1.0 s window with >= 25 frames after 3-frame warmup: <= 1/25 (4%) edge
   * error at 23.98-29.97 fps and <= 1/59 (1.7%) at 59.94 fps vs +/-10%
   * tolerance (~8-10x faster than legacy sleep(10)). */
  auto win = mtl_test::measure_rate_window(raw_rx, mtl_test::rx_frames,
                                           /*warmup_frames=*/3,
                                           /*min_delta_frames=*/25,
                                           /*min_window_ms=*/1000);
  EXPECT_TRUE(win.warmed_up);
  EXPECT_TRUE(win.complete);

  if (s.check_sha) {
    EXPECT_TRUE(mtl_test::wait_for_all(raw_rx, mtl_test::sha_checked_frames, 1, 2000));
  }

  const uint64_t stop_time = st_test_get_monotonic_time();
  for (auto& w : tx_workers) w.stop();
  for (auto& w : sha_workers) w.stop();
  EXPECT_GE(mtl_run.stop(), 0);

  for (int i = 0; i < sessions; i++) {
    int rec = mtl_test::rx_frames(raw_rx[i]);
    EXPECT_GT(rec, 0);
    info("%s, session %d fb_rec %d framerate %f\n", __func__, i, rec, win.rate[i]);
    EXPECT_NEAR(win.rate[i], expect_framerate[i], expect_framerate[i] * 0.1);
    int fail_tolerance = raw_rx[i]->st40_fmd ? 0 : 2;
    EXPECT_LE(mtl_test::relaxed_load(&raw_rx[i]->sha_fail_cnt), fail_tolerance);
    EXPECT_LE(mtl_test::relaxed_load(&raw_rx[i]->rx_meta_fail_cnt), fail_tolerance);
    if (raw_rx[i]->st40_fmd) {
      st40_fmd_expect_stats(raw_rx[i], tx_handles[i].get(), rx_handles[i].get(),
                            stop_time);
    }
    if (s.check_sha) {
      EXPECT_GT(mtl_test::sha_checked_frames(raw_rx[i]), 0);
    }
  }
}

TEST(St40_tx, frame_fps59_94_s1) {
  st40_tx_fps_test({{ST40_TYPE_FRAME_LEVEL, ST_FPS_P59_94}});
}
TEST(St40_tx, mix_fps50_fps29_97) {
  st40_tx_fps_test({
      {ST40_TYPE_FRAME_LEVEL, ST_FPS_P50},
      {ST40_TYPE_RTP_LEVEL, ST_FPS_P29_97},
  });
}

TEST(St40_rx, mix_s2) {
  St40RxScenario s;
  s.sessions = {
      {ST40_TYPE_RTP_LEVEL, ST_FPS_P50},
      {ST40_TYPE_FRAME_LEVEL, ST_FPS_P59_94},
  };
  s.check_sha = true;
  s.dedicate_tx_queue = true;
  st40_rx_fps_test(s);
}
TEST(St40_rx, frame_user_timestamp) {
  St40RxScenario s;
  s.sessions = {{ST40_TYPE_FRAME_LEVEL, ST_FPS_P59_94}};
  s.check_sha = true;
  s.user_timestamp = true;
  st40_rx_fps_test(s);
}
TEST(St40_rx, frame_interlaced_empty) {
  St40RxScenario s;
  s.sessions = {{ST40_TYPE_FRAME_LEVEL, ST_FPS_P50}};
  s.empty_frame = true;
  s.interlaced = true;
  st40_rx_fps_test(s);
}
TEST(St40_rx, fmd_mix_digest) {
  /* RFC 8331 ANC next to FMD at both levels on one transmitter, 241 needs zero padding */
  St40RxScenario s;
  s.sessions = {
      {ST40_TYPE_FRAME_LEVEL, ST_FPS_P59_94, false, 0},
      {ST40_TYPE_FRAME_LEVEL, ST_FPS_P59_94, true, 241},
      {ST40_TYPE_RTP_LEVEL, ST_FPS_P50, true, 1336},
  };
  s.check_sha = true;
  st40_rx_fps_test(s);
}
TEST(St40_rx, fmd_frame_interlaced_fps50_digest) {
  /* the largest data item one packet carries */
  St40RxScenario s;
  s.sessions = {{ST40_TYPE_FRAME_LEVEL, ST_FPS_P50, true, 1436}};
  s.check_sha = true;
  s.interlaced = true;
  st40_rx_fps_test(s);
}
TEST(St40_rx, fmd_rtp_fps29_97_fps59_94_digest) {
  /* the largest item is MTL_PKT_MAX_RTP_BYTES minus the 16 bytes rtp header */
  St40RxScenario s;
  s.sessions = {
      {ST40_TYPE_RTP_LEVEL, ST_FPS_P29_97, true, 5},
      {ST40_TYPE_RTP_LEVEL, ST_FPS_P59_94, true, 1336},
  };
  s.check_sha = true;
  st40_rx_fps_test(s);
}

static int tx_fmd_frame_done(void* priv, uint16_t frame_idx,
                             struct st40_tx_frame_meta* /*meta*/) {
  auto ctx = (tests_context*)priv;
  ((std::atomic<int>*)ctx->priv)[frame_idx].fetch_add(1, std::memory_order_relaxed);
  return 0;
}

static void st40_fmd_oversize_test() {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto m_handle = ctx->handle;
  /* frame 0 is one byte over the largest item one packet carries, frame 1 is that */
  const size_t frame_size[2] = {1437, 1436};

  MTL_TEST_REQUIRE_PORTS(ctx, 2);

  auto tx = make_st40_ctx(ctx, 0, 2);
  auto rx = make_st40_ctx(ctx, 0, 3, /*check_sha=*/true);
  ASSERT_TRUE(tx != nullptr);
  ASSERT_TRUE(rx != nullptr);

  auto frame_done = (std::atomic<int>*)st_test_zmalloc(2 * sizeof(std::atomic<int>));
  ASSERT_TRUE(frame_done != nullptr);
  new (&frame_done[0]) std::atomic<int>(0);
  new (&frame_done[1]) std::atomic<int>(0);
  tx->priv = frame_done; /* freed by tests_context_unit() via ScopedSt40Ctx */

  struct st40_tx_ops ops_tx;
  st40_fmd_tx_ops_init(tx.get(), &ops_tx);
  ops_tx.num_port = 1;
  memcpy(
      ops_tx.dip_addr[MTL_SESSION_PORT_P],
      ctx->mcast_only ? ctx->mcast_ip_addr[MTL_PORT_P] : ctx->para.sip_addr[MTL_PORT_R],
      MTL_IP_ADDR_LEN);
  ops_tx.payload_type = 0;
  ops_tx.notify_frame_done = tx_fmd_frame_done;

  ScopedSt40Tx tx_handle(st40_tx_create(m_handle, &ops_tx));
  ASSERT_TRUE(tx_handle);
  for (int i = 0; i < tx->fb_cnt; i++) {
    tx->frame_buf[i] = (uint8_t*)st_test_zmalloc(frame_size[i]);
    ASSERT_TRUE(tx->frame_buf[i] != nullptr);
    st_test_rand_data(tx->frame_buf[i], frame_size[i], i);
    struct st40_frame* dst =
        (struct st40_frame*)st40_tx_get_framebuffer(tx_handle.get(), i);
    ASSERT_TRUE(dst != nullptr);
    dst->data = tx->frame_buf[i];
    dst->data_size = frame_size[i];
    dst->meta_num = 0;
  }
  __atomic_store_n(&tx->handle, tx_handle.get(), __ATOMIC_RELEASE);

  rx->st40_fmd = true;
  rx->frame_size = frame_size[1];
  rx->frame_time = NS_PER_S / st_frame_rate(ops_tx.fps);
  SHA256(tx->frame_buf[1], frame_size[1], rx->shas[0]);

  struct st40_rx_ops ops_rx;
  st40_fmd_rx_ops_init(rx.get(), &ops_rx);
  ops_rx.num_port = 1;
  memcpy(
      ops_rx.ip_addr[MTL_SESSION_PORT_P],
      ctx->mcast_only ? ctx->mcast_ip_addr[MTL_PORT_P] : ctx->para.sip_addr[MTL_PORT_P],
      MTL_IP_ADDR_LEN);
  (void)snprintf(ops_rx.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
                 ctx->para.port[MTL_PORT_R]);

  ScopedSt40Rx rx_handle(st40_rx_create(m_handle, &ops_rx));
  ASSERT_TRUE(rx_handle);
  __atomic_store_n(&rx->handle, rx_handle.get(), __ATOMIC_RELEASE);

  mtl_test::ScopedMtlStart mtl_run(m_handle);
  mtl_test::ScopedWorker sha_worker(rx.get(), std::thread(sha_frame_check, rx.get()));

  EXPECT_GE(mtl_run.start(), 0);
  /* Wait for RX to join (3 valid frames ~0.10 s) then observe 25 valid frames
   * (50 total epochs, every other frame oversize -> ~0.83 s at 59.94 fps vs
   * legacy sleep(1) + sleep(5)). */
  EXPECT_TRUE(
      mtl_test::wait_until([&]() { return mtl_test::rx_frames(rx.get()) >= 3; }, 6000));
  int sent_base = frame_done[1].load(std::memory_order_relaxed);
  int received_base = mtl_test::rx_frames(rx.get());
  EXPECT_TRUE(mtl_test::wait_until(
      [&]() {
        return frame_done[1].load(std::memory_order_relaxed) - sent_base >= 25 &&
               mtl_test::sha_checked_frames(rx.get()) >= 1;
      },
      6000));
  int sent = frame_done[1].load(std::memory_order_relaxed) - sent_base;
  int received = mtl_test::rx_frames(rx.get()) - received_base;
  const uint64_t stop_time = st_test_get_monotonic_time();
  EXPECT_GE(mtl_run.stop(), 0);
  int aborted = frame_done[0].load(std::memory_order_relaxed);
  int done = frame_done[1].load(std::memory_order_relaxed);

  sha_worker.stop();

  info("%s, aborted %d done %d, %d sent %d received\n", __func__, aborted, done, sent,
       received);
  EXPECT_GT(aborted, 0);
  EXPECT_NEAR(aborted, done, 1);
  EXPECT_GT(sent, 0);
  EXPECT_NEAR(received, sent, 2);
  EXPECT_EQ(mtl_test::relaxed_load(&rx->rx_meta_fail_cnt), 0);
  EXPECT_GT(mtl_test::sha_checked_frames(rx.get()), 0);
  EXPECT_EQ(mtl_test::relaxed_load(&rx->sha_fail_cnt), 0);
  st40_fmd_expect_stats(rx.get(), tx_handle.get(), rx_handle.get(), stop_time);
}

TEST(St40_tx, fmd_frame_oversize_aborted) {
  st40_fmd_oversize_test();
}

/* §5.1: at the lowest frame rate, empty frames still keep a packet every 42 ms */
TEST(St40_rx, fmd_frame_empty_keepalive_fps23_98) {
  St40RxScenario s;
  s.sessions = {{ST40_TYPE_FRAME_LEVEL, ST_FPS_P23_98, true, 0}};
  st40_rx_fps_test(s);
}

static void st40_fmd_annex_a_test() {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto m_handle = ctx->handle;
  fmd_annex_a annex_a;

  MTL_TEST_REQUIRE_PORTS(ctx, 2);

  fmd_annex_a_init(&annex_a);

  auto tx = make_st40_ctx(ctx, 0, 2);
  auto rx = make_st40_ctx(ctx, 0, 3);
  ASSERT_TRUE(tx != nullptr);
  ASSERT_TRUE(rx != nullptr);

  tx->st40_fmd = true;
  tx->st40_annex_a = &annex_a;
  struct st40_tx_ops ops_tx;
  st40_fmd_tx_ops_init(tx.get(), &ops_tx);
  ops_tx.type = ST40_TYPE_RTP_LEVEL;
  ops_tx.num_port = 1;
  memcpy(
      ops_tx.dip_addr[MTL_SESSION_PORT_P],
      ctx->mcast_only ? ctx->mcast_ip_addr[MTL_PORT_P] : ctx->para.sip_addr[MTL_PORT_R],
      MTL_IP_ADDR_LEN);
  ops_tx.payload_type = ST40_TEST_FMD_PAYLOAD_TYPE;

  ScopedSt40Tx tx_handle(st40_tx_create(m_handle, &ops_tx));
  ASSERT_TRUE(tx_handle);
  __atomic_store_n(&tx->handle, tx_handle.get(), __ATOMIC_RELEASE);

  rx->st40_fmd = true;
  rx->st40_annex_a = &annex_a;
  rx->frame_time = NS_PER_S / st_frame_rate(ops_tx.fps);
  struct st40_rx_ops ops_rx;
  st40_fmd_rx_ops_init(rx.get(), &ops_rx);
  ops_rx.num_port = 1;
  memcpy(
      ops_rx.ip_addr[MTL_SESSION_PORT_P],
      ctx->mcast_only ? ctx->mcast_ip_addr[MTL_PORT_P] : ctx->para.sip_addr[MTL_PORT_P],
      MTL_IP_ADDR_LEN);
  (void)snprintf(ops_rx.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
                 ctx->para.port[MTL_PORT_R]);

  ScopedSt40Rx rx_handle(st40_rx_create(m_handle, &ops_rx));
  ASSERT_TRUE(rx_handle);
  __atomic_store_n(&rx->handle, rx_handle.get(), __ATOMIC_RELEASE);

  mtl_test::ScopedMtlStart mtl_run(m_handle);
  mtl_test::ScopedWorker feed_worker(tx.get(), std::thread(tx_feed_packet, tx.get()));

  EXPECT_GE(mtl_run.start(), 0);
  /* 15 complete 2-frame cycles (~0.50 s at 59.94 fps vs legacy sleep(5)):
   * verifies both segmented objects, multi-item packets, and zero-item packets. */
  EXPECT_TRUE(mtl_test::wait_until(
      [&]() {
        return annex_a.rx_done[0].load(std::memory_order_relaxed) >= 15 &&
               annex_a.rx_done[1].load(std::memory_order_relaxed) >= 15 &&
               annex_a.rx_multi_item_pkts.load(std::memory_order_relaxed) > 0 &&
               annex_a.rx_zero_item_pkts.load(std::memory_order_relaxed) > 0;
      },
      6000));

  const uint64_t stop_time = st_test_get_monotonic_time();
  feed_worker.stop();
  EXPECT_GE(mtl_run.stop(), 0);

  info("%s, objects %d %d, %d multi-item and %d zero-item pkts\n", __func__,
       annex_a.rx_done[0].load(), annex_a.rx_done[1].load(),
       annex_a.rx_multi_item_pkts.load(), annex_a.rx_zero_item_pkts.load());
  EXPECT_EQ(mtl_test::relaxed_load(&rx->rx_meta_fail_cnt), 0);
  EXPECT_EQ(mtl_test::relaxed_load(&rx->sha_fail_cnt), 0);
  EXPECT_GT(annex_a.rx_done[0].load(), 0);
  EXPECT_GT(annex_a.rx_done[1].load(), 0);
  EXPECT_GT(annex_a.rx_multi_item_pkts.load(), 0);
  EXPECT_GT(annex_a.rx_zero_item_pkts.load(), 0);
  st40_fmd_expect_stats(rx.get(), tx_handle.get(), rx_handle.get(), stop_time);
}

/* Annex A over RTP level: multi-item, multi-DIT and zero-item packets */
TEST(St40_rx, fmd_rtp_annex_a_segments_digest) {
  st40_fmd_annex_a_test();
}

static void st40_verify_rx_switch_phase(const std::vector<ScopedSt40Rx>& rx_handles,
                                        const std::vector<tests_context*>& raw_rx,
                                        const std::vector<double>& expect_framerate,
                                        struct st_rx_source_info& src,
                                        const char* label) {
  std::vector<int> baselines(raw_rx.size(), 0);
  for (size_t i = 0; i < rx_handles.size(); i++) {
    EXPECT_GE(st40_rx_update_source(rx_handles[i].get(), &src), 0);
    baselines[i] = mtl_test::rx_frames(raw_rx[i]);
  }
  auto delta_rx = [&](tests_context* c) {
    return mtl_test::rx_frames(c) - baselines[static_cast<size_t>(c->idx)];
  };
  /* 1.0 s window with >= 25 frames after 3-frame warmup absorbs source-switch
   * settle and bounds edge error to <= 1.7% at 59.94 fps vs +/-10% tolerance. */
  auto win = mtl_test::measure_rate_window(raw_rx, delta_rx,
                                           /*warmup_frames=*/3,
                                           /*min_delta_frames=*/25,
                                           /*min_window_ms=*/1000);
  EXPECT_TRUE(win.warmed_up) << label;
  EXPECT_TRUE(win.complete) << label;
  for (size_t i = 0; i < raw_rx.size(); i++) {
    int rec = delta_rx(raw_rx[i]);
    EXPECT_GT(rec, 0);
    info("%s, session %zu fb_rec %d framerate %f for %s\n", __func__, i, rec, win.rate[i],
         label);
    EXPECT_NEAR(win.rate[i], expect_framerate[i], expect_framerate[i] * 0.1);
  }
}

static void st40_rx_update_src_test(enum st40_type type, int tx_sessions) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto m_handle = ctx->handle;

  MTL_TEST_REQUIRE_PORTS(ctx, 2);
  ASSERT_GE(tx_sessions, 1);

  const bool tx_update_dst = (tx_sessions == 1);
  const int rx_sessions = 1;

  std::vector<ScopedSt40Ctx> tx_ctxs, rx_ctxs;
  std::vector<tests_context*> raw_tx, raw_rx;
  std::vector<ScopedSt40Tx> tx_handles;
  std::vector<ScopedSt40Rx> rx_handles;
  std::vector<double> expect_framerate(rx_sessions, st_frame_rate(ST_FPS_P59_94));
  tx_ctxs.reserve(tx_sessions);
  rx_ctxs.reserve(rx_sessions);
  raw_tx.reserve(tx_sessions);
  raw_rx.reserve(rx_sessions);
  tx_handles.reserve(tx_sessions);
  rx_handles.reserve(rx_sessions);

  mtl_test::ScopedMtlStart mtl_run(m_handle);
  std::vector<mtl_test::ScopedWorker> tx_workers;

  for (int i = 0; i < tx_sessions; i++) {
    tx_ctxs.push_back(make_st40_ctx(ctx, i, 3));
    raw_tx.push_back(tx_ctxs.back().get());

    struct st40_tx_ops ops_tx;
    st40_tx_loopback_ops_init(raw_tx[i], &ops_tx, type, ST_FPS_P59_94, 30000 + i * 2);
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

    tx_handles.emplace_back(st40_tx_create(m_handle, &ops_tx));
    ASSERT_TRUE(tx_handles[i]);

    st40_tx_frame_init(raw_tx[i], tx_handles[i].get(), type);
    __atomic_store_n(&raw_tx[i]->handle, tx_handles[i].get(), __ATOMIC_RELEASE);

    if (type == ST40_TYPE_RTP_LEVEL) {
      raw_tx[i]->stop = false;
      tx_workers.emplace_back(raw_tx[i], std::thread(tx_feed_packet, raw_tx[i]));
    }
  }

  for (int i = 0; i < rx_sessions; i++) {
    rx_ctxs.push_back(make_st40_ctx(ctx, i, 3));
    raw_rx.push_back(rx_ctxs.back().get());

    struct st40_rx_ops ops_rx;
    st40_rx_loopback_ops_init(raw_rx[i], &ops_rx, 30000 + i * 2);
    if (ctx->mcast_only)
      memcpy(ops_rx.ip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_2],
             MTL_IP_ADDR_LEN);
    else
      memcpy(ops_rx.ip_addr[MTL_SESSION_PORT_P], ctx->para.sip_addr[MTL_PORT_P],
             MTL_IP_ADDR_LEN);

    rx_handles.emplace_back(st40_rx_create(m_handle, &ops_rx));
    ASSERT_TRUE(rx_handles[i]);
    __atomic_store_n(&raw_rx[i]->handle, rx_handles[i].get(), __ATOMIC_RELEASE);
  }

  EXPECT_GE(mtl_run.start(), 0);
  /* Wait for initial stream to deliver >= 20 frames (~0.33 s at 59.94 fps vs
   * legacy sleep(10)) before switching sources/destinations. */
  EXPECT_TRUE(mtl_test::wait_for_all(raw_rx, mtl_test::rx_frames, 20, 6000));

  /* Phase 1: switch to mcast port p (tx_session:1) */
  struct st_rx_source_info src;
  memset(&src, 0, sizeof(src));
  src.udp_port[MTL_SESSION_PORT_P] = 30000 + 2;
  memcpy(src.ip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
         MTL_IP_ADDR_LEN);
  if (tx_update_dst) {
    __atomic_store_n(&raw_tx[0]->seq_id, 0, __ATOMIC_RELAXED); /* reset seq id */
    struct st_tx_dest_info dst;
    memset(&dst, 0, sizeof(dst));
    dst.udp_port[MTL_SESSION_PORT_P] = 30000 + 2;
    memcpy(dst.dip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
           MTL_IP_ADDR_LEN);
    EXPECT_GE(st40_tx_update_destination(tx_handles[0].get(), &dst), 0);
  } else {
    __atomic_store_n(&raw_tx[1]->seq_id, 0, __ATOMIC_RELAXED); /* reset seq id */
  }
  st40_verify_rx_switch_phase(rx_handles, raw_rx, expect_framerate, src, "mcast 1");

  /* Phase 2: switch to mcast port r (tx_session:2) */
  if (tx_sessions > 2) {
    memset(&src, 0, sizeof(src));
    src.udp_port[MTL_SESSION_PORT_P] = 30000 + 4;
    memcpy(src.ip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_R],
           MTL_IP_ADDR_LEN);
    __atomic_store_n(&raw_tx[2]->seq_id, rand(), __ATOMIC_RELAXED); /* random seq id */
    st40_verify_rx_switch_phase(rx_handles, raw_rx, expect_framerate, src, "mcast 2");
  }

  /* Phase 3: switch to unicast (tx_session:0) */
  memset(&src, 0, sizeof(src));
  src.udp_port[MTL_SESSION_PORT_P] = 30000 + 0;
  memcpy(
      src.ip_addr[MTL_SESSION_PORT_P],
      ctx->mcast_only ? ctx->mcast_ip_addr[MTL_PORT_2] : ctx->para.sip_addr[MTL_PORT_P],
      MTL_IP_ADDR_LEN);
  __atomic_store_n(&raw_tx[0]->seq_id, rand(), __ATOMIC_RELAXED); /* random seq id */
  if (tx_update_dst) {
    struct st_tx_dest_info dst;
    memset(&dst, 0, sizeof(dst));
    dst.udp_port[MTL_SESSION_PORT_P] = 30000 + 0;
    memcpy(
        dst.dip_addr[MTL_SESSION_PORT_P],
        ctx->mcast_only ? ctx->mcast_ip_addr[MTL_PORT_2] : ctx->para.sip_addr[MTL_PORT_R],
        MTL_IP_ADDR_LEN);
    EXPECT_GE(st40_tx_update_destination(tx_handles[0].get(), &dst), 0);
  }
  st40_verify_rx_switch_phase(rx_handles, raw_rx, expect_framerate, src, "unicast 0");

  for (auto& w : tx_workers) w.stop();
  EXPECT_GE(mtl_run.stop(), 0);
}

TEST(St40_rx, update_source_rtp) {
  st40_rx_update_src_test(ST40_TYPE_RTP_LEVEL, 3);
}
TEST(St40_tx, update_dest_rtp) {
  st40_rx_update_src_test(ST40_TYPE_RTP_LEVEL, 1);
}

static void st40_after_start_test(const std::vector<St40SessionSpec>& specs, int repeat) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto m_handle = ctx->handle;
  const int sessions = static_cast<int>(specs.size());

  MTL_TEST_REQUIRE_PORTS(ctx, 2);

  mtl_test::ScopedMtlStart mtl_run(m_handle);
  EXPECT_GE(mtl_run.start(), 0);

  for (int r = 0; r < repeat; r++) {
    std::vector<ScopedSt40Ctx> tx_ctxs, rx_ctxs;
    std::vector<tests_context*> raw_tx, raw_rx;
    std::vector<ScopedSt40Tx> tx_handles;
    std::vector<ScopedSt40Rx> rx_handles;
    std::vector<double> expect_framerate(sessions);
    std::vector<mtl_test::ScopedWorker> tx_workers;
    tx_ctxs.reserve(sessions);
    rx_ctxs.reserve(sessions);
    raw_tx.reserve(sessions);
    raw_rx.reserve(sessions);
    tx_handles.reserve(sessions);
    rx_handles.reserve(sessions);

    for (int i = 0; i < sessions; i++) {
      expect_framerate[i] = st_frame_rate(specs[i].fps);
      tx_ctxs.push_back(make_st40_ctx(ctx, i, 3));
      raw_tx.push_back(tx_ctxs.back().get());

      struct st40_tx_ops ops_tx;
      st40_tx_loopback_ops_init(raw_tx[i], &ops_tx, specs[i].type, specs[i].fps,
                                30000 + i * 2);
      tx_handles.emplace_back(st40_tx_create(m_handle, &ops_tx));
      ASSERT_TRUE(tx_handles[i]);

      st40_tx_frame_init(raw_tx[i], tx_handles[i].get(), specs[i].type);
      __atomic_store_n(&raw_tx[i]->handle, tx_handles[i].get(), __ATOMIC_RELEASE);

      if (specs[i].type == ST40_TYPE_RTP_LEVEL) {
        raw_tx[i]->stop = false;
        tx_workers.emplace_back(raw_tx[i], std::thread(tx_feed_packet, raw_tx[i]));
      }
    }

    for (int i = 0; i < sessions; i++) {
      rx_ctxs.push_back(make_st40_ctx(ctx, i, 3));
      raw_rx.push_back(rx_ctxs.back().get());

      struct st40_rx_ops ops_rx;
      st40_rx_loopback_ops_init(raw_rx[i], &ops_rx, 30000 + i * 2);
      rx_handles.emplace_back(st40_rx_create(m_handle, &ops_rx));
      ASSERT_TRUE(rx_handles[i]);
      __atomic_store_n(&raw_rx[i]->handle, rx_handles[i].get(), __ATOMIC_RELEASE);
    }

    /* 1.0 s window with >= 25 frames after 3-frame warmup (~1.05 s/repeat vs
     * legacy sleep(10)); <= 2% edge error at 50-59.94 fps vs +/-10% tolerance. */
    auto win = mtl_test::measure_rate_window(raw_rx, mtl_test::rx_frames,
                                             /*warmup_frames=*/3,
                                             /*min_delta_frames=*/25,
                                             /*min_window_ms=*/1000);
    EXPECT_TRUE(win.warmed_up);
    EXPECT_TRUE(win.complete);

    for (auto& w : tx_workers) w.stop();

    for (int i = 0; i < sessions; i++) {
      int rec = mtl_test::rx_frames(raw_rx[i]);
      EXPECT_GT(rec, 0);
      info("%s, session %d fb_rec %d framerate %f\n", __func__, i, rec, win.rate[i]);
      EXPECT_NEAR(win.rate[i], expect_framerate[i], expect_framerate[i] * 0.1);
    }
  }

  EXPECT_GE(mtl_run.stop(), 0);
}

TEST(St40_rx, after_start_mix_s2_r2) {
  st40_after_start_test(
      {
          {ST40_TYPE_RTP_LEVEL, ST_FPS_P50},
          {ST40_TYPE_FRAME_LEVEL, ST_FPS_P59_94},
      },
      /*repeat=*/2);
}

namespace {

struct St40RxFrameLevelState {
  tests_context* ctx = nullptr;
  std::atomic<st40_rx_handle> rx_handle{nullptr};
  size_t expected_udw_size = 0;
  std::atomic<int> put_ok{0};
  std::atomic<int> put_fail{0};
  std::queue<void*> pending_frames;
};

int rx_anc_frame_ready(void* priv, void* frame, struct st40_rx_frame_meta* meta) {
  auto* st = static_cast<St40RxFrameLevelState*>(priv);
  auto* ctx = st ? st->ctx : nullptr;
  st40_rx_handle handle = st ? st->rx_handle.load(std::memory_order_acquire) : nullptr;
  if (!ctx || !handle || ctx->stop.load(std::memory_order_relaxed)) {
    return -EBUSY;
  }

  bool meta_ok = frame != nullptr && meta != nullptr && meta->meta != nullptr &&
                 meta->meta_num == 1 && meta->udw_buffer_fill == st->expected_udw_size &&
                 meta->meta[0].did == 0x43 && meta->meta[0].sdid == 0x02 &&
                 meta->meta[0].udw_size == st->expected_udw_size &&
                 meta->status == ST_FRAME_STATUS_COMPLETE;
  if (!meta_ok) {
    __atomic_fetch_add(&ctx->rx_meta_fail_cnt, 1, __ATOMIC_RELAXED);
  }

  {
    std::lock_guard<std::mutex> lck(ctx->mtx);
    st->pending_frames.push(frame);
    ctx->cv.notify_one();
  }
  __atomic_fetch_add(&ctx->fb_rec, 1, __ATOMIC_RELAXED);
  return 0;
}

void st40_rx_frame_level_consumer(St40RxFrameLevelState* st) {
  auto* ctx = st->ctx;
  while (true) {
    void* frame = nullptr;
    {
      std::unique_lock<std::mutex> lck(ctx->mtx);
      ctx->cv.wait(lck, [&]() {
        return !st->pending_frames.empty() || ctx->stop.load(std::memory_order_relaxed);
      });
      if (st->pending_frames.empty()) break;
      frame = st->pending_frames.front();
      st->pending_frames.pop();
    }

    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(static_cast<const unsigned char*>(frame), st->expected_udw_size, digest);
    bool matched = false;
    for (int i = 0; i < TEST_SHA_HIST_NUM; i++) {
      if (memcmp(digest, ctx->shas[i], SHA256_DIGEST_LENGTH) == 0) {
        matched = true;
        break;
      }
    }
    if (!matched) {
      __atomic_fetch_add(&ctx->sha_fail_cnt, 1, __ATOMIC_RELAXED);
    }
    __atomic_fetch_add(&ctx->check_sha_frame_cnt, 1, __ATOMIC_RELAXED);

    st40_rx_handle handle = st->rx_handle.load(std::memory_order_acquire);
    if (handle && st40_rx_put_framebuff(handle, frame) == 0) {
      st->put_ok.fetch_add(1, std::memory_order_relaxed);
    } else {
      st->put_fail.fetch_add(1, std::memory_order_relaxed);
    }
  }
}

} /* namespace */

/* End-to-end ST40_TYPE_FRAME_LEVEL RX test covering notify_frame_ready, UDW
 * payload digest verification, and st40_rx_put_framebuff slot release. */
TEST(St40_rx, frame_level_put_framebuff) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto m_handle = ctx->handle;

  MTL_TEST_REQUIRE_PORTS(ctx, 2);

  constexpr uint16_t kUdpPort = 30400;
  constexpr size_t kUdwSize = 240;

  auto tx = make_st40_ctx(ctx, 0, TEST_SHA_HIST_NUM, /*check_sha=*/true);
  auto rx = make_st40_ctx(ctx, 0, 4, /*check_sha=*/true);
  ASSERT_TRUE(tx != nullptr);
  ASSERT_TRUE(rx != nullptr);

  St40RxFrameLevelState state;
  state.ctx = rx.get();
  state.expected_udw_size = kUdwSize;

  struct st40_tx_ops ops_tx;
  st40_tx_loopback_ops_init(tx.get(), &ops_tx, ST40_TYPE_FRAME_LEVEL, ST_FPS_P59_94,
                            kUdpPort);
  ScopedSt40Tx tx_handle(st40_tx_create(m_handle, &ops_tx));
  ASSERT_TRUE(tx_handle);
  st40_tx_frame_init(tx.get(), tx_handle.get(), ST40_TYPE_FRAME_LEVEL, kUdwSize);
  for (int frame = 0; frame < tx->fb_cnt; frame++) {
    st_test_rand_data(tx->frame_buf[frame], kUdwSize, frame);
    SHA256(tx->frame_buf[frame], kUdwSize, tx->shas[frame]);
  }
  memcpy(rx->shas, tx->shas, TEST_SHA_HIST_NUM * SHA256_DIGEST_LENGTH);
  __atomic_store_n(&tx->handle, tx_handle.get(), __ATOMIC_RELEASE);

  struct st40_rx_ops ops_rx;
  st40_rx_loopback_ops_init(rx.get(), &ops_rx, kUdpPort);
  ops_rx.priv = &state;
  ops_rx.type = ST40_TYPE_FRAME_LEVEL;
  ops_rx.notify_rtp_ready = nullptr;
  ops_rx.rtp_ring_size = 0;
  ops_rx.framebuff_cnt = 4;
  ops_rx.framebuff_size = 1024;
  ops_rx.notify_frame_ready = rx_anc_frame_ready;

  ScopedSt40Rx rx_handle(st40_rx_create(m_handle, &ops_rx));
  ASSERT_TRUE(rx_handle);
  __atomic_store_n(&rx->handle, rx_handle.get(), __ATOMIC_RELEASE);
  state.rx_handle.store(rx_handle.get(), std::memory_order_release);

  /* Destruction order: mtl_run.stop() -> consumer.stop() (drains queue via
   * st40_rx_put_framebuff) -> rx_handle -> tx_handle -> state -> rx -> tx. */
  mtl_test::ScopedWorker consumer(rx.get(),
                                  std::thread(st40_rx_frame_level_consumer, &state));
  mtl_test::ScopedMtlStart mtl_run(m_handle);

  EXPECT_GE(mtl_run.start(), 0);
  /* 20 frames at 59.94 fps (~0.33 s) cycles the 4-slot RX pool 5 times. */
  EXPECT_TRUE(mtl_test::wait_until(
      [&]() { return mtl_test::rx_frames(rx.get()) >= 20 && state.put_ok.load() >= 20; },
      6000));

  EXPECT_GE(mtl_run.stop(), 0);
  consumer.stop();

  const int rec = mtl_test::rx_frames(rx.get());
  EXPECT_GE(rec, 20);
  EXPECT_EQ(state.put_ok.load(), rec);
  EXPECT_EQ(state.put_fail.load(), 0);
  EXPECT_EQ(mtl_test::relaxed_load(&rx->rx_meta_fail_cnt), 0);
  EXPECT_EQ(mtl_test::relaxed_load(&rx->sha_fail_cnt), 0);
  EXPECT_GE(mtl_test::sha_checked_frames(rx.get()), 20);

  /* Invalid pointers must fail cleanly without corrupting the pool. */
  uint8_t bogus = 0;
  EXPECT_LT(st40_rx_put_framebuff(rx_handle.get(), nullptr), 0);
  EXPECT_LT(st40_rx_put_framebuff(rx_handle.get(), &bogus), 0);
}

/* Verify st40 TX and RX session stats accumulate under traffic and reset cleanly. */
TEST(St40_rx, session_stats_reset) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto m_handle = ctx->handle;

  MTL_TEST_REQUIRE_PORTS(ctx, 2);

  constexpr uint16_t kUdpPort = 30410;
  auto tx = make_st40_ctx(ctx, 0, 3);
  auto rx = make_st40_ctx(ctx, 0, 3);
  ASSERT_TRUE(tx != nullptr);
  ASSERT_TRUE(rx != nullptr);

  struct st40_tx_ops ops_tx;
  st40_tx_loopback_ops_init(tx.get(), &ops_tx, ST40_TYPE_FRAME_LEVEL, ST_FPS_P59_94,
                            kUdpPort);
  ScopedSt40Tx tx_handle(st40_tx_create(m_handle, &ops_tx));
  ASSERT_TRUE(tx_handle);
  st40_tx_frame_init(tx.get(), tx_handle.get(), ST40_TYPE_FRAME_LEVEL);
  __atomic_store_n(&tx->handle, tx_handle.get(), __ATOMIC_RELEASE);

  struct st40_rx_ops ops_rx;
  st40_rx_loopback_ops_init(rx.get(), &ops_rx, kUdpPort);
  ScopedSt40Rx rx_handle(st40_rx_create(m_handle, &ops_rx));
  ASSERT_TRUE(rx_handle);
  __atomic_store_n(&rx->handle, rx_handle.get(), __ATOMIC_RELEASE);

  mtl_test::ScopedMtlStart mtl_run(m_handle);
  EXPECT_GE(mtl_run.start(), 0);
  /* 15 frames at 59.94 fps (~0.25 s) is sufficient to populate TX and RX stats. */
  EXPECT_TRUE(
      mtl_test::wait_until([&]() { return mtl_test::rx_frames(rx.get()) >= 15; }, 6000));
  EXPECT_GE(mtl_run.stop(), 0);

  struct st40_tx_user_stats tx_stats;
  struct st40_rx_user_stats rx_stats;
  ASSERT_GE(st40_tx_get_session_stats(tx_handle.get(), &tx_stats), 0);
  ASSERT_GE(st40_rx_get_session_stats(rx_handle.get(), &rx_stats), 0);
  EXPECT_GT(tx_stats.common.port[MTL_SESSION_PORT_P].packets, 0u);
  EXPECT_GT(tx_stats.common.port[MTL_SESSION_PORT_P].bytes, 0u);
  EXPECT_GT(tx_stats.common.port[MTL_SESSION_PORT_P].frames, 0u);
  EXPECT_GT(rx_stats.common.port[MTL_SESSION_PORT_P].packets, 0u);
  EXPECT_GT(rx_stats.common.port[MTL_SESSION_PORT_P].bytes, 0u);

  EXPECT_GE(st40_tx_reset_session_stats(tx_handle.get()), 0);
  EXPECT_GE(st40_rx_reset_session_stats(rx_handle.get()), 0);

  ASSERT_GE(st40_tx_get_session_stats(tx_handle.get(), &tx_stats), 0);
  ASSERT_GE(st40_rx_get_session_stats(rx_handle.get(), &rx_stats), 0);
  EXPECT_EQ(tx_stats.common.port[MTL_SESSION_PORT_P].packets, 0u);
  EXPECT_EQ(tx_stats.common.port[MTL_SESSION_PORT_P].bytes, 0u);
  EXPECT_EQ(tx_stats.common.port[MTL_SESSION_PORT_P].frames, 0u);
  EXPECT_EQ(rx_stats.common.port[MTL_SESSION_PORT_P].packets, 0u);
  EXPECT_EQ(rx_stats.common.port[MTL_SESSION_PORT_P].bytes, 0u);
  EXPECT_EQ(rx_stats.common.port[MTL_SESSION_PORT_P].frames, 0u);

  /* Invalid argument handling. */
  EXPECT_LT(st40_tx_get_session_stats(nullptr, &tx_stats), 0);
  EXPECT_LT(st40_tx_get_session_stats(tx_handle.get(), nullptr), 0);
  EXPECT_LT(st40_tx_reset_session_stats(nullptr), 0);
  EXPECT_LT(st40_rx_get_session_stats(nullptr, &rx_stats), 0);
  EXPECT_LT(st40_rx_get_session_stats(rx_handle.get(), nullptr), 0);
  EXPECT_LT(st40_rx_reset_session_stats(nullptr), 0);
}
