/* ex14 — ANC units (ST 2110-40): a caption inserter, an SCTE-104 relay that keeps the
   sender's RTP packet boundaries and drops damaged messages, a raw relay that changes no
   bit, timecode in both fields of an interlaced stream, and a receiver that dumps what
   arrives. A unit is the ANC packets of one frame or field: the table in plane 0, the
   words in plane 1. MTL writes the RTP packets and, outside the raw mode, every parity
   bit and checksum. DIDs and SDIDs are the SMPTE registry's. Needs: MS4a2 (the dump's
   detail: MS2). */
#include <inttypes.h>
#include <mtl/experimental/mtl_observe.h>
#include <mtl/experimental/mtl_util.h>

#include "ex_common.h"

uint8_t cdp_for_frame(int64_t frame, uint8_t cdp[255]); /* a CEA-708 CDP: its length */
void atc_vitc_words(int64_t field, uint8_t udw[16]);    /* ST 12-2 ATC_VITC of a field */

#define FRAME_NS_59_94 ((int64_t)1001 * 1000000000 / 60000) /* 16 683 333 ns */

/* A located ANC packet without a horizontal position. */
static struct mtl_anc_packet anc_at(uint8_t did, uint8_t sdid, uint16_t line, uint8_t n) {
  struct mtl_anc_packet p;
  memset(&p, 0, sizeof(p));
  p.did = did;
  p.sdid = sdid;
  p.line = line;
  p.hoffset = MTL_ANC_HOFFSET_ANY;
  p.udw_count = n;
  return p;
}

/* 1. Captions on a progressive session: one CDP per frame on line 9 (DID 61h, SDID 01h).
   A frame the producer misses still gets its empty keep-alive packet. (On an interlaced
   session the index counts fields: 2 x frame, and 2 x frame + 1 for the second field.) */
int insert_captions(mtl_session_h s, int64_t frame) {
  struct mtl_unit u;
  MTL_INIT(&u);
  int ret = mtl_tx_acquire(s, &u, MTL_MS(20));
  if (ret < 0) return ret;
  uint8_t cdp[255];
  struct mtl_anc_packet p = anc_at(0x61, 0x01, 9, cdp_for_frame(frame, cdp));
  ret = mtl_anc_put(&u, &p, cdp, sizeof(cdp), NULL);
  if (ret < 0) {
    mtl_tx_release(s, u.lease);
    return ret;
  }
  u.media_index = frame; /* MTL_MEDIA_INDEX: the RTP timestamp of video frame `frame` */
  return mtl_tx_submit(s, &u);
}

/* 2. An SCTE-104 relay (DID 41h, SDID 07h), 8-bit sessions. The TX session keeps the
   input's index (MTL_MEDIA_INDEX), so RTP equals the input RTP, and goes out one frame
   later than its own window (the live-ANC rule: an RX unit completes after its window),
   so its launch delay is one frame period plus the pick-up lead. */
int open_relay_tx(mtl_instance_h mt, mtl_session_h* s) {
  struct mtl_session_config sc;
  MTL_INIT(&sc);
  sc.direction = MTL_TX;
  sc.essence = MTL_ANC;
  mtl_flow_ipv4(&sc.flows[0], 239, 168, 85, 42, 40004);
  sc.media_mode = MTL_MEDIA_INDEX;
  sc.min_tx_delay_ns = FRAME_NS_59_94 + MTL_MS(1); /* one frame + the pick-up lead */
  sc.anc.video.width = 1920;
  sc.anc.video.height = 1080;
  sc.anc.video.fps = mtl_fps_rational(MTL_FPS_59_94);
  return mtl_session_open(mt, &sc, s);
}
/* Entries are copied unchanged: RX marked the first entry of each RTP packet
   MTL_ANCF_NEW_RTP, so the boundaries stay. A message with a gap (a packet skipped as
   corrupt, or lost) is not forwarded whole: its entries from the gap on are dropped, and
   an input unit with nothing left still gives an empty output unit. */
int relay_scte104(mtl_session_h rx, mtl_session_h tx) {
  struct mtl_unit in, out;
  MTL_INIT(&in);
  MTL_INIT(&out);
  int ret = mtl_rx_dequeue(rx, &in, MTL_MS(50));
  if (ret < 0) return ret;
  if (in.flags & MTL_UNITF_INDEX_VALID)
    ret = mtl_tx_acquire(tx, &out, MTL_MS(10));
  else
    ret = -MTL_EAGAIN; /* no index to keep: skip the unit */
  if (ret == 0) {
    const struct mtl_anc_packet* t = mtl_anc_table(&in);
    int damaged = in.status == MTL_RX_INCOMPLETE;
    for (uint32_t i = 0; i < in.used && ret == 0; i++) {
      if (t[i].flags & MTL_ANCF_GAP_BEFORE) damaged = 1;
      if (t[i].did != 0x41 || t[i].sdid != 0x07 || damaged) continue;
      ret = mtl_anc_put(&out, &t[i], mtl_anc_words(&in, &t[i]), t[i].udw_count, NULL);
    }
    out.media_index = in.media_index;
    if (ret == 0)
      ret = mtl_tx_submit(tx, &out);
    else
      mtl_tx_release(tx, out.lease);
  }
  int r = mtl_rx_release(rx, in.lease);
  return ret < 0 ? ret : r;
}

