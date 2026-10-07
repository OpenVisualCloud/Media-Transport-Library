/* ex09 — packet units, both ways: build your own RTP packets, and forward received
   packets byte for byte, timed by their own timestamps. Needs: MS5. */
#include <mtl/experimental/mtl_packet.h>
#include <mtl/experimental/mtl_sync.h>
#include <mtl/experimental/mtl_util.h>

#include "ex_common.h"

#define PKTS 4320 /* per frame: 1080p 4:2:2 10-bit, 1200-byte payloads */
#define CHUNK 32  /* packets per chunk, on both sides of the forwarder */
#define CHUNKS ((PKTS + CHUNK - 1) / CHUNK) /* chunks per frame */
/* the RTP header: 12 bytes, no CSRC, no extension */
#define RTP_HDR ((uint32_t)sizeof(struct mtl_rtp_hdr))
#define PT 96

/* ---- 1. Your own RTP packets ------------------------------------------------------- */

uint16_t payload(uint8_t* at, int64_t frame, uint32_t pkt); /* RFC 4175: its bytes */

/* s: a video TX session (two legs), MTL_UNIT_PACKETS, INDEX mode; set_fields
   MTL_PKT_SET_SEQ: MTL numbers the packets, the application writes the rest. */
int send_frame(mtl_session_h s, uint32_t ssrc) {
  struct mtl_unit u;
  struct mtl_tx_next nx;
  MTL_INIT(&u);
  int ret = 0;
  for (uint32_t pkt = 0; ret >= 0 && pkt < PKTS;) {
    int first_chunk = pkt == 0;
    ret = mtl_tx_acquire(s, &u, MTL_FOREVER); /* a chunk of slots; results off */
    if (ret < 0) break;
    /* after the wait: the next frame that can still go */
    if (first_chunk && (ret = mtl_tx_get_next(s, &nx, sizeof(nx))) < 0) {
      mtl_tx_release(s, u.lease);
      break;
    }
    uint32_t n = 0;
    for (; n < u.plane[0].rows && pkt < PKTS; n++, pkt++) {
      uint8_t* p = mtl_pkt_slot(&u, n); /* next_rtp: floor((M + rtp_trim) x 90 kHz) */
      mtl_rtp_set((struct mtl_rtp_hdr*)p, PT, pkt + 1 == PKTS, 0, nx.next_rtp, ssrc);
      uint16_t len = payload(p + RTP_HDR, nx.next_media_index, pkt);
      mtl_pkt_tx_table(&u)[n].len = (uint16_t)(RTP_HDR + len);
    }
    u.used = n;
    if (first_chunk) u.media_index = nx.next_media_index;
    if (pkt == PKTS) u.flags |= MTL_SUBMIT_UNIT_END;
    ret = mtl_tx_submit(s, &u); /* a late frame is DROPPED whole */
  }
  return ret < 0 ? ex_fail("send", ret) : 0;
}

/* ---- 2. A forwarder that keeps the input's timing ---------------------------------- */

#define MARGIN MTL_MS(1) /* MTL's pick-up lead and the input's jitter */
void input_gap(uint8_t leg, uint32_t seq, uint16_t gap); /* what the network lost */
void input_offset(int32_t ticks); /* first packet arrival - RTP: LIST's RTP offset */

/* How late the input arrives after its media time: a marker packet's arrival minus its
   RTP as a TAI instant. Measure it over a few units, or take its SDP's TSDELAY plus one
   frame. */
int64_t input_delay(const struct mtl_pkt_rx* marker) {
  uint32_t ts = mtl_rtp_get_ts((const struct mtl_rtp_hdr*)marker->data);
  return marker->arrival_tai_ns - mtl_media_tai(marker->arrival_tai_ns, ts, 90000);
}

/* The forwarder's TX: generic RTP at the input's rate, every byte verbatim (set_fields
   0), each unit sent min_tx_delay_ns after its first packet's RTP time. Its pool holds
   every chunk until it leaves: the frames of the delay, plus one. */
