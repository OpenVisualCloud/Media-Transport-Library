/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include <algorithm>
#include <thread>
#include <vector>

#include "log.h"
#include "tests.hpp"

#define ST40_TEST_PAYLOAD_TYPE (113)
#define ST40_TEST_FMD_PAYLOAD_TYPE (115) /* the library default for fast metadata */
#define ST40_TEST_FMD_DIT (0x2a5a5a)
#define ST40_TEST_FMD_K_BIT (1)
#define ST40_TEST_FMD_PADDED(size) (((size) + 3) & ~3)
/* ST 2110-10 Standard UDP Size Limit minus the UDP header */
#define ST40_TEST_FMD_MAX_RTP_BYTES (1452)
#define ST40_TEST_FMD_MAX_GAP_NS (500 * (uint64_t)NS_PER_MS)

static int tx_anc_next_frame(void* priv, uint16_t* next_frame_idx,
                             struct st40_tx_frame_meta* meta) {
  auto ctx = (tests_context*)priv;

  if (ctx->stop) return -EIO;
  return tx_next_frame(priv, next_frame_idx);
}

static int tx_anc_next_frame_timestamp(void* priv, uint16_t* next_frame_idx,
                                       struct st40_tx_frame_meta* meta) {
  auto ctx = (tests_context*)priv;

  if (!ctx->handle) return -EIO; /* not ready */

  meta->tfmt = ST10_TIMESTAMP_FMT_TAI;
  meta->timestamp = mtl_ptp_read_time(ctx->ctx->handle) + 40 * 1000 * 1000;
  *next_frame_idx = ctx->fb_idx;
  dbg("%s, next_frame_idx %d\n", __func__, *next_frame_idx);
  ctx->fb_idx++;
  if (ctx->fb_idx >= ctx->fb_cnt) ctx->fb_idx = 0;
  ctx->fb_send++;
  if (!ctx->start_time) ctx->start_time = st_test_get_monotonic_time();
  return 0;
}

static int tx_fmd_build_rtp_packet(tests_context* s, struct st40_fmd_rtp_hdr* rtp,
                                   uint16_t* pkt_len) {
  size_t padded_size = ST40_TEST_FMD_PADDED(s->frame_size);

  memset(rtp, 0x0, sizeof(*rtp) + padded_size);
  rtp->base.payload_type = ST40_TEST_FMD_PAYLOAD_TYPE;
  rtp->base.version = 2;
  rtp->base.tmstamp = s->rtp_tmstamp;
  rtp->base.ssrc = htonl(0x88888888 + s->idx);
  rtp->base.seq_number = htons((uint16_t)s->seq_id);
  s->rtp_tmstamp++;
  s->seq_id++;
  rtp->fmd_hdr_chunk.data_item_type = ST40_TEST_FMD_DIT;
  rtp->fmd_hdr_chunk.data_item_k_bit = ST40_TEST_FMD_K_BIT;
  rtp->fmd_hdr_chunk.data_item_length = padded_size / 4;
  rtp->swapped_fmd_hdr_chunk = htonl(rtp->swapped_fmd_hdr_chunk);
  memcpy(&rtp[1], s->frame_buf[s->seq_id % TEST_SHA_HIST_NUM], s->frame_size);
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
  int rx_done[2] = {0, 0};
  int rx_zero_item_pkts = 0, rx_multi_item_pkts = 0;
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

  memset(hdr, 0, sizeof(*hdr));
  hdr->version = 2;
  hdr->payload_type = ST40_TEST_FMD_PAYLOAD_TYPE;
  hdr->seq_number = htons((uint16_t)s->seq_id++);
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
  rtp->base.seq_number = htons((uint16_t)s->seq_id);
  rtp->seq_number_ext = htons((uint16_t)(s->seq_id >> 16));
  s->rtp_tmstamp++;
  s->seq_id++;
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
      st40_set_udw(i + 3,
                   st40_add_parity_bits(s->frame_buf[s->seq_id % TEST_SHA_HIST_NUM][i]),
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
  void* mbuf;
  void* usrptr = NULL;
  uint16_t mbuf_len = 0;
  std::unique_lock<std::mutex> lck(ctx->mtx, std::defer_lock);
  while (!ctx->stop) {
    /* get available buffer*/
    mbuf = st40_tx_get_mbuf((st40_tx_handle)ctx->handle, &usrptr);
    if (!mbuf) {
      lck.lock();
      /* try again */
      mbuf = st40_tx_get_mbuf((st40_tx_handle)ctx->handle, &usrptr);
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
    st40_tx_put_mbuf((st40_tx_handle)ctx->handle, mbuf, mbuf_len);
  }
}

static int tx_rtp_done(void* args) {
  auto ctx = (tests_context*)args;

  if (!ctx->handle) return -EIO; /* not ready */

  std::unique_lock<std::mutex> lck(ctx->mtx);
  ctx->cv.notify_all();
  if (!ctx->start_time) ctx->start_time = st_test_get_monotonic_time();
  ctx->fb_send++;
  return 0;
}

static void rx_handle_rtp(tests_context* s, struct st40_rfc8331_rtp_hdr* hdr) {
  struct st40_rfc8331_payload_hdr* payload_hdr =
      (struct st40_rfc8331_payload_hdr*)(&hdr[1]);
  int anc_count = hdr->first_hdr_chunk.anc_count;
  int idx;

  for (idx = 0; idx < anc_count; idx++) {
    st40_rfc8331_payload_hdr_bswap(payload_hdr);
    if (!st40_check_parity_bits(payload_hdr->second_hdr_chunk.did) ||
        !st40_check_parity_bits(payload_hdr->second_hdr_chunk.sdid) ||
        !st40_check_parity_bits(payload_hdr->second_hdr_chunk.data_count)) {
      err("anc RTP checkParityBits for payload hdr error\n");
      s->rx_meta_fail_cnt++;
      return;
    }
    int udw_size = payload_hdr->second_hdr_chunk.data_count & 0xff;

    // verify checksum
    uint16_t checksum = 0;
    checksum = st40_get_udw(udw_size + 3, (uint8_t*)&payload_hdr->second_hdr_chunk);
    payload_hdr->swapped_second_hdr_chunk = htonl(payload_hdr->swapped_second_hdr_chunk);
    if (checksum !=
        st40_calc_checksum(3 + udw_size, (uint8_t*)&payload_hdr->second_hdr_chunk)) {
      s->sha_fail_cnt++;
      return;
    }
    // get payload
    uint16_t data;
    uint8_t* udw = (uint8_t*)st_test_zmalloc(udw_size);
    ASSERT_TRUE(udw != NULL);
    for (int i = 0; i < udw_size; i++) {
      data = st40_get_udw(i + 3, (uint8_t*)&payload_hdr->second_hdr_chunk);
      if (!st40_check_parity_bits(data)) {
        err("anc RTP checkParityBits for udw error\n");
        s->rx_meta_fail_cnt++;
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
    int16_t lost = (int16_t)(seq - (uint16_t)(s->seq_id + 1));
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
        s->rx_meta_fail_cnt++;
      }
    }
  }
  s->last_rx_time = now;
  s->seq_id = seq;
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
    s->rx_meta_fail_cnt++;
    return;
  }
  if (!s->check_sha) return;

  uint8_t* data = (uint8_t*)st_test_zmalloc(s->frame_size);
  ASSERT_TRUE(data != nullptr);
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
        if (memcmp(result, a->sha[o], sizeof(result))) s->sha_fail_cnt++;
        for (size_t i = a->obj_size[o]; i < a->rx_obj[o].size(); i++)
          if (a->rx_obj[o][i]) s->sha_fail_cnt++;
        a->rx_done[o]++;
        a->rx_next[o] = -1;
      }
    }
    off += 4 + length * 4;
  }
  if (!valid || off != len) {
    err("%s(%d), invalid Annex A packet\n", __func__, s->idx);
    s->rx_meta_fail_cnt++;
    return;
  }
  if (!items) a->rx_zero_item_pkts++;
  if (items > 1) a->rx_multi_item_pkts++;
}

static int rx_rtp_ready(void* priv) {
  auto ctx = (tests_context*)priv;
  void* useptr;
  void* mbuf;
  uint16_t len;

  if (!ctx->handle) return -EIO;

  while (1) {
    mbuf = st40_rx_get_mbuf((st40_rx_handle)ctx->handle, &useptr, &len);
    if (!mbuf) break; /* no mbuf */
    if (ctx->st40_annex_a)
      rx_handle_fmd_annex_a(ctx, (uint8_t*)useptr, len);
    else if (ctx->st40_fmd)
      rx_handle_fmd(ctx, (struct st40_fmd_rtp_hdr*)useptr, len);
    else if (ctx->check_sha)
      rx_handle_rtp(ctx, (struct st40_rfc8331_rtp_hdr*)useptr);
    st40_rx_put_mbuf((st40_rx_handle)ctx->handle, mbuf);
    ctx->fb_rec++;
  }

  if (!ctx->start_time) ctx->start_time = st_test_get_monotonic_time();

  return 0;
}