/* 3. A raw relay: both sessions MTL_ANC_WORDS_RAW. Every entry, with its DID, SDID,
   Data_Count, words and checksum as received (a wrong parity or checksum included), goes
   out bit for bit; TX accepts every entry RX delivers. The relay could also be a TX
   session attached over the RX pool, sending each slot with mtl_tx_send_slot and a hold.
 */
int relay_raw(mtl_session_h rx, mtl_session_h tx) {
  struct mtl_unit in, out;
  MTL_INIT(&in);
  MTL_INIT(&out);
  int ret = mtl_rx_dequeue(rx, &in, MTL_MS(50));
  if (ret < 0) return ret;
  ret = mtl_tx_acquire(tx, &out, MTL_MS(10));
  if (ret == 0) {
    const struct mtl_anc_packet* t = mtl_anc_table(&in);
    for (uint32_t i = 0; i < in.used && ret == 0; i++)
      ret = mtl_anc_put(&out, &t[i], mtl_anc_words(&in, &t[i]),
                        (size_t)t[i].udw_count * 2, mtl_anc_raw_hdr(&in, i));
    out.media_index = in.media_index;
    if (ret == 0)
      ret = mtl_tx_submit(tx, &out);
    else
      mtl_tx_release(tx, out.lease);
  }
  int r = mtl_rx_release(rx, in.lease);
  return ret < 0 ? ret : r;
}

/* 4. Timecode in every field of 1080i29.97 (59.94 fields/s): ATC_VITC (DID 60h, SDID 60h,
   16 words). The session is interlaced, so a unit is a field: media index 2n is the first
   field of frame n (F = 0b10, lines 1-563), 2n + 1 the second (F = 0b11, lines 564-1125);
   a line of the other field is refused unless the entry says MTL_ANCF_AS_IS. */
int open_timecode(mtl_instance_h mt, mtl_session_h* s) {
  struct mtl_session_config sc;
  MTL_INIT(&sc);
  sc.direction = MTL_TX;
  sc.essence = MTL_ANC;
  mtl_flow_ipv4(&sc.flows[0], 239, 168, 85, 41, 40002);
  sc.media_mode = MTL_MEDIA_INDEX;
  sc.anc.video.width = 1920;
  sc.anc.video.height = 1080;
  sc.anc.video.scan = MTL_INTERLACED;
  sc.anc.video.fps = mtl_fps_rational(MTL_FPS_29_97); /* frames, not fields */
  sc.anc.max_packets = 2; /* small slots: 2 entries, 510 words */
  return mtl_session_open(mt, &sc, s);
}
int send_timecode(mtl_session_h s, int64_t field) {
  struct mtl_unit u;
  MTL_INIT(&u);
  int ret = mtl_tx_acquire(s, &u, MTL_MS(20));
  if (ret < 0) return ret;
  uint8_t udw[16];
  atc_vitc_words(field, udw);
  struct mtl_anc_packet p = anc_at(0x60, 0x60, (field & 1) ? 571 : 9, 16);
  ret = mtl_anc_put(&u, &p, udw, sizeof(udw), NULL);
  if (ret < 0) {
    mtl_tx_release(s, u.lease);
    return ret;
  }
  u.media_index = field;
  return mtl_tx_submit(s, &u);
}

/* 5. A dump of one received unit (an 8-bit session): every ANC packet with its location,
   its RTP packet, its flags, and what was lost. */
int dump_anc(mtl_session_h rx) {
  struct mtl_unit u;
  MTL_INIT(&u);
  int ret = mtl_rx_dequeue(rx, &u, MTL_MS(100));
  if (ret < 0) return ret;
  const struct mtl_anc_packet* t = mtl_anc_table(&u);
  printf("index %" PRId64 " rtp %" PRIu32 " %s: %" PRIu32 " packets%s\n", u.media_index,
         u.rtp, (u.flags & MTL_UNITF_SECOND_FIELD) ? "field 2" : "field 1 or frame",
         u.used, u.status == MTL_RX_INCOMPLETE ? ", incomplete" : "");
  for (uint32_t i = 0; i < u.used; i++) {
    const uint8_t* w = (const uint8_t*)mtl_anc_words(&u, &t[i]);
    printf("  rtp %u did %02x sdid %02x line %u hoffset %u c %u stream %d words %u%s%s%s",
           t[i].rtp_index, t[i].did, t[i].sdid, t[i].line, t[i].hoffset,
           t[i].flags & MTL_ANCF_C ? 1u : 0u, t[i].flags & MTL_ANCF_S ? t[i].stream : -1,
           t[i].udw_count, t[i].flags & MTL_ANCF_NEW_RTP ? " new-rtp" : "",
           t[i].flags & MTL_ANCF_GAP_BEFORE ? " gap-before" : "",
           t[i].flags & MTL_ANCF_AS_IS ? " as-is" : "");
    if (t[i].udw_count) printf(" first %02x", w[0]);
    printf("\n");
  }
  struct mtl_rx_detail d;
  if (u.status == MTL_RX_INCOMPLETE && mtl_rx_get_detail(rx, u.lease, &d, sizeof(d)) >= 0)
    printf("  lost runs %" PRIu32 ", corrupt %" PRIu32 ", beyond capacity %" PRIu32 "\n",
           d.missing_ranges, d.anc_skipped, d.anc_truncated);
  return mtl_rx_release(rx, u.lease);
}
