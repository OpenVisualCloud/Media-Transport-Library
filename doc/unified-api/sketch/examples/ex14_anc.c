/* ex14 — ANC both ways: an ANC unit is the ANC packets of one frame or field (the table
   in plane 0, the words in plane 1), sent with its video's RTP timestamp. Here: captions,
   timecode in both fields, a monitor, and the RFC 8331 codec. Needs: MS4 (MS4a2). */
#include <inttypes.h>
#include <mtl/experimental/mtl_observe.h>
#include <mtl/experimental/mtl_packet.h>
#include <mtl/experimental/mtl_util.h>

#include "ex_common.h"

uint8_t cdp_for_frame(int64_t frame, uint8_t cdp[255]); /* a CEA-708 CDP: its length */
void atc_vitc_words(int64_t field, uint8_t udw[16]);    /* ST 12-2 ATC_VITC of a field */

/* One ANC packet on `line` (no horizontal position) as unit `index` (INDEX mode). MTL
   writes the parity, Data_Count and checksum of the 8-bit words, and the RTP packets. */
static int send_anc(mtl_session_h s, int64_t index, uint8_t did, uint8_t sdid,
                    uint16_t line, const uint8_t* words, uint8_t n) {
  struct mtl_anc_packet p;
  struct mtl_unit u;
  memset(&p, 0, sizeof(p));
  MTL_INIT(&u);
  p.did = did;
  p.sdid = sdid;
  p.line = line;
  p.hoffset = MTL_ANC_HOFFSET_ANY;
  p.udw_count = n;
  int ret = mtl_tx_acquire(s, &u, MTL_MS(20));
  if (ret < 0) return ret;
  ret = mtl_anc_put(&u, &p, words, n, NULL);
  if (ret < 0) {
    mtl_tx_release(s, u.lease);
    return ex_fail("anc", ret);
  }
  u.media_index = index;
  return mtl_tx_submit(s, &u);
}

/* Captions: a CDP per frame on line 9 (DID 61h, SDID 01h). A frame without one still
   sends the empty packet that keeps the stream alive. */
int insert_captions(mtl_session_h s, int64_t frame) {
  uint8_t cdp[255];
  uint8_t n = cdp_for_frame(frame, cdp);
  return send_anc(s, frame, 0x61, 0x01, 9, cdp, n);
}

/* Timecode in every field of 1080i29.97 (DID 60h, SDID 60h): on an interlaced session a
   unit is a field, index 2n the first (lines 1-563), 2n + 1 the second (564-1125). */
int send_timecode(mtl_session_h s, int64_t field) {
  uint8_t udw[16];
  atc_vitc_words(field, udw);
  return send_anc(s, field, 0x60, 0x60, (field & 1) ? 571 : 9, udw, 16);
}

/* A monitor: every ANC packet of a received unit, its RTP packet and what was lost. */
int dump_anc(mtl_session_h rx) {
  struct mtl_unit u;
  struct mtl_rx_detail d;
  MTL_INIT(&u);
  int ret = mtl_rx_dequeue(rx, &u, MTL_MS(100));
  if (ret < 0) return ret;
  const struct mtl_anc_packet* t = mtl_anc_table(&u);
  printf("index %" PRId64 " %s%s\n", u.media_index,
         (u.flags & MTL_UNITF_SECOND_FIELD) ? "field 2" : "field 1 or frame",
         u.status == MTL_RX_INCOMPLETE ? ", incomplete" : "");
  for (uint32_t i = 0; i < u.used; i++)
    printf("  rtp %u did %02x sdid %02x line %u words %u%s%s\n", t[i].rtp_index, t[i].did,
           t[i].sdid, t[i].line, t[i].udw_count,
           t[i].flags & MTL_ANCF_NEW_RTP ? " new-rtp" : "",
           t[i].flags & MTL_ANCF_GAP_BEFORE ? " gap-before" : "");
  if (u.status == MTL_RX_INCOMPLETE && mtl_rx_get_detail(rx, u.lease, &d, sizeof(d)) >= 0)
    printf("  corrupt %" PRIu32 ", beyond capacity %" PRIu32 "\n", d.anc_skipped,
           d.anc_truncated);
  return mtl_rx_release(rx, u.lease);
}

/* The codec, for ANC in the application's own RTP packets (packet units): an RTP packet's
   ANC data without its SCTE-104 messages (DID 41h, SDID 07h). The caller writes the new
   Length (*out_len) and ANC_Count (*count) into the RFC 8331 header. */
int strip_scte104(const uint8_t* in, uint32_t len, uint32_t* count, uint8_t* out,
                  uint32_t cap, uint32_t* out_len) {
  struct mtl_anc_packet t[32];
  uint8_t words[32 * 255];
  struct mtl_anc_decode_info info;
  uint32_t n = 0;
  int ret = mtl_anc_rfc8331_decode(in, len, *count, MTL_ANC_WORDS_8BIT, 0, t, 32, words,
                                   sizeof(words), NULL, &info);
  for (uint32_t i = 0; ret == 0 && i < info.pkts; i++)
    if (t[i].did != 0x41 || t[i].sdid != 0x07) t[n++] = t[i];
  *count = n;
  return ret < 0 ? ret
                 : mtl_anc_rfc8331_encode(t, n, MTL_ANC_WORDS_8BIT, words, info.udw_words,
                                          NULL, out, cap, out_len);
}