static void st40_rx_ops_init(tests_context* st40, struct st40_rx_ops* ops) {
  auto ctx = st40->ctx;

  memset(ops, 0, sizeof(*ops));
  ops->name = "st40_test";
  ops->priv = st40;
  ops->num_port = ctx->para.num_ports;
  if (ctx->same_dual_port) ops->num_port = 1;
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
  ops->num_port = ctx->para.num_ports;
  if (ctx->same_dual_port) ops->num_port = 1;
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

static void st40_tx_assert_cnt(int expect_s40_tx_cnt) {
  auto ctx = st_test_ctx();
  auto handle = ctx->handle;
  struct st_var_info var;
  int ret;

  ret = st_get_var_info(handle, &var);
  EXPECT_GE(ret, 0);
  EXPECT_EQ(var.st40_tx_sessions_cnt, expect_s40_tx_cnt);
}

static void st40_rx_assert_cnt(int expect_s40_rx_cnt) {
  auto ctx = st_test_ctx();
  auto handle = ctx->handle;
  struct st_var_info var;
  int ret;

  ret = st_get_var_info(handle, &var);
  EXPECT_GE(ret, 0);
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

static void st40_fmd_tx_create_free_test(int sessions) {
  auto ctx = st_test_ctx();
  tests_context st40;
  struct st40_tx_ops ops;
  std::vector<st40_tx_handle> handle(sessions);

  st40.ctx = ctx;
  st40.fb_cnt = 2;
  st40_fmd_tx_ops_init(&st40, &ops);
  for (int i = 0; i < sessions; i++) {
    handle[i] = st40_tx_create(ctx->handle, &ops);
    ASSERT_TRUE(handle[i] != nullptr);
    ops.udp_port[MTL_SESSION_PORT_P]++;
    ops.udp_port[MTL_SESSION_PORT_R]++;
    st40_tx_assert_cnt(i + 1);
  }
  for (int i = sessions - 1; i >= 0; i--) {
    EXPECT_GE(st40_tx_free(handle[i]), 0);
    st40_tx_assert_cnt(i);
  }
}

static void st40_fmd_rx_create_free_test(int sessions) {
  auto ctx = st_test_ctx();
  tests_context st40;
  struct st40_rx_ops ops;
  std::vector<st40_rx_handle> handle(sessions);

  st40.ctx = ctx;
  st40_fmd_rx_ops_init(&st40, &ops);
  for (int i = 0; i < sessions; i++) {
    handle[i] = st40_rx_create(ctx->handle, &ops);
    ASSERT_TRUE(handle[i] != nullptr);
    ops.udp_port[MTL_SESSION_PORT_P]++;
    ops.udp_port[MTL_SESSION_PORT_R]++;
    st40_rx_assert_cnt(i + 1);
  }
  for (int i = sessions - 1; i >= 0; i--) {
    EXPECT_GE(st40_rx_free(handle[i]), 0);
    st40_rx_assert_cnt(i);
  }
}

static bool st40_fmd_tx_ops_accepted(uint32_t extra_flags, uint32_t dit, uint8_t k_bit,
                                     uint8_t payload_type = ST40_TEST_FMD_PAYLOAD_TYPE) {
  auto ctx = st_test_ctx();
  tests_context st40;
  struct st40_tx_ops ops;

  st40.ctx = ctx;
  st40.fb_cnt = 2;
  st40_fmd_tx_ops_init(&st40, &ops);
  ops.flags |= extra_flags;
  ops.fmd_dit = dit;
  ops.fmd_k_bit = k_bit;
  ops.payload_type = payload_type;
  st40_tx_handle handle = st40_tx_create(ctx->handle, &ops);
  if (!handle) return false;
  EXPECT_GE(st40_tx_free(handle), 0);
  return true;
}

static void st40_fmd_expect_stats(tests_context* rx, st40_tx_handle tx_handle,
                                  st40_rx_handle rx_handle, bool expect_gap = false) {
  struct st40_tx_user_stats tx_stats;
  struct st40_rx_user_stats rx_stats;

  if (rx->last_rx_time)
    rx->max_rx_gap =
        std::max(rx->max_rx_gap, st_test_get_monotonic_time() - rx->last_rx_time);
  if (expect_gap)
    EXPECT_GE(rx->max_rx_gap, ST40_TEST_FMD_MAX_GAP_NS);
  else
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

static int st40_fmd_result(const tests_context* rx) {
  return rx->max_rx_gap < ST40_TEST_FMD_MAX_GAP_NS ? 0 : -EIO;
}

TEST(St40_tx, create_free_single) {
  create_free_test(st40_tx, 0, 1, 1);
}
TEST(St40_tx, create_free_multi) {
  create_free_test(st40_tx, 0, 1, 6);
}
TEST(St40_tx, create_free_mix) {
  create_free_test(st40_tx, 2, 3, 4);
}
TEST(St40_tx, create_free_max) {
  create_free_max(st40_tx, TEST_CREATE_FREE_MAX);
}
TEST(St40_tx, create_expect_fail) {
  expect_fail_test(st40_tx);
}
TEST(St40_tx, create_expect_fail_ring_sz) {
  uint16_t ring_size = 0;
  expect_fail_test_rtp_ring(st40_tx, ST40_TYPE_RTP_LEVEL, ring_size);
  ring_size = 128 + 1;
  expect_fail_test_rtp_ring(st40_tx, ST40_TYPE_RTP_LEVEL, ring_size);
}
TEST(St40_tx, get_framebuffer) {
  uint16_t fbcnt = 3;
  test_get_framebuffer(st40_tx, fbcnt);
  fbcnt = 1000;
  test_get_framebuffer(st40_tx, fbcnt);
}
TEST(St40_tx, get_framebuffer_expect_fail) {
  uint16_t fbcnt = 3;
  expect_fail_test_get_framebuffer(st40_tx, fbcnt);
  fbcnt = 1000;
  expect_fail_test_get_framebuffer(st40_tx, fbcnt);
}

TEST(St40_rx, create_free_single) {
  create_free_test(st40_rx, 0, 1, 1);
}
TEST(St40_rx, create_free_multi) {
  create_free_test(st40_rx, 0, 1, 6);
}
TEST(St40_rx, create_free_mix) {
  create_free_test(st40_rx, 2, 3, 4);
}
TEST(St40_rx, create_free_max) {
  create_free_max(st40_rx, TEST_CREATE_FREE_MAX);
}
TEST(St40_rx, create_expect_fail) {
  expect_fail_test(st40_rx);
}
TEST(St40_rx, create_expect_fail_ring_sz) {
  uint16_t ring_size = 0;
  expect_fail_test_rtp_ring_2(st40_rx, ring_size);
  ring_size = 128 + 1;
  expect_fail_test_rtp_ring_2(st40_rx, ring_size);
}

TEST(St40_tx, fmd_create_free_single) {
  st40_fmd_tx_create_free_test(1);
}
TEST(St40_tx, fmd_create_free_multi) {
  st40_fmd_tx_create_free_test(4);
}
TEST(St40_tx, fmd_create_expect_fail_split_anc) {
  EXPECT_TRUE(st40_fmd_tx_ops_accepted(0, ST40_TEST_FMD_DIT, ST40_TEST_FMD_K_BIT));
  EXPECT_FALSE(st40_fmd_tx_ops_accepted(ST40_TX_FLAG_SPLIT_ANC_BY_PKT, ST40_TEST_FMD_DIT,
                                        ST40_TEST_FMD_K_BIT));
}
TEST(St40_tx, fmd_create_expect_fail_dit) {
  EXPECT_TRUE(st40_fmd_tx_ops_accepted(0, 0x3fffff, 0));
  EXPECT_FALSE(st40_fmd_tx_ops_accepted(0, 0x3fffff + 1, 0));
}
TEST(St40_tx, fmd_create_expect_fail_k_bit) {
  EXPECT_TRUE(st40_fmd_tx_ops_accepted(0, 0, 1));
  EXPECT_FALSE(st40_fmd_tx_ops_accepted(0, 0, 2));
}
TEST(St40_tx, fmd_create_expect_fail_reserved_dit) {
  EXPECT_TRUE(st40_fmd_tx_ops_accepted(0, 0x2fffff, 0));
  EXPECT_FALSE(st40_fmd_tx_ops_accepted(0, 0x300000, 0));
  EXPECT_FALSE(st40_fmd_tx_ops_accepted(0, 0x3fefff, 0));
  EXPECT_TRUE(st40_fmd_tx_ops_accepted(0, 0x3ff000, 0));
}
TEST(St40_tx, fmd_create_expect_fail_payload_type) {
  EXPECT_FALSE(st40_fmd_tx_ops_accepted(0, ST40_TEST_FMD_DIT, 0, 95));
  EXPECT_TRUE(st40_fmd_tx_ops_accepted(0, ST40_TEST_FMD_DIT, 0, 96));
  EXPECT_TRUE(st40_fmd_tx_ops_accepted(0, ST40_TEST_FMD_DIT, 0, 127));
}

static int rx_frame_ready_unused(void* priv, void* frame,
                                 struct st40_rx_frame_meta* meta) {
  return 0;
}

TEST(St40_rx, fmd_create_free_single) {
  st40_fmd_rx_create_free_test(1);
}
TEST(St40_rx, fmd_create_free_multi) {
  st40_fmd_rx_create_free_test(4);
}
TEST(St40_rx, fmd_create_expect_fail_frame_level) {
  auto ctx = st_test_ctx();
  tests_context st40;
  struct st40_rx_ops ops;

  st40.ctx = ctx;
  st40_fmd_rx_ops_init(&st40, &ops);
  ops.type = ST40_TYPE_FRAME_LEVEL;
  ops.notify_frame_ready = rx_frame_ready_unused;
  ops.framebuff_cnt = 2;
  ops.framebuff_size = 2048;
  EXPECT_TRUE(st40_rx_create(ctx->handle, &ops) == nullptr);

  /* the same FRAME_LEVEL ops without the opt-in are valid */
  ops.flags &= ~ST40_RX_FLAG_FAST_METADATA;
  st40_rx_handle handle = st40_rx_create(ctx->handle, &ops);
  ASSERT_TRUE(handle != nullptr);
  EXPECT_GE(st40_rx_free(handle), 0);
}

static void st40_tx_frame_init(tests_context* st40, st40_tx_handle handle,
                               enum st40_type type, size_t frame_size = 240) {
  if (st40->st40_empty_frame) frame_size = 0;

  st40->pkt_data_len = frame_size;
  st40->frame_size = frame_size;

  for (int frame = 0; frame < st40->fb_cnt; frame++) {
    st40->frame_buf[frame] = (uint8_t*)st_test_zmalloc(frame_size);
    ASSERT_TRUE(st40->frame_buf[frame] != NULL);

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
      if (st40->st40_empty_frame || st40->st40_fmd)
        dst->meta_num = 0;
      else
        dst->meta_num = 1;
      dst->data = st40->frame_buf[frame];
    }
  }
}

static void st40_tx_frame_uinit(tests_context* st40) {
  for (int frame = 0; frame < st40->fb_cnt; frame++) {
    if (st40->frame_buf[frame]) {
      st_test_free(st40->frame_buf[frame]);
      st40->frame_buf[frame] = NULL;
    }
  }
}

static void st40_tx_fps_test(enum st40_type type[], enum st_fps fps[],
                             enum st_test_level level, int sessions = 1) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto m_handle = ctx->handle;
  int ret;
  struct st40_tx_ops ops;

  std::vector<tests_context*> test_ctx;
  std::vector<st40_tx_handle> handle;
  std::vector<double> expect_framerate;
  std::vector<double> framerate;
  std::vector<std::thread> rtp_thread;

  /* return if level small than global */
  if (level < ctx->level) return;

  test_ctx.resize(sessions);
  handle.resize(sessions);
  expect_framerate.resize(sessions);
  framerate.resize(sessions);
  rtp_thread.resize(sessions);

  for (int i = 0; i < sessions; i++) {
    expect_framerate[i] = st_frame_rate(fps[i]);
    test_ctx[i] = new tests_context();
    ASSERT_TRUE(test_ctx[i] != NULL);

    test_ctx[i]->idx = i;
    test_ctx[i]->ctx = ctx;
    test_ctx[i]->fb_cnt = 3;
    test_ctx[i]->fb_idx = 0;
    st40_tx_ops_init(test_ctx[i], &ops);
    ops.type = type[i];
    ops.fps = fps[i];

    handle[i] = st40_tx_create(m_handle, &ops);
    ASSERT_TRUE(handle[i] != NULL);

    st40_tx_frame_init(test_ctx[i], handle[i], type[i]);

    test_ctx[i]->handle = handle[i];

    if (type[i] == ST40_TYPE_RTP_LEVEL) {
      test_ctx[i]->stop = false;
      rtp_thread[i] = std::thread(tx_feed_packet, test_ctx[i]);
    }
  }

  ret = mtl_start(m_handle);
  EXPECT_GE(ret, 0);
  sleep(5);

  for (int i = 0; i < sessions; i++) {
    uint64_t cur_time_ns = st_test_get_monotonic_time();
    double time_sec = (double)(cur_time_ns - test_ctx[i]->start_time) / NS_PER_S;
    framerate[i] = test_ctx[i]->fb_send / time_sec;
    if (type[i] == ST40_TYPE_RTP_LEVEL) {
      test_ctx[i]->stop = true;
      {
        std::unique_lock<std::mutex> lck(test_ctx[i]->mtx);
        test_ctx[i]->cv.notify_all();
      }
      rtp_thread[i].join();
    }
  }

  ret = mtl_stop(m_handle);
  EXPECT_GE(ret, 0);

  for (int i = 0; i < sessions; i++) {
    EXPECT_GT(test_ctx[i]->fb_send, 0);
    info("%s, session %d fb_send %d framerate %f\n", __func__, i, test_ctx[i]->fb_send,
         framerate[i]);
    EXPECT_NEAR(framerate[i], expect_framerate[i], expect_framerate[i] * 0.1);
    ret = st40_tx_free(handle[i]);
    EXPECT_GE(ret, 0);
    st40_tx_frame_uinit(test_ctx[i]);
    delete test_ctx[i];
  }
}

static void st40_rx_fps_test(enum st40_type type[], enum st_fps fps[],
                             enum st_test_level level, int sessions = 1,
                             bool check_sha = false, bool user_timestamp = false,
                             bool empty_frame = false, bool interlaced = false,
                             bool dedicate_tx_queue = false,
                             const bool* fmd_session = nullptr,
                             const uint32_t* fmd_data_size = nullptr,
                             bool fmd_trailing_gap = false) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto m_handle = ctx->handle;
  int ret;
  struct st40_tx_ops ops_tx;
  struct st40_rx_ops ops_rx;

  /* return if level small than global */
  if (level < ctx->level) return;

  if (ctx->para.num_ports < 2) {
    info(
        "%s, dual port should be enabled for tx test, one for tx and one for "
        "rx\n",
        __func__);
    throw std::runtime_error("Dual port not enabled");
  }

  std::vector<tests_context*> test_ctx_tx;
  std::vector<tests_context*> test_ctx_rx;
  std::vector<st40_tx_handle> tx_handle;
  std::vector<st40_rx_handle> rx_handle;
  std::vector<double> expect_framerate;
  std::vector<double> framerate;
  std::vector<std::thread> rtp_thread_tx;
  std::vector<std::thread> sha_check;

  test_ctx_tx.resize(sessions);
  test_ctx_rx.resize(sessions);
  tx_handle.resize(sessions);
  rx_handle.resize(sessions);
  expect_framerate.resize(sessions);
  framerate.resize(sessions);
  rtp_thread_tx.resize(sessions);
  sha_check.resize(sessions);

  for (int i = 0; i < sessions; i++) {
    bool fmd = fmd_session && fmd_session[i];
    test_ctx_tx[i] = new tests_context();
    ASSERT_TRUE(test_ctx_tx[i] != NULL);
    expect_framerate[i] = st_frame_rate(fps[i]);
    if (user_timestamp) expect_framerate[i] /= 2;

    test_ctx_tx[i]->idx = i;
    test_ctx_tx[i]->ctx = ctx;
    test_ctx_tx[i]->fb_cnt = TEST_SHA_HIST_NUM;
    test_ctx_tx[i]->fb_idx = 0;
    test_ctx_tx[i]->st40_empty_frame = empty_frame;
    memset(&ops_tx, 0, sizeof(ops_tx));
    ops_tx.name = "st40_test";
    ops_tx.priv = test_ctx_tx[i];
    ops_tx.num_port = 1;
    if (ctx->mcast_only)
      memcpy(ops_tx.dip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
             MTL_IP_ADDR_LEN);
    else
      memcpy(ops_tx.dip_addr[MTL_SESSION_PORT_P], ctx->para.sip_addr[MTL_PORT_R],
             MTL_IP_ADDR_LEN);
    snprintf(ops_tx.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
             ctx->para.port[MTL_PORT_P]);
    ops_tx.udp_port[MTL_SESSION_PORT_P] = 30000 + i * 2;
    ops_tx.type = type[i];
    ops_tx.fps = fps[i];
    ops_tx.payload_type = ST40_TEST_PAYLOAD_TYPE;
    ops_tx.interlaced = interlaced;
    ops_tx.ssrc = i ? i + 0x88888888 : 0;
    ops_tx.framebuff_cnt = test_ctx_tx[i]->fb_cnt;
    if (user_timestamp) {
      ops_tx.get_next_frame = tx_anc_next_frame_timestamp;
      ops_tx.flags |= ST40_TX_FLAG_USER_PACING;
    } else {
      ops_tx.get_next_frame = tx_anc_next_frame;
    }
    if (dedicate_tx_queue) ops_tx.flags |= ST40_TX_FLAG_DEDICATE_QUEUE;
    ops_tx.rtp_ring_size = 1024;
    ops_tx.notify_rtp_done = tx_rtp_done;
    if (fmd) {
      ops_tx.flags |= ST40_TX_FLAG_FAST_METADATA;
      ops_tx.fmd_dit = ST40_TEST_FMD_DIT;
      ops_tx.fmd_k_bit = ST40_TEST_FMD_K_BIT;
      ops_tx.payload_type = 0; /* frame level: RX filters on the lib default */
    }

    tx_handle[i] = st40_tx_create(m_handle, &ops_tx);
    ASSERT_TRUE(tx_handle[i] != NULL);

    test_ctx_tx[i]->check_sha = check_sha;
    test_ctx_tx[i]->st40_fmd = fmd;
    st40_tx_frame_init(test_ctx_tx[i], tx_handle[i], type[i],
                       fmd ? fmd_data_size[i] : 240);
    if (check_sha) {
      uint8_t* fb;
      for (int frame = 0; frame < test_ctx_tx[i]->fb_cnt; frame++) {
        fb = test_ctx_tx[i]->frame_buf[frame];
        st_test_rand_data(fb, test_ctx_tx[i]->frame_size, frame);
        /* RFC 8331 RX reads F from item[1] >> 6 and drops 0b01 */
        if (fmd && test_ctx_tx[i]->frame_size > 1) fb[1] = 0x40 | (fb[1] & 0x3f);
        unsigned char* result = test_ctx_tx[i]->shas[frame];
        SHA256((unsigned char*)fb, test_ctx_tx[i]->frame_size, result);
        test_sha_dump("st40_rx", result);
      }
    }

    test_ctx_tx[i]->handle = tx_handle[i];

    if (type[i] == ST40_TYPE_RTP_LEVEL) {
      test_ctx_tx[i]->stop = false;
      rtp_thread_tx[i] = std::thread(tx_feed_packet, test_ctx_tx[i]);
    }
  }

  for (int i = 0; i < sessions; i++) {
    bool fmd = fmd_session && fmd_session[i];
    test_ctx_rx[i] = new tests_context();
    ASSERT_TRUE(test_ctx_rx[i] != NULL);

    test_ctx_rx[i]->idx = i;
    test_ctx_rx[i]->ctx = ctx;
    test_ctx_rx[i]->fb_cnt = 3;
    test_ctx_rx[i]->fb_idx = 0;
    memset(&ops_rx, 0, sizeof(ops_rx));
    ops_rx.name = "st40_test";
    ops_rx.priv = test_ctx_rx[i];
    ops_rx.num_port = 1;
    if (ctx->mcast_only)
      memcpy(ops_rx.ip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
             MTL_IP_ADDR_LEN);
    else
      memcpy(ops_rx.ip_addr[MTL_SESSION_PORT_P], ctx->para.sip_addr[MTL_PORT_P],
             MTL_IP_ADDR_LEN);
    snprintf(ops_rx.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
             ctx->para.port[MTL_PORT_R]);
    ops_rx.udp_port[MTL_SESSION_PORT_P] = 30000 + i * 2;
    ops_rx.type = ST40_TYPE_RTP_LEVEL;
    ops_rx.notify_rtp_ready = rx_rtp_ready;
    ops_rx.rtp_ring_size = 1024;
    ops_rx.payload_type = ST40_TEST_PAYLOAD_TYPE;
    ops_rx.interlaced = interlaced;
    ops_rx.ssrc = i ? i + 0x88888888 : 0;
    if (fmd) {
      ops_rx.flags |= ST40_RX_FLAG_FAST_METADATA;
      ops_rx.payload_type = ST40_TEST_FMD_PAYLOAD_TYPE;
      test_ctx_rx[i]->frame_size = test_ctx_tx[i]->frame_size;
      test_ctx_rx[i]->frame_time = NS_PER_S / st_frame_rate(fps[i]);
    }
    rx_handle[i] = st40_rx_create(m_handle, &ops_rx);
    ASSERT_TRUE(rx_handle[i] != NULL);

    test_ctx_rx[i]->check_sha = check_sha;
    test_ctx_rx[i]->st40_fmd = fmd;
    if (check_sha) {
      test_ctx_rx[i]->pkt_data_len = test_ctx_tx[i]->pkt_data_len;
      test_ctx_rx[i]->frame_size = test_ctx_rx[i]->pkt_data_len;
      memcpy(test_ctx_rx[i]->shas, test_ctx_tx[i]->shas,
             TEST_SHA_HIST_NUM * SHA256_DIGEST_LENGTH);
      sha_check[i] = std::thread(sha_frame_check, test_ctx_rx[i]);
    }

    test_ctx_rx[i]->handle = rx_handle[i];

    struct st_queue_meta meta;
    ret = st40_rx_get_queue_meta(rx_handle[i], &meta);
    EXPECT_GE(ret, 0);
  }

  ret = mtl_start(m_handle);
  EXPECT_GE(ret, 0);
  sleep(10);

  if (fmd_trailing_gap) {
    for (int i = 0; i < sessions; i++) test_ctx_tx[i]->stop = true;
    usleep(600 * 1000);
  }

  for (int i = 0; i < sessions; i++) {
    uint64_t cur_time_ns = st_test_get_monotonic_time();
    double time_sec = (double)(cur_time_ns - test_ctx_rx[i]->start_time) / NS_PER_S;
    framerate[i] = test_ctx_rx[i]->fb_rec / time_sec;
    if (type[i] == ST40_TYPE_RTP_LEVEL) {
      test_ctx_tx[i]->stop = true;
      {
        std::unique_lock<std::mutex> lck(test_ctx_tx[i]->mtx);
        test_ctx_tx[i]->cv.notify_all();
      }
      rtp_thread_tx[i].join();
    }
    if (check_sha) {
      test_ctx_rx[i]->stop = true;
      {
        std::unique_lock<std::mutex> lck(test_ctx_rx[i]->mtx);
        test_ctx_rx[i]->cv.notify_all();
      }
      sha_check[i].join();
      while (!test_ctx_rx[i]->buf_q.empty()) {
        void* frame = test_ctx_rx[i]->buf_q.front();
        st_test_free(frame);
        test_ctx_rx[i]->buf_q.pop();
      }
    }
  }

  ret = mtl_stop(m_handle);
  EXPECT_GE(ret, 0);
  for (int i = 0; i < sessions; i++) {
    EXPECT_GT(test_ctx_rx[i]->fb_rec, 0);
    info("%s, session %d fb_rec %d framerate %f\n", __func__, i, test_ctx_rx[i]->fb_rec,
         framerate[i]);
    EXPECT_NEAR(framerate[i], expect_framerate[i], expect_framerate[i] * 0.1);
    int fail_tolerance = test_ctx_rx[i]->st40_fmd ? 0 : 2;
    EXPECT_LE(test_ctx_rx[i]->sha_fail_cnt, fail_tolerance);
    EXPECT_LE(test_ctx_rx[i]->rx_meta_fail_cnt, fail_tolerance);
    if (test_ctx_rx[i]->st40_fmd) {
      st40_fmd_expect_stats(test_ctx_rx[i], tx_handle[i], rx_handle[i], fmd_trailing_gap);
      if (fmd_trailing_gap)
        EXPECT_LT(st40_fmd_result(test_ctx_rx[i]), 0);
      else
        EXPECT_EQ(st40_fmd_result(test_ctx_rx[i]), 0);
    }
    ret = st40_tx_free(tx_handle[i]);
    EXPECT_GE(ret, 0);
    ret = st40_rx_free(rx_handle[i]);
    EXPECT_GE(ret, 0);
    if (check_sha) {
      EXPECT_GT(test_ctx_rx[i]->check_sha_frame_cnt, 0);
    }
    /* free all payload in buf_q */
    while (!test_ctx_rx[i]->buf_q.empty()) {
      void* frame = test_ctx_rx[i]->buf_q.front();
      st_test_free(frame);
      test_ctx_rx[i]->buf_q.pop();
    }
    st40_tx_frame_uinit(test_ctx_tx[i]);
    delete test_ctx_tx[i];
    delete test_ctx_rx[i];
  }
}