int open_forward_tx(mtl_instance_h mt, int64_t input_delay_ns, mtl_session_h* tx) {
  struct mtl_session_config sc;
  struct mtl_session_info info;
  MTL_INIT(&sc);
  sc.direction = MTL_TX;
  sc.essence = MTL_RTP;
  sc.unit = MTL_UNIT_PACKETS;
  mtl_flow_ipv4(&sc.flows[0], 239, 168, 87, 50, 20000);
  mtl_flow_ipv4(&sc.flows[1], 239, 168, 88, 50, 20000);
  snprintf(sc.rtp.encoding, sizeof(sc.rtp.encoding), "raw");
  sc.rtp.clock_rate = 90000;
  sc.rtp.unit.fps = mtl_fps_rational(MTL_FPS_59_94);
  sc.packet.packets_per_unit = PKTS;
  sc.packet.packets_per_chunk = CHUNK;
  sc.packet.unit_time = MTL_PKT_TIME_FROM_RTP;
  sc.min_tx_delay_ns = input_delay_ns + MARGIN;
  const int64_t frame_ns = mtl_frame_ns(sc.rtp.unit.fps);
  int64_t frames = (sc.min_tx_delay_ns + frame_ns - 1) / frame_ns + 1;
  int ret = mtl_session_query(mt, &sc, 0, &info, sizeof(info), NULL, 0); /* the limit */
  sc.pool_count = (uint32_t)frames * CHUNKS;
  if (ret >= 0 && sc.pool_count > info.max_count) ret = -MTL_ERANGE; /* delay too long */
  if (ret >= 0) ret = mtl_session_open(mt, &sc, tx);
  return ret < 0 ? ex_fail("forward tx", ret) : 0;
}

/* What the input did: its losses per leg, and its RTP offset. */
static void measure(const struct mtl_pkt_rx* p) {
  if (p->flags & MTL_PKTE_GAP_BEFORE) input_gap(p->leg, p->seq, p->gap);
  if ((p->flags & MTL_PKTE_UNIT_START) && (p->flags & MTL_PKTE_ARRIVAL_VALID)) {
    uint32_t ts = mtl_rtp_get_ts((const struct mtl_rtp_hdr*)p->data);
    input_offset((int32_t)(mtl_media_ticks(p->arrival_tai_ns, 90000) - ts));
  }
}

/* A lost marker leaves a TX unit open: end it short (MTL_TXR_PKT_SHORT) with an empty
   chunk before the next unit starts. */
static int end_unit(mtl_session_h tx) {
  struct mtl_unit u;
  MTL_INIT(&u);
  int ret = mtl_tx_acquire(tx, &u, 0);
  if (ret < 0) return ret;
  u.flags |= MTL_SUBMIT_UNIT_END; /* used 0 */
  return mtl_tx_submit(tx, &u);
}

/* The forwarder's TX unit between chunks. */
struct fwd_tx {
  mtl_session_h s;
  int open;        /* chunks submitted, no UNIT_END yet */
  int end_pending; /* the open unit lost its marker: end it before the next chunk */
};

/* rx: the input as video packet units, packet.packets_per_chunk = CHUNK and
   MTL_PKT_RX_UNIT_ALIGNED; the two legs' duplicates are already removed. -MTL_EAGAIN:
   the TX pool was full, and the chunk is lost. */
int forward_chunk(mtl_session_h rx, struct fwd_tx* tx) {
  struct mtl_unit in, out;
  MTL_INIT(&in);
  MTL_INIT(&out);
  int ret = mtl_rx_dequeue(rx, &in, MTL_FOREVER);
  if (ret < 0) return ret;
  const struct mtl_pkt_rx* p = mtl_pkt_rx_table(&in);
  for (uint32_t i = 0; i < in.used; i++) measure(&p[i]);
  if (tx->open && (p[0].flags & MTL_PKTE_UNIT_START)) tx->end_pending = 1;
  if (tx->end_pending && (ret = end_unit(tx->s)) == 0) /* retried on every chunk */
    tx->open = tx->end_pending = 0;
  if (ret == 0) ret = mtl_tx_acquire(tx->s, &out, 0);
  for (uint32_t i = 0; ret == 0 && i < in.used; i++) {
    memcpy(mtl_pkt_slot(&out, i), p[i].data, p[i].len);
    mtl_pkt_tx_table(&out)[i].len = p[i].len;
    if (p[i].flags & MTL_PKTE_MARKER) out.flags |= MTL_SUBMIT_UNIT_END;
  }
  if (ret == 0) {
    out.used = in.used;
    ret =
        mtl_tx_submit(tx->s, &out); /* past its deadline: DROPPED, TOO_LATE, as a frame */
    if (ret == 0) tx->open = !(out.flags & MTL_SUBMIT_UNIT_END);
  }
  int r = mtl_rx_release(rx, in.lease);
  return ret < 0 ? ret : r;
}
