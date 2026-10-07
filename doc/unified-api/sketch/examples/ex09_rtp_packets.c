/* ex09 — packet units, both ways: the application builds its RTP packets, and a forwarder
   sends received packets on byte for byte, timed by their own timestamps. Needs: MS5. */
#include <mtl/experimental/mtl_packet.h>
#include <mtl/experimental/mtl_sync.h>

#include "ex_common.h"

#define PKTS 4320 /* per frame: 1080p 4:2:2 10-bit, 1200-byte payloads */
#define CHUNK 32  /* packets per chunk, the same on both sides of the forwarder */
#define PT 96
uint16_t payload(uint8_t* at, int64_t frame, uint32_t pkt); /* RFC 4175: its bytes */
void input_gap(uint8_t leg, uint32_t seq, uint16_t gap);    /* what the network lost */
void input_offset(int32_t ticks); /* first packet arrival - RTP: LIST's RTP offset */

/* s: a video TX session (two legs), MTL_UNIT_PACKETS, INDEX mode; set_fields
   MTL_PKT_SET_SEQ: MTL numbers the packets, the application writes the rest. */
int send_frame(mtl_session_h s, uint32_t ssrc) {
  struct mtl_unit u;
  struct mtl_tx_next nx;
  MTL_INIT(&u);
  int ret = mtl_tx_get_next(s, &nx, sizeof(nx)); /* the next frame that can still go */
  for (uint32_t pkt = 0; ret >= 0 && pkt < PKTS;) {
    ret = mtl_tx_acquire(s, &u, MTL_MS(20)); /* a chunk of packet slots */
    if (ret < 0) break;
    uint32_t n = 0;
    for (; n < u.plane[0].rows && pkt < PKTS; n++, pkt++) {
      uint8_t* p = mtl_pkt_slot(&u, n); /* next_rtp: floor(M x 90 kHz), exact */
      mtl_rtp_set((struct mtl_rtp_hdr*)p, PT, pkt + 1 == PKTS, 0, nx.next_rtp, ssrc);
      mtl_pkt_tx_table(&u)[n].len =
          (uint16_t)(12 + payload(p + 12, nx.next_media_index, pkt));
    }
    u.used = n;
    if (n == pkt) u.media_index = nx.next_media_index; /* the unit's first chunk */
    if (pkt == PKTS) u.flags |= MTL_SUBMIT_UNIT_END;
    ret = mtl_tx_submit(s, &u); /* a late frame is DROPPED whole */
  }
  return ret < 0 ? ex_fail("send", ret) : 0;
}

/* The forwarder's TX: generic RTP at the input's rate, every byte verbatim (set_fields
   0), each unit's media time from its first packet's RTP (MTL_PKT_TIME_FROM_RTP), sent
   min_tx_delay_ns after it. input_delay_ns: the input's last packet minus its media time,
   measured over a few units (input_delay) or its SDP's TSDELAY plus one frame: an input
   with a launch delay of one frame arrives a frame later. A later increase: stop,
   MTL_UPDATE_MEDIA, start. */
int open_forward_tx(mtl_instance_h mt, int64_t input_delay_ns, mtl_session_h* tx) {
  struct mtl_session_config sc;
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
  sc.min_tx_delay_ns = input_delay_ns + MTL_MS(1); /* + the pick-up lead and a margin */
  int ret = mtl_session_open(mt, &sc, tx);
  return ret < 0 ? ex_fail("forward tx", ret) : 0;
}

/* rx: the input as video packet units, packet.packets_per_chunk = CHUNK and
   MTL_PKT_RX_UNIT_ALIGNED; the two legs' duplicates are already removed. */
int forward_chunk(mtl_session_h rx, mtl_session_h tx) {
  struct mtl_unit in, out;
  MTL_INIT(&in);
  MTL_INIT(&out);
  int ret = mtl_rx_dequeue(rx, &in, MTL_MS(10));
  if (ret < 0) return ret;
  const struct mtl_pkt_rx* p = mtl_pkt_rx_table(&in);
  ret = mtl_tx_acquire(tx, &out, MTL_MS(10)); /* -MTL_EAGAIN: the chunk is lost */
  for (uint32_t i = 0; ret == 0 && i < in.used; i++) {
    uint32_t ts = mtl_rtp_get_ts((const struct mtl_rtp_hdr*)p[i].data);
    if (p[i].flags & MTL_PKTE_GAP_BEFORE) input_gap(p[i].leg, p[i].seq, p[i].gap);
    if ((p[i].flags & MTL_PKTE_UNIT_START) && (p[i].flags & MTL_PKTE_ARRIVAL_VALID))
      input_offset((int32_t)(mtl_media_ticks(p[i].arrival_tai_ns, 90000) - ts));
    memcpy(mtl_pkt_slot(&out, i), p[i].data, p[i].len);
    mtl_pkt_tx_table(&out)[i].len = p[i].len;
    if (p[i].flags & MTL_PKTE_MARKER) out.flags |= MTL_SUBMIT_UNIT_END;
  }
  if (ret == 0) {
    out.used = in.used;
    ret = mtl_tx_submit(tx, &out); /* past its deadline: DROPPED, TOO_LATE, as a frame */
  }
  int r = mtl_rx_release(rx, in.lease);
  return ret < 0 ? ret : r;
}

/* The input's delay, for open_forward_tx: a marker packet's arrival minus the unit's
   media time (its RTP as a TAI instant). */
int64_t input_delay(const struct mtl_pkt_rx* marker) {
  uint32_t ts = mtl_rtp_get_ts((const struct mtl_rtp_hdr*)marker->data);
  return marker->arrival_tai_ns - mtl_media_tai(marker->arrival_tai_ns, ts, 90000);
}