TEST(St40_tx, frame_fps59_94_s1) {
  enum st40_type type[1] = {ST40_TYPE_FRAME_LEVEL};
  enum st_fps fps[1] = {ST_FPS_P59_94};
  st40_tx_fps_test(type, fps, ST_TEST_LEVEL_ALL);
}
TEST(St40_tx, rtp_fps29_97_s1) {
  enum st40_type type[1] = {ST40_TYPE_RTP_LEVEL};
  enum st_fps fps[1] = {ST_FPS_P29_97};
  st40_tx_fps_test(type, fps, ST_TEST_LEVEL_ALL);
}
TEST(St40_tx, frame_fps50_s1) {
  enum st40_type type[1] = {ST40_TYPE_FRAME_LEVEL};
  enum st_fps fps[1] = {ST_FPS_P50};
  st40_tx_fps_test(type, fps, ST_TEST_LEVEL_ALL);
}
TEST(St40_tx, mix_fps59_94_s3) {
  enum st40_type type[3] = {ST40_TYPE_FRAME_LEVEL, ST40_TYPE_RTP_LEVEL,
                            ST40_TYPE_RTP_LEVEL};
  enum st_fps fps[3] = {ST_FPS_P59_94, ST_FPS_P59_94, ST_FPS_P59_94};
  st40_tx_fps_test(type, fps, ST_TEST_LEVEL_ALL, 3);
}
TEST(St40_tx, mix_fps29_97_s3) {
  enum st40_type type[3] = {ST40_TYPE_FRAME_LEVEL, ST40_TYPE_RTP_LEVEL,
                            ST40_TYPE_RTP_LEVEL};
  enum st_fps fps[3] = {ST_FPS_P29_97, ST_FPS_P29_97, ST_FPS_P29_97};
  st40_tx_fps_test(type, fps, ST_TEST_LEVEL_ALL, 3);
}
TEST(St40_tx, rtp_fps50_s3) {
  enum st40_type type[3] = {ST40_TYPE_RTP_LEVEL, ST40_TYPE_RTP_LEVEL,
                            ST40_TYPE_RTP_LEVEL};
  enum st_fps fps[3] = {ST_FPS_P50, ST_FPS_P50, ST_FPS_P50};
  st40_tx_fps_test(type, fps, ST_TEST_LEVEL_ALL, 3);
}

TEST(St40_tx, mix_fps50_fps29_97) {
  enum st40_type type[2] = {ST40_TYPE_FRAME_LEVEL, ST40_TYPE_RTP_LEVEL};
  enum st_fps fps[2] = {ST_FPS_P50, ST_FPS_P29_97};
  st40_tx_fps_test(type, fps, ST_TEST_LEVEL_ALL, 2);
}
TEST(St40_tx, mix_fps50_fps59_94) {
  enum st40_type type[2] = {ST40_TYPE_FRAME_LEVEL, ST40_TYPE_RTP_LEVEL};
  enum st_fps fps[2] = {ST_FPS_P50, ST_FPS_P59_94};
  st40_tx_fps_test(type, fps, ST_TEST_LEVEL_ALL, 2);
}
TEST(St40_tx, frame_fps29_97_fps59_94) {
  enum st40_type type[2] = {ST40_TYPE_FRAME_LEVEL, ST40_TYPE_FRAME_LEVEL};
  enum st_fps fps[2] = {ST_FPS_P29_97, ST_FPS_P59_94};
  st40_tx_fps_test(type, fps, ST_TEST_LEVEL_ALL, 2);
}
TEST(St40_rx, frame_fps29_97_fps59_94) {
  enum st40_type type[2] = {ST40_TYPE_RTP_LEVEL, ST40_TYPE_RTP_LEVEL};
  enum st_fps fps[2] = {ST_FPS_P29_97, ST_FPS_P59_94};
  st40_rx_fps_test(type, fps, ST_TEST_LEVEL_ALL, 2);
}
TEST(St40_rx, mix_s2) {
  enum st40_type type[2] = {ST40_TYPE_RTP_LEVEL, ST40_TYPE_FRAME_LEVEL};
  enum st_fps fps[2] = {ST_FPS_P50, ST_FPS_P59_94};
  st40_rx_fps_test(type, fps, ST_TEST_LEVEL_MANDATORY, 2, true, false, false, false,
                   true);
}
TEST(St40_rx, frame_fps50_fps59_94_digest) {
  enum st40_type type[2] = {ST40_TYPE_FRAME_LEVEL, ST40_TYPE_FRAME_LEVEL};
  enum st_fps fps[2] = {ST_FPS_P50, ST_FPS_P59_94};
  st40_rx_fps_test(type, fps, ST_TEST_LEVEL_ALL, 2, true);
}
TEST(St40_rx, rtp_fps50_fps59_94_digest) {
  enum st40_type type[2] = {ST40_TYPE_RTP_LEVEL, ST40_TYPE_RTP_LEVEL};
  enum st_fps fps[2] = {ST_FPS_P50, ST_FPS_P59_94};
  st40_rx_fps_test(type, fps, ST_TEST_LEVEL_ALL, 2, true);
}
TEST(St40_rx, frame_user_timestamp) {
  enum st40_type type[1] = {ST40_TYPE_FRAME_LEVEL};
  enum st_fps fps[1] = {ST_FPS_P59_94};
  st40_rx_fps_test(type, fps, ST_TEST_LEVEL_MANDATORY, 1, true, true);
}
TEST(St40_rx, frame_interlaced_empty) {
  enum st40_type type[1] = {ST40_TYPE_FRAME_LEVEL};
  enum st_fps fps[1] = {ST_FPS_P50};
  /* no sha check */
  st40_rx_fps_test(type, fps, ST_TEST_LEVEL_MANDATORY, 1, false, false, true, true);
}
TEST(St40_rx, fmd_mix_digest) {
  enum st40_type type[3] = {ST40_TYPE_FRAME_LEVEL, ST40_TYPE_FRAME_LEVEL,
                            ST40_TYPE_RTP_LEVEL};
  enum st_fps fps[3] = {ST_FPS_P59_94, ST_FPS_P59_94, ST_FPS_P50};
  /* RFC 8331 ANC next to FMD at both levels on one transmitter, 241 needs zero padding */
  bool fmd[3] = {false, true, true};
  uint32_t data_size[3] = {0, 241, 1336};
  st40_rx_fps_test(type, fps, ST_TEST_LEVEL_MANDATORY, 3, true, false, false, false,
                   false, fmd, data_size);
}
TEST(St40_rx, fmd_frame_interlaced_fps50_digest) {
  enum st40_type type[1] = {ST40_TYPE_FRAME_LEVEL};
  enum st_fps fps[1] = {ST_FPS_P50};
  /* the largest data item one packet carries */
  bool fmd[1] = {true};
  uint32_t data_size[1] = {1436};
  st40_rx_fps_test(type, fps, ST_TEST_LEVEL_ALL, 1, true, false, false, true, false, fmd,
                   data_size);
}
TEST(St40_rx, fmd_rtp_fps29_97_fps59_94_digest) {
  enum st40_type type[2] = {ST40_TYPE_RTP_LEVEL, ST40_TYPE_RTP_LEVEL};
  enum st_fps fps[2] = {ST_FPS_P29_97, ST_FPS_P59_94};
  /* the largest item is MTL_PKT_MAX_RTP_BYTES minus the 16 bytes rtp header */
  bool fmd[2] = {true, true};
  uint32_t data_size[2] = {5, 1336};
  st40_rx_fps_test(type, fps, ST_TEST_LEVEL_ALL, 2, true, false, false, false, false, fmd,
                   data_size);
}
TEST(St40_rx, fmd_trailing_gap) {
  enum st40_type type[1] = {ST40_TYPE_FRAME_LEVEL};
  enum st_fps fps[1] = {ST_FPS_P59_94};
  bool fmd[1] = {true};
  uint32_t data_size[1] = {241};

  st40_rx_fps_test(type, fps, ST_TEST_LEVEL_MANDATORY, 1, false, false, false, false,
                   false, fmd, data_size, true);
}

static int tx_fmd_frame_done(void* priv, uint16_t frame_idx,
                             struct st40_tx_frame_meta* meta) {
  auto ctx = (tests_context*)priv;
  ((std::atomic<int>*)ctx->priv)[frame_idx]++;
  return 0;
}

static void st40_fmd_oversize_test(enum st_test_level level) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto m_handle = ctx->handle;
  struct st40_tx_ops ops_tx;
  struct st40_rx_ops ops_rx;
  /* frame 0 is one byte over the largest item one packet carries, frame 1 is that */
  const size_t frame_size[2] = {1437, 1436};
  int ret;

  /* return if level small than global */
  if (level < ctx->level) return;

  if (ctx->para.num_ports < 2) {
    info(
        "%s, dual port should be enabled for tx test, one for tx and one for "
        "rx\n",
        __func__);
    throw std::runtime_error("Dual port not enabled");
  }

  auto tx = new tests_context();
  ASSERT_TRUE(tx != nullptr);
  tx->ctx = ctx;
  tx->fb_cnt = 2;
  auto frame_done = (std::atomic<int>*)st_test_zmalloc(2 * sizeof(std::atomic<int>));
  ASSERT_TRUE(frame_done != nullptr);
  tx->priv = frame_done;
  st40_fmd_tx_ops_init(tx, &ops_tx);
  ops_tx.num_port = 1;
  memcpy(
      ops_tx.dip_addr[MTL_SESSION_PORT_P],
      ctx->mcast_only ? ctx->mcast_ip_addr[MTL_PORT_P] : ctx->para.sip_addr[MTL_PORT_R],
      MTL_IP_ADDR_LEN);
  ops_tx.payload_type = 0;
  ops_tx.notify_frame_done = tx_fmd_frame_done;
  st40_tx_handle tx_handle = st40_tx_create(m_handle, &ops_tx);
  ASSERT_TRUE(tx_handle != nullptr);
  for (int i = 0; i < tx->fb_cnt; i++) {
    tx->frame_buf[i] = (uint8_t*)st_test_zmalloc(frame_size[i]);
    ASSERT_TRUE(tx->frame_buf[i] != nullptr);
    st_test_rand_data(tx->frame_buf[i], frame_size[i], i);
    struct st40_frame* dst = (struct st40_frame*)st40_tx_get_framebuffer(tx_handle, i);
    ASSERT_TRUE(dst != nullptr);
    dst->data = tx->frame_buf[i];
    dst->data_size = frame_size[i];
    dst->meta_num = 0;
  }
  tx->handle = tx_handle;

  auto rx = new tests_context();
  ASSERT_TRUE(rx != nullptr);
  rx->ctx = ctx;
  rx->check_sha = true;
  rx->st40_fmd = true;
  rx->frame_size = frame_size[1];
  rx->frame_time = NS_PER_S / st_frame_rate(ops_tx.fps);
  SHA256(tx->frame_buf[1], frame_size[1], rx->shas[0]);
  st40_fmd_rx_ops_init(rx, &ops_rx);
  ops_rx.num_port = 1;
  memcpy(
      ops_rx.ip_addr[MTL_SESSION_PORT_P],
      ctx->mcast_only ? ctx->mcast_ip_addr[MTL_PORT_P] : ctx->para.sip_addr[MTL_PORT_P],
      MTL_IP_ADDR_LEN);
  (void)snprintf(ops_rx.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
                 ctx->para.port[MTL_PORT_R]);
  st40_rx_handle rx_handle = st40_rx_create(m_handle, &ops_rx);
  ASSERT_TRUE(rx_handle != nullptr);
  rx->handle = rx_handle;
  std::thread sha_check(sha_frame_check, rx);

  ret = mtl_start(m_handle);
  EXPECT_GE(ret, 0);
  /* count only once RX has joined: 5 s at 59.94 fps, every other frame oversize */
  sleep(1);
  int sent_base = frame_done[1], received_base = rx->fb_rec;
  sleep(5);
  int sent = frame_done[1] - sent_base, received = rx->fb_rec - received_base;
  ret = mtl_stop(m_handle);
  EXPECT_GE(ret, 0);
  int aborted = frame_done[0], done = frame_done[1];

  rx->stop = true;
  {
    std::unique_lock<std::mutex> lck(rx->mtx);
    rx->cv.notify_all();
  }
  sha_check.join();
  while (!rx->buf_q.empty()) {
    st_test_free(rx->buf_q.front());
    rx->buf_q.pop();
  }

  info("%s, aborted %d done %d, %d sent %d received in 5 s\n", __func__, aborted, done,
       sent, received);
  EXPECT_GT(aborted, 0);
  EXPECT_NEAR(aborted, done, 1);
  EXPECT_GT(sent, 0);
  EXPECT_NEAR(received, sent, 2);
  EXPECT_EQ(rx->rx_meta_fail_cnt, 0);
  EXPECT_GT(rx->check_sha_frame_cnt, 0);
  EXPECT_EQ(rx->sha_fail_cnt, 0);
  st40_fmd_expect_stats(rx, tx_handle, rx_handle);

  ret = st40_rx_free(rx_handle);
  EXPECT_GE(ret, 0);
  ret = st40_tx_free(tx_handle);
  EXPECT_GE(ret, 0);
  st40_tx_frame_uinit(tx);
  st_test_free(tx->priv);
  delete rx;
  delete tx;
}

TEST(St40_tx, fmd_frame_oversize_aborted) {
  st40_fmd_oversize_test(ST_TEST_LEVEL_MANDATORY);
}

/* §5.1: at the lowest frame rate, empty frames still keep a packet every 42 ms */
TEST(St40_rx, fmd_frame_empty_keepalive_fps23_98) {
  enum st40_type type[1] = {ST40_TYPE_FRAME_LEVEL};
  enum st_fps fps[1] = {ST_FPS_P23_98};
  bool fmd[1] = {true};
  uint32_t data_size[1] = {0};
  st40_rx_fps_test(type, fps, ST_TEST_LEVEL_MANDATORY, 1, false, false, false, false,
                   false, fmd, data_size);
}

static void st40_fmd_annex_a_test(enum st_test_level level) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto m_handle = ctx->handle;
  struct st40_tx_ops ops_tx;
  struct st40_rx_ops ops_rx;
  fmd_annex_a annex_a;
  int ret;

  /* return if level small than global */
  if (level < ctx->level) return;

  if (ctx->para.num_ports < 2) {
    info(
        "%s, dual port should be enabled for tx test, one for tx and one for "
        "rx\n",
        __func__);
    throw std::runtime_error("Dual port not enabled");
  }

  fmd_annex_a_init(&annex_a);

  auto tx = new tests_context();
  ASSERT_TRUE(tx != nullptr);
  tx->ctx = ctx;
  tx->st40_fmd = true;
  tx->st40_annex_a = &annex_a;
  st40_fmd_tx_ops_init(tx, &ops_tx);
  ops_tx.type = ST40_TYPE_RTP_LEVEL;
  ops_tx.num_port = 1;
  memcpy(
      ops_tx.dip_addr[MTL_SESSION_PORT_P],
      ctx->mcast_only ? ctx->mcast_ip_addr[MTL_PORT_P] : ctx->para.sip_addr[MTL_PORT_R],
      MTL_IP_ADDR_LEN);
  ops_tx.payload_type = ST40_TEST_FMD_PAYLOAD_TYPE;
  st40_tx_handle tx_handle = st40_tx_create(m_handle, &ops_tx);
  ASSERT_TRUE(tx_handle != nullptr);
  tx->handle = tx_handle;

  auto rx = new tests_context();
  ASSERT_TRUE(rx != nullptr);
  rx->ctx = ctx;
  rx->st40_fmd = true;
  rx->st40_annex_a = &annex_a;
  rx->frame_time = NS_PER_S / st_frame_rate(ops_tx.fps);
  st40_fmd_rx_ops_init(rx, &ops_rx);
  ops_rx.num_port = 1;
  memcpy(
      ops_rx.ip_addr[MTL_SESSION_PORT_P],
      ctx->mcast_only ? ctx->mcast_ip_addr[MTL_PORT_P] : ctx->para.sip_addr[MTL_PORT_P],
      MTL_IP_ADDR_LEN);
  (void)snprintf(ops_rx.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
                 ctx->para.port[MTL_PORT_R]);
  st40_rx_handle rx_handle = st40_rx_create(m_handle, &ops_rx);
  ASSERT_TRUE(rx_handle != nullptr);
  rx->handle = rx_handle;
  std::thread feed(tx_feed_packet, tx);

  ret = mtl_start(m_handle);
  EXPECT_GE(ret, 0);
  /* 5 s at 59.94 fps is 150 cycles of 2 frames, each cycle carrying both objects */
  sleep(5);
  tx->stop = true;
  {
    std::unique_lock<std::mutex> lck(tx->mtx);
    tx->cv.notify_all();
  }
  feed.join();
  ret = mtl_stop(m_handle);
  EXPECT_GE(ret, 0);

  info("%s, objects %d %d, %d multi-item and %d zero-item pkts\n", __func__,
       annex_a.rx_done[0], annex_a.rx_done[1], annex_a.rx_multi_item_pkts,
       annex_a.rx_zero_item_pkts);
  EXPECT_EQ(rx->rx_meta_fail_cnt, 0);
  EXPECT_EQ(rx->sha_fail_cnt, 0);
  EXPECT_GT(annex_a.rx_done[0], 0);
  EXPECT_GT(annex_a.rx_done[1], 0);
  EXPECT_GT(annex_a.rx_multi_item_pkts, 0);
  EXPECT_GT(annex_a.rx_zero_item_pkts, 0);
  st40_fmd_expect_stats(rx, tx_handle, rx_handle);

  ret = st40_rx_free(rx_handle);
  EXPECT_GE(ret, 0);
  ret = st40_tx_free(tx_handle);
  EXPECT_GE(ret, 0);
  delete rx;
  delete tx;
}

/* Annex A over RTP level: multi-item, multi-DIT and zero-item packets */
TEST(St40_rx, fmd_rtp_annex_a_segments_digest) {
  st40_fmd_annex_a_test(ST_TEST_LEVEL_MANDATORY);
}

static void st40_rx_update_src_test(enum st40_type type, int tx_sessions,
                                    enum st_test_level level) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto m_handle = ctx->handle;
  int ret;
  struct st40_tx_ops ops_tx;
  struct st40_rx_ops ops_rx;

  if (ctx->para.num_ports < 2) {
    info(
        "%s, dual port should be enabled for tx test, one for tx and one for "
        "rx\n",
        __func__);
    throw std::runtime_error("Dual port not enabled");
  }
  /* return if level lower than global */
  if (level < ctx->level) return;

  ASSERT_TRUE(tx_sessions >= 1);
  bool tx_update_dst = (tx_sessions == 1);

  int rx_sessions = 1;

  std::vector<tests_context*> test_ctx_tx;
  std::vector<tests_context*> test_ctx_rx;
  std::vector<st40_tx_handle> tx_handle;
  std::vector<st40_rx_handle> rx_handle;
  std::vector<double> expect_framerate;
  std::vector<double> framerate;
  std::vector<std::thread> rtp_thread_tx;

  test_ctx_tx.resize(tx_sessions);
  test_ctx_rx.resize(rx_sessions);
  tx_handle.resize(tx_sessions);
  rx_handle.resize(rx_sessions);
  expect_framerate.resize(rx_sessions);
  framerate.resize(rx_sessions);
  rtp_thread_tx.resize(tx_sessions);

  for (int i = 0; i < rx_sessions; i++)
    expect_framerate[i] = st_frame_rate(ST_FPS_P59_94);

  for (int i = 0; i < tx_sessions; i++) {
    test_ctx_tx[i] = new tests_context();
    ASSERT_TRUE(test_ctx_tx[i] != NULL);

    test_ctx_tx[i]->idx = i;
    test_ctx_tx[i]->ctx = ctx;
    test_ctx_tx[i]->fb_cnt = 3;
    test_ctx_tx[i]->fb_idx = 0;
    memset(&ops_tx, 0, sizeof(ops_tx));
    ops_tx.name = "st40_test";
    ops_tx.priv = test_ctx_tx[i];
    ops_tx.num_port = 1;
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
    snprintf(ops_tx.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
             ctx->para.port[MTL_PORT_P]);
    ops_tx.udp_port[MTL_SESSION_PORT_P] = 30000 + i * 2;
    ops_tx.type = type;
    ops_tx.fps = ST_FPS_P59_94;
    ops_tx.payload_type = ST40_TEST_PAYLOAD_TYPE;
    ops_tx.framebuff_cnt = test_ctx_tx[i]->fb_cnt;
    ops_tx.get_next_frame = tx_anc_next_frame;
    ops_tx.notify_rtp_done = tx_rtp_done;
    ops_tx.rtp_ring_size = 1024;

    tx_handle[i] = st40_tx_create(m_handle, &ops_tx);
    ASSERT_TRUE(tx_handle[i] != NULL);

    st40_tx_frame_init(test_ctx_tx[i], tx_handle[i], type);

    test_ctx_tx[i]->handle = tx_handle[i];

    if (type == ST40_TYPE_RTP_LEVEL) {
      test_ctx_tx[i]->stop = false;
      rtp_thread_tx[i] = std::thread(tx_feed_packet, test_ctx_tx[i]);
    }
  }

  for (int i = 0; i < rx_sessions; i++) {
    test_ctx_rx[i] = new tests_context();
    ASSERT_TRUE(test_ctx_rx[i] != NULL);

    test_ctx_rx[i]->idx = i;
    test_ctx_rx[i]->ctx = ctx;
    test_ctx_rx[i]->fb_cnt = 3;
    test_ctx_rx[i]->fb_idx = 0;
    memset(&ops_rx, 0, sizeof(ops_rx));
    ops_rx.name = "st40_test";
    ops_rx.priv = test_ctx_rx[i];
    ops_rx.num_port = 1;
    if (ctx->mcast_only)
      memcpy(ops_rx.ip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_2],
             MTL_IP_ADDR_LEN);
    else
      memcpy(ops_rx.ip_addr[MTL_SESSION_PORT_P], ctx->para.sip_addr[MTL_PORT_P],
             MTL_IP_ADDR_LEN);
    snprintf(ops_rx.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
             ctx->para.port[MTL_PORT_R]);
    ops_rx.udp_port[MTL_SESSION_PORT_P] = 30000 + i * 2;
    ops_rx.type = ST40_TYPE_RTP_LEVEL;
    ops_rx.notify_rtp_ready = rx_rtp_ready;
    ops_rx.rtp_ring_size = 1024;
    ops_rx.payload_type = ST40_TEST_PAYLOAD_TYPE;

    rx_handle[i] = st40_rx_create(m_handle, &ops_rx);
    test_ctx_rx[i]->handle = rx_handle[i];
    ASSERT_TRUE(rx_handle[i] != NULL);
  }

  ret = mtl_start(m_handle);
  EXPECT_GE(ret, 0);
  sleep(10);

  struct st_rx_source_info src;
  /* switch to mcast port p(tx_session:1) */
  memset(&src, 0, sizeof(src));
  src.udp_port[MTL_SESSION_PORT_P] = 30000 + 2;
  memcpy(src.ip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
         MTL_IP_ADDR_LEN);
  if (tx_update_dst) {
    test_ctx_tx[0]->seq_id = 0; /* reset seq id */
    struct st_tx_dest_info dst;
    memset(&dst, 0, sizeof(dst));
    dst.udp_port[MTL_SESSION_PORT_P] = 30000 + 2;
    memcpy(dst.dip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
           MTL_IP_ADDR_LEN);
    ret = st40_tx_update_destination(tx_handle[0], &dst);
    EXPECT_GE(ret, 0);
  } else {
    test_ctx_tx[1]->seq_id = 0; /* reset seq id */
  }
  for (int i = 0; i < rx_sessions; i++) {
    ret = st40_rx_update_source(rx_handle[i], &src);
    EXPECT_GE(ret, 0);
    test_ctx_rx[i]->start_time = 0;
    test_ctx_rx[i]->fb_rec = 0;
  }
  sleep(10);
  /* check rx fps */
  for (int i = 0; i < rx_sessions; i++) {
    uint64_t cur_time_ns = st_test_get_monotonic_time();
    double time_sec = (double)(cur_time_ns - test_ctx_rx[i]->start_time) / NS_PER_S;
    framerate[i] = test_ctx_rx[i]->fb_rec / time_sec;

    EXPECT_GT(test_ctx_rx[i]->fb_rec, 0);
    info("%s, session %d fb_rec %d framerate %f for mcast 1\n", __func__, i,
         test_ctx_rx[i]->fb_rec, framerate[i]);
    EXPECT_NEAR(framerate[i], expect_framerate[i], expect_framerate[i] * 0.1);
  }

  if (tx_sessions > 2) {
    /* switch to mcast port r(tx_session:2) */
    memset(&src, 0, sizeof(src));
    src.udp_port[MTL_SESSION_PORT_P] = 30000 + 4;
    memcpy(src.ip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_R],
           MTL_IP_ADDR_LEN);
    for (int i = 0; i < rx_sessions; i++) {
      ret = st40_rx_update_source(rx_handle[i], &src);
      EXPECT_GE(ret, 0);
      test_ctx_tx[2]->seq_id = rand(); /* random seq id */
      test_ctx_rx[i]->start_time = 0;
      test_ctx_rx[i]->fb_rec = 0;
    }
    sleep(10);
    /* check rx fps */
    for (int i = 0; i < rx_sessions; i++) {
      uint64_t cur_time_ns = st_test_get_monotonic_time();
      double time_sec = (double)(cur_time_ns - test_ctx_rx[i]->start_time) / NS_PER_S;
      framerate[i] = test_ctx_rx[i]->fb_rec / time_sec;

      EXPECT_GT(test_ctx_rx[i]->fb_rec, 0);
      info("%s, session %d fb_rec %d framerate %f for mcast 2\n", __func__, i,
           test_ctx_rx[i]->fb_rec, framerate[i]);
      EXPECT_NEAR(framerate[i], expect_framerate[i], expect_framerate[i] * 0.1);
    }
  }

  /* switch to unicast(tx_session:0) */
  memset(&src, 0, sizeof(src));
  src.udp_port[MTL_SESSION_PORT_P] = 30000 + 0;
  memcpy(src.ip_addr[MTL_SESSION_PORT_P], ctx->para.sip_addr[MTL_PORT_P],
         MTL_IP_ADDR_LEN);
  test_ctx_tx[0]->seq_id = rand(); /* random seq id */
  if (tx_update_dst) {
    struct st_tx_dest_info dst;
    memset(&dst, 0, sizeof(dst));
    dst.udp_port[MTL_SESSION_PORT_P] = 30000 + 0;
    memcpy(dst.dip_addr[MTL_SESSION_PORT_P], ctx->para.sip_addr[MTL_PORT_R],
           MTL_IP_ADDR_LEN);
    ret = st40_tx_update_destination(tx_handle[0], &dst);
    EXPECT_GE(ret, 0);
  }
  for (int i = 0; i < rx_sessions; i++) {
    ret = st40_rx_update_source(rx_handle[i], &src);
    EXPECT_GE(ret, 0);
    test_ctx_rx[i]->start_time = 0;
    test_ctx_rx[i]->fb_rec = 0;
  }
  sleep(10);
  /* check rx fps */
  for (int i = 0; i < rx_sessions; i++) {
    uint64_t cur_time_ns = st_test_get_monotonic_time();
    double time_sec = (double)(cur_time_ns - test_ctx_rx[i]->start_time) / NS_PER_S;
    framerate[i] = test_ctx_rx[i]->fb_rec / time_sec;

    EXPECT_GT(test_ctx_rx[i]->fb_rec, 0);
    info("%s, session %d fb_rec %d framerate %f for unicast 0\n", __func__, i,
         test_ctx_rx[i]->fb_rec, framerate[i]);
    EXPECT_NEAR(framerate[i], expect_framerate[i], expect_framerate[i] * 0.1);
  }

  /* stop rtp thread */
  for (int i = 0; i < tx_sessions; i++) {
    if (type == ST40_TYPE_RTP_LEVEL) {
      test_ctx_tx[i]->stop = true;
      {
        std::unique_lock<std::mutex> lck(test_ctx_tx[i]->mtx);
        test_ctx_tx[i]->cv.notify_all();
      }
      rtp_thread_tx[i].join();
    }
  }

  ret = mtl_stop(m_handle);
  EXPECT_GE(ret, 0);

  /* free all tx and rx */
  for (int i = 0; i < rx_sessions; i++) {
    ret = st40_rx_free(rx_handle[i]);
    EXPECT_GE(ret, 0);
    delete test_ctx_rx[i];
  }
  for (int i = 0; i < tx_sessions; i++) {
    ret = st40_tx_free(tx_handle[i]);
    EXPECT_GE(ret, 0);
    st40_tx_frame_uinit(test_ctx_tx[i]);
    delete test_ctx_tx[i];
  }
}

TEST(St40_rx, update_source_rtp) {
  st40_rx_update_src_test(ST40_TYPE_RTP_LEVEL, 3, ST_TEST_LEVEL_ALL);
}
TEST(St40_tx, update_dest_rtp) {
  st40_rx_update_src_test(ST40_TYPE_RTP_LEVEL, 1, ST_TEST_LEVEL_ALL);
}

static void st40_after_start_test(enum st40_type type[], enum st_fps fps[], int sessions,
                                  int repeat) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto m_handle = ctx->handle;
  int ret;
  struct st40_tx_ops ops_tx;
  struct st40_rx_ops ops_rx;

  if (ctx->para.num_ports < 2) {
    info(
        "%s, dual port should be enabled for tx test, one for tx and one for "
        "rx\n",
        __func__);
    throw std::runtime_error("Dual port not enabled");
  }

  std::vector<tests_context*> test_ctx_tx;
  std::vector<tests_context*> test_ctx_rx;
  std::vector<st40_tx_handle> tx_handle;
  std::vector<st40_rx_handle> rx_handle;
  std::vector<double> expect_framerate;
  std::vector<double> framerate;
  std::vector<std::thread> rtp_thread_tx;

  test_ctx_tx.resize(sessions);
  test_ctx_rx.resize(sessions);
  tx_handle.resize(sessions);
  rx_handle.resize(sessions);
  expect_framerate.resize(sessions);
  framerate.resize(sessions);
  rtp_thread_tx.resize(sessions);

  ret = mtl_start(m_handle);
  EXPECT_GE(ret, 0);

  for (int r = 0; r < repeat; r++) {
    for (int i = 0; i < sessions; i++) {
      test_ctx_tx[i] = new tests_context();
      ASSERT_TRUE(test_ctx_tx[i] != NULL);
      expect_framerate[i] = st_frame_rate(fps[i]);

      test_ctx_tx[i]->idx = i;
      test_ctx_tx[i]->ctx = ctx;
      test_ctx_tx[i]->fb_cnt = 3;
      test_ctx_tx[i]->fb_idx = 0;
      memset(&ops_tx, 0, sizeof(ops_tx));
      ops_tx.name = "st40_test";
      ops_tx.priv = test_ctx_tx[i];
      ops_tx.num_port = 1;
      if (ctx->mcast_only)
        memcpy(ops_tx.dip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
               MTL_IP_ADDR_LEN);
      else
        memcpy(ops_tx.dip_addr[MTL_SESSION_PORT_P], ctx->para.sip_addr[MTL_PORT_R],
               MTL_IP_ADDR_LEN);
      snprintf(ops_tx.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
               ctx->para.port[MTL_PORT_P]);
      ops_tx.udp_port[MTL_SESSION_PORT_P] = 30000 + i * 2;
      ops_tx.type = type[i];
      ops_tx.fps = fps[i];
      ops_tx.payload_type = ST40_TEST_PAYLOAD_TYPE;
      ops_tx.framebuff_cnt = test_ctx_tx[i]->fb_cnt;
      ops_tx.get_next_frame = tx_anc_next_frame;
      ops_tx.rtp_ring_size = 1024;
      ops_tx.notify_rtp_done = tx_rtp_done;

      tx_handle[i] = st40_tx_create(m_handle, &ops_tx);
      ASSERT_TRUE(tx_handle[i] != NULL);

      st40_tx_frame_init(test_ctx_tx[i], tx_handle[i], type[i]);

      test_ctx_tx[i]->handle = tx_handle[i];

      if (type[i] == ST40_TYPE_RTP_LEVEL) {
        test_ctx_tx[i]->stop = false;
        rtp_thread_tx[i] = std::thread(tx_feed_packet, test_ctx_tx[i]);
      }
    }

    for (int i = 0; i < sessions; i++) {
      test_ctx_rx[i] = new tests_context();
      ASSERT_TRUE(test_ctx_rx[i] != NULL);

      test_ctx_rx[i]->idx = i;
      test_ctx_rx[i]->ctx = ctx;
      test_ctx_rx[i]->fb_cnt = 3;
      test_ctx_rx[i]->fb_idx = 0;
      memset(&ops_rx, 0, sizeof(ops_rx));
      ops_rx.name = "st40_test";
      ops_rx.priv = test_ctx_rx[i];
      ops_rx.num_port = 1;
      if (ctx->mcast_only)
        memcpy(ops_rx.ip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
               MTL_IP_ADDR_LEN);
      else
        memcpy(ops_rx.ip_addr[MTL_SESSION_PORT_P], ctx->para.sip_addr[MTL_PORT_P],
               MTL_IP_ADDR_LEN);
      snprintf(ops_rx.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
               ctx->para.port[MTL_PORT_R]);
      ops_rx.udp_port[MTL_SESSION_PORT_P] = 30000 + i * 2;
      ops_rx.type = ST40_TYPE_RTP_LEVEL;
      ops_rx.notify_rtp_ready = rx_rtp_ready;
      ops_rx.rtp_ring_size = 1024;
      ops_rx.payload_type = ST40_TEST_PAYLOAD_TYPE;
      rx_handle[i] = st40_rx_create(m_handle, &ops_rx);
      ASSERT_TRUE(rx_handle[i] != NULL);

      test_ctx_rx[i]->handle = rx_handle[i];
    }

    sleep(10);

    for (int i = 0; i < sessions; i++) {
      uint64_t cur_time_ns = st_test_get_monotonic_time();
      double time_sec = (double)(cur_time_ns - test_ctx_rx[i]->start_time) / NS_PER_S;
      framerate[i] = test_ctx_rx[i]->fb_rec / time_sec;
      if (type[i] == ST40_TYPE_RTP_LEVEL) {
        test_ctx_tx[i]->stop = true;
        {
          std::unique_lock<std::mutex> lck(test_ctx_tx[i]->mtx);
          test_ctx_tx[i]->cv.notify_all();
        }
        rtp_thread_tx[i].join();
      }
    }

    /* check fps */
    for (int i = 0; i < sessions; i++) {
      EXPECT_GT(test_ctx_rx[i]->fb_rec, 0);
      info("%s, session %d fb_rec %d framerate %f\n", __func__, i, test_ctx_rx[i]->fb_rec,
           framerate[i]);
      EXPECT_NEAR(framerate[i], expect_framerate[i], expect_framerate[i] * 0.1);
      ret = st40_tx_free(tx_handle[i]);
      EXPECT_GE(ret, 0);
      st40_tx_frame_uinit(test_ctx_tx[i]);
      delete test_ctx_tx[i];
      ret = st40_rx_free(rx_handle[i]);
      EXPECT_GE(ret, 0);
      delete test_ctx_rx[i];
    }
  }

  ret = mtl_stop(m_handle);
  EXPECT_GE(ret, 0);
}

TEST(St40_rx, after_start_mix_s2_r2) {
  enum st40_type type[2] = {ST40_TYPE_RTP_LEVEL, ST40_TYPE_FRAME_LEVEL};
  enum st_fps fps[2] = {ST_FPS_P50, ST_FPS_P59_94};
  st40_after_start_test(type, fps, 2, 2);
}
