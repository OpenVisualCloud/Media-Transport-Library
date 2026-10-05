/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */
/* Hand-formatted: clang-format stays off so the milestone tags keep their place
   (check.sh lint 5). */
/* clang-format off */
/*
 * mtl_util.h - helpers of the unified MTL API, revision 0.2.
 *
 * Every function here is static inline and built only on public calls of mtl.h, mtl_mem.h
 * and mtl_sync.h; an application could write it itself, and the library exports none of
 * them. They exist so that common code is written once: the copy path for byte-stream
 * essences, one-call sends from named slots, plane copies, meta records, and the ST 2110-40
 * user-data-word arithmetic. A failure a helper detects itself (a bound, a malformed
 * payload) returns its code without setting mtl_last_error(); a failure of a call it makes
 * keeps that call's. A helper is installed from the milestone of the latest function it
 * calls: mtl_tx_write from MS3 (mtl_tx_next_slot), mtl_tx_send_slot from MS2
 * (mtl_tx_acquire_slot), the others from MS1.
 */

#ifndef MTL_EXPERIMENTAL_MTL_UTIL_H
#define MTL_EXPERIMENTAL_MTL_UTIL_H

#include <string.h>

#include "mtl_mem.h"
#include "mtl_sync.h"

#if defined(__cplusplus)
extern "C" {
#endif

/* ---- Copy path and one-call sends ---------------------------------------------------- */

/* Copies the template fields of `how` into an acquired unit (mtl.h: media_index,
   media_tai_ns, cookie, hold, launch_tai_ns, the TX bits of flags, and the meta area up
   to both capacities). A received unit is a valid template. */
static inline void mtl_unit_from_template(struct mtl_unit* u, const struct mtl_unit* how) {
  u->media_index = how->media_index;
  u->media_tai_ns = how->media_tai_ns;
  u->cookie = how->cookie;
  u->hold = how->hold;
  u->launch_tai_ns = how->launch_tai_ns;
  u->flags = how->flags & MTL_SUBMIT_MASK;
  if (how->meta && u->meta)
    memcpy((void*)u->meta, (const void*)how->meta,
           how->meta_capacity < u->meta_capacity ? how->meta_capacity : u->meta_capacity);
}

/* Copy path (audio, cvideo codestreams, ANC user data, fastmeta): acquire, copy `bytes`
   into plane 0 (splitting at the unit's capacity, rows x row_bytes), and submit, the first
   unit with the template fields of `how` (NULL = AUTO), each later one at the media time
   mtl_tx_next_slot() gives, so audio advances by the samples sent. Returns the bytes
   accepted, or the first call's error when none was. A partial write returns the bytes
   accepted; to continue, call again with the rest and `how` = the next index
   (next_media_index and next_media_tai_ns of mtl_tx_next_slot). WT. */
static inline int mtl_tx_write(mtl_session_h s, const void* data, size_t bytes,
                               const struct mtl_unit* MTL_NULLABLE how, int64_t timeout_ns) {
  const uint8_t* src = (const uint8_t*)data;
  size_t done = 0;
  int ret;
  struct mtl_unit u;
  MTL_INIT(&u);
  do {
    ret = mtl_tx_acquire(s, &u, timeout_ns);
    if (ret < 0) break;
    size_t cap = (size_t)u.plane[0].row_bytes * u.plane[0].rows;
    size_t n = bytes - done < cap ? bytes - done : cap;
    if (n) memcpy((void*)u.plane[0].addr, src + done, n);
    if (how) mtl_unit_from_template(&u, how);
    if (done) { /* a continuation: the next media time, no discontinuity */
      struct mtl_slot_hint h;
      ret = mtl_tx_next_slot(s, &h, sizeof(h));
      if (ret < 0) {
        mtl_tx_release(s, u.lease);
        break;
      }
      u.media_index = h.next_media_index;
      u.media_tai_ns = h.next_media_tai_ns;
      u.flags &= ~(uint64_t)MTL_SUBMIT_DISCONTINUITY;
    }
    u.used = (uint32_t)n;
    ret = mtl_tx_submit(s, &u);
    if (ret < 0) break;
    done += n;
  } while (done < bytes);
  return done ? (int)done : ret;
}

/* Acquire slot `slot` and submit it with the template fields of `how`, or do nothing:
   -MTL_EAGAIN while the slot is still in flight. WT. */
static inline int mtl_tx_send_slot(mtl_session_h s, uint32_t slot,
                                   const struct mtl_unit* MTL_NULLABLE how,
                                   int64_t timeout_ns) {
  struct mtl_unit u;
  MTL_INIT(&u);
  int ret = mtl_tx_acquire_slot(s, slot, &u, timeout_ns);
  if (ret < 0) return ret;
  if (how) mtl_unit_from_template(&u, how);
  return mtl_tx_submit(s, &u);
}

/* Bounded copies into or out of a leased unit (bindings, Python buffers): -MTL_EINVAL for
   a plane without a CPU mapping or a range outside it. DPC. */
static inline int mtl_unit_copy_in(struct mtl_unit* u, uint32_t plane, uint64_t offset,
                                   const void* src, size_t len) {
  const struct mtl_plane* p = &u->plane[plane < MTL_MAX_PLANES ? plane : 0];
  uint64_t span = p->rows ? (uint64_t)p->stride * (p->rows - 1) + p->row_bytes : 0;
  if (plane >= u->plane_count || !p->addr || offset > span || len > span - offset)
    return -MTL_EINVAL;
  memcpy((uint8_t*)p->addr + offset, src, len);
  return 0;
}
static inline int mtl_unit_copy_out(const struct mtl_unit* u, uint32_t plane,
                                    uint64_t offset, void* dst, size_t len) {
  const struct mtl_plane* p = &u->plane[plane < MTL_MAX_PLANES ? plane : 0];
  uint64_t span = p->rows ? (uint64_t)p->stride * (p->rows - 1) + p->row_bytes : 0;
  if (plane >= u->plane_count || !p->addr || offset > span || len > span - offset)
    return -MTL_EINVAL;
  memcpy(dst, (const uint8_t*)p->addr + offset, len);
  return 0;
}
/* Copies a whole plane, row by row, from or to caller memory with its own stride (>= the
   plane's row_bytes): rows x row_bytes bytes. -MTL_EINVAL for a plane without a CPU
   mapping or a short stride. DPC. */
static inline int mtl_unit_copy_plane_in(struct mtl_unit* u, uint32_t plane, const void* src,
                                         size_t src_stride) {
  const struct mtl_plane* p = &u->plane[plane < MTL_MAX_PLANES ? plane : 0];
  if (plane >= u->plane_count || !p->addr || src_stride < p->row_bytes) return -MTL_EINVAL;
  for (uint32_t r = 0; r < p->rows; r++)
    memcpy((uint8_t*)p->addr + (size_t)r * p->stride, (const uint8_t*)src + r * src_stride,
           p->row_bytes);
  return 0;
}
static inline int mtl_unit_copy_plane_out(const struct mtl_unit* u, uint32_t plane, void* dst,
                                          size_t dst_stride) {
  const struct mtl_plane* p = &u->plane[plane < MTL_MAX_PLANES ? plane : 0];
  if (plane >= u->plane_count || !p->addr || dst_stride < p->row_bytes) return -MTL_EINVAL;
  for (uint32_t r = 0; r < p->rows; r++)
    memcpy((uint8_t*)dst + r * dst_stride, (const uint8_t*)p->addr + (size_t)r * p->stride,
           p->row_bytes);
  return 0;
}

/* ---- Meta records (frame and row units, mtl.h) ---------------------------------------- */

/* Appends the record (kind, tag) with n payload bytes at the NONE header that ends the
   records (TX acquire writes one at the start), padded to 4 bytes, and a new NONE header
   after it when one fits. MTL_META_ANC: data is n / sizeof(struct mtl_anc_packet) packet
   entries. 0; -MTL_EEXIST if (kind, tag) is there; -MTL_ENOSPC beyond the capacity;
   -MTL_EINVAL for kind NONE, a malformed area or a unit without one. DP. */
static inline int mtl_meta_put(struct mtl_unit* u, uint32_t kind, uint32_t tag,
                               const void* data, uint32_t n) {
  uint8_t* base = (uint8_t*)u->meta;
  uint64_t cap = u->meta_capacity, off = 0;
  uint64_t padded = ((uint64_t)n + 3) & ~(uint64_t)3;
  struct mtl_meta_hdr h;
  if (!base || kind == MTL_META_NONE || kind > 0xffff ||
      (kind == MTL_META_ANC && n % sizeof(struct mtl_anc_packet)))
    return -MTL_EINVAL;
  for (;; off += sizeof(h) + (((uint64_t)h.bytes + 3) & ~(uint64_t)3)) {
    if (off > cap) return -MTL_EINVAL;
    if (cap - off < sizeof(h)) return -MTL_ENOSPC; /* the capacity ends the records */
    memcpy(&h, base + off, sizeof(h));
    if (h.kind == MTL_META_NONE) break;
    if (h.kind == kind && h.tag == tag) return -MTL_EEXIST;
  }
  if (cap - off < sizeof(h) + padded) return -MTL_ENOSPC;
  h.kind = (uint16_t)kind;
  h.tag_version = 0;
  h.count = kind == MTL_META_ANC ? n / (uint32_t)sizeof(struct mtl_anc_packet) : 1;
  h.bytes = n;
  h.tag = tag;
  memcpy(base + off, &h, sizeof(h));
  if (n) memcpy(base + off + sizeof(h), data, n);
  memset(base + off + sizeof(h) + n, 0, (size_t)(padded - n));
  off += sizeof(h) + padded;
  if (cap - off >= sizeof(h)) memset(base + off, 0, sizeof(h)); /* the new NONE header */
  return 0;
}
/* The record (kind, tag): 1 with *hdr on its header (the payload follows it), 0 if there
   is none, -MTL_EINVAL for a malformed area. DP. */
static inline int mtl_meta_find(const struct mtl_unit* u, uint32_t kind, uint32_t tag,
                                const struct mtl_meta_hdr** hdr) {
  const uint8_t* base = (const uint8_t*)u->meta;
  uint64_t cap = u->meta_capacity;
  if (!base) return 0;
  for (uint64_t off = 0; cap - off >= sizeof(struct mtl_meta_hdr);) {
    const struct mtl_meta_hdr* h = (const struct mtl_meta_hdr*)(const void*)(base + off);
    if (h->kind == MTL_META_NONE) return 0;
    if (h->bytes > cap - off - sizeof(*h)) return -MTL_EINVAL;
    if (h->kind == kind && h->tag == tag) {
      *hdr = h;
      return 1;
    }
    off += sizeof(*h) + (((uint64_t)h->bytes + 3) & ~(uint64_t)3);
    if (off > cap) return 0; /* the padding of the last record reached the capacity */
  }
  return 0;
}

/* ---- ST 2110-40 helpers (RFC 8331) ----------------------------------------------------- */

/* 10-bit words packed big endian in a byte run (the RFC 8331 stream from DID on). */
static inline uint16_t mtl_anc_udw_get(const uint8_t* udw_run, uint32_t index) {
  uint32_t bit = index * 10, b = bit / 8;
  uint32_t w = (uint32_t)udw_run[b] << 8 | udw_run[b + 1];
  return (uint16_t)((w >> (6 - bit % 8)) & 0x3ff);
}
static inline void mtl_anc_udw_set(uint8_t* udw_run, uint32_t index, uint16_t word) {
  uint32_t bit = index * 10, b = bit / 8, shift = 6 - bit % 8;
  uint32_t w = (uint32_t)udw_run[b] << 8 | udw_run[b + 1];
  w = (w & ~(0x3ffu << shift)) | (uint32_t)(word & 0x3ff) << shift;
  udw_run[b] = (uint8_t)(w >> 8);
  udw_run[b + 1] = (uint8_t)w;
}
/* An 8-bit value as a 10-bit word: b8 = even parity of b0-b7, b9 = not b8. */
static inline uint16_t mtl_anc_parity(uint8_t value) {
  uint32_t ones = 0;
  for (uint32_t v = value; v; v >>= 1) ones += v & 1;
  uint32_t b8 = ones & 1;
  return (uint16_t)(value | b8 << 8 | (b8 ^ 1) << 9);
}
/* 1 if the parity bits of a 10-bit word are right. */
static inline int mtl_anc_parity_ok(uint16_t word) {
  return (word & 0x3ff) == mtl_anc_parity((uint8_t)word);
}
/* The checksum word over the first udw_count words of a run (DID, SDID, DC and the user
   data words): the 9-bit sum, b9 = not b8. */
static inline uint16_t mtl_anc_checksum(const uint8_t* udw_run, uint32_t udw_count) {
  uint32_t sum = 0;
  for (uint32_t i = 0; i < udw_count; i++) sum += mtl_anc_udw_get(udw_run, i);
  sum &= 0x1ff;
  return (uint16_t)(sum | (((sum >> 8) & 1) ^ 1) << 9);
}
/* Wire bytes of one RFC 8331 ANC data packet: the 32-bit C/line/offset/S/stream word, the
   DID, SDID, DC, user data and checksum words, padded to 32 bits. */
static inline uint32_t mtl_anc_rfc8331_bytes(uint32_t udw_count) {
  return 4 + ((((4 + udw_count) * 10 + 7) / 8 + 3) & ~3u);
}
/* mtl_anc_rfc8331_decode() and _encode() flags */
#define MTL_ANC_UDW_10BIT 0x1u /* udw_run holds uint16_t words with all 10 bits, unchanged:
                                  no parity is added or checked (ST 291-1 §6.6: parity is
                                  defined only for DID, SDID/DBN and DC). Without it, words
                                  are the 8-bit values and the 10-bit word adds b8 = even
                                  parity, b9 = not b8 */
/* What a decode found; valid after 0 and after -MTL_ENOSPC (what fitted). */
struct mtl_anc_decode_info {
  uint32_t pkts;              /* entries written to pkts */
  uint32_t pkts_skipped;      /* ANC packets dropped: DID or SDID parity, a reserved DID 00h,
                                 a checksum mismatch; a DC parity error or a packet past
                                 len hides the next packet's start, so it and every later
                                 one of anc_count are counted here */
  uint32_t udw_parity_errors; /* 8-bit mode: words whose b8/b9 were wrong, kept as their
                                 low 8 bits */
  uint32_t udw_words;         /* words written to udw_run */
};
/* RFC 8331 ANC data packets to a packet table and their user data words, for packet-unit
   ANC: `data` is the RFC 8331 Length bytes after struct mtl_rfc8331_hdr (mtl_packet.h), and
   anc_count its ANC_Count; decoding stops after anc_count packets, so word-align padding
   or trailing bytes are never read as packets. udw_run receives the words back to back
   (uint8_t, or uint16_t with MTL_ANC_UDW_10BIT), udw_cap words at most; each entry's
   udw_offset counts words. A corrupt ANC packet is skipped and counted, never the end of
   the payload. Line_Number and Horizontal_Offset read as on the wire (MTL_ANC_LINE_ANY,
   MTL_ANC_HOFFSET_ANY and the other RFC 8331 codes included), never 0 for the line. 0;
   -MTL_ENOSPC if max_pkts or udw_cap is short. */
static inline int mtl_anc_rfc8331_decode(const uint8_t* data, uint32_t len, uint32_t anc_count,
                                         uint32_t flags, struct mtl_anc_packet* pkts,
                                         uint32_t max_pkts, void* udw_run, uint32_t udw_cap,
                                         struct mtl_anc_decode_info* info) {
  uint32_t off = 0;
  info->pkts = info->pkts_skipped = info->udw_parity_errors = info->udw_words = 0;
  for (uint32_t k = 0; k < anc_count; k++) {
    const uint8_t* p = data + off;
    const uint8_t* w = p + 4;
    uint32_t count = 0, bytes = 0;
    uint16_t dc = 0;
    if (len - off >= mtl_anc_rfc8331_bytes(0)) {
      dc = mtl_anc_udw_get(w, 2);
      count = dc & 0xff;
      bytes = mtl_anc_rfc8331_bytes(count);
    }
    if (bytes == 0 || !mtl_anc_parity_ok(dc) || bytes > len - off) {
      info->pkts_skipped += anc_count - k; /* the next packet's start is unknown */
      break;
    }
    off += bytes;
    uint16_t did = mtl_anc_udw_get(w, 0), sdid = mtl_anc_udw_get(w, 1);
    if (!mtl_anc_parity_ok(did) || !mtl_anc_parity_ok(sdid) || (did & 0xff) == 0 ||
        mtl_anc_udw_get(w, 3 + count) != mtl_anc_checksum(w, 3 + count)) {
      info->pkts_skipped++;
      continue;
    }
    if (info->pkts == max_pkts || count > udw_cap - info->udw_words) return -MTL_ENOSPC;
    for (uint32_t i = 0; i < count; i++) {
      uint16_t udw = mtl_anc_udw_get(w, 3 + i);
      if (flags & MTL_ANC_UDW_10BIT) {
        ((uint16_t*)udw_run)[info->udw_words + i] = udw;
      } else {
        if (!mtl_anc_parity_ok(udw)) info->udw_parity_errors++;
        ((uint8_t*)udw_run)[info->udw_words + i] = (uint8_t)udw;
      }
    }
    struct mtl_anc_packet* a = &pkts[info->pkts++];
    uint32_t w0 = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
    a->c = (uint8_t)(w0 >> 31);
    a->line = (uint16_t)((w0 >> 20) & 0x7ff);
    if (a->line == 0) a->line = MTL_ANC_LINE_ANY; /* not an SDI line */
    a->hoffset = (uint16_t)((w0 >> 8) & 0xfff);
    a->stream = (uint8_t)w0;
    a->did = (uint16_t)(did & 0xff);
    a->sdid = (uint16_t)(sdid & 0xff);
    a->udw_count = (uint16_t)count;
    a->udw_offset = info->udw_words;
    info->udw_words += count;
  }
  return 0;
}
/* The reverse, for the ANC packets of one RTP packet (n_pkts is its ANC_Count, <= 255):
   a packet table and its user data words to RFC 8331 ANC data packets; *len receives the
   bytes written. A line of 0 is sent without a location: MTL_ANC_LINE_ANY and
   MTL_ANC_HOFFSET_ANY, its hoffset not read. -MTL_ENOSPC if cap is short; -MTL_EINVAL for
   n_pkts or a udw_count above 255, a DID of 00h or a Type 2 SDID of 00h (reserved, ST
   291-1 §6.2, Figure 4b), a DID, SDID, line or hoffset outside its field, a 10-bit word
   with a protected value (000h-003h, 3FCh-3FFh, ST 291-1 §9.1), or exact locations (line
   1-0x7FC) that
   decrease in table order (ST 2110-40 §5.2.2 wants them increasing within a frame; the
   caller keeps that order across the calls of one frame). */
static inline int mtl_anc_rfc8331_encode(const struct mtl_anc_packet* pkts, uint32_t n_pkts,
                                         uint32_t flags, const void* udw_run, uint8_t* payload,
                                         uint32_t cap, uint32_t* len) {
  uint32_t off = 0, last = 0;
  if (n_pkts > 255) return -MTL_EINVAL;
  for (uint32_t k = 0; k < n_pkts; k++) {
    const struct mtl_anc_packet* a = &pkts[k];
    uint32_t count = a->udw_count, bytes = mtl_anc_rfc8331_bytes(count);
    uint32_t line = a->line ? a->line : MTL_ANC_LINE_ANY;
    uint32_t hoffset = a->line ? a->hoffset : MTL_ANC_HOFFSET_ANY;
    if (count > 255 || a->did == 0 || a->did > 0xff || a->sdid > 0xff || line > 0x7ff ||
        hoffset > 0xfff || (a->did < 0x80 && a->sdid == 0))
      return -MTL_EINVAL;
    if (line < 0x7fd) { /* an exact line: (line, hoffset) must not go backwards */
      uint32_t key = line << 12 | hoffset;
      if (key < last) return -MTL_EINVAL;
      last = key;
    }
    if (bytes > cap - off) return -MTL_ENOSPC;
    uint8_t* p = payload + off;
    uint8_t* w = p + 4;
    memset(p, 0, bytes);
    uint32_t w0 = (uint32_t)(a->c & 1) << 31 | line << 20 | hoffset << 8 | a->stream;
    p[0] = (uint8_t)(w0 >> 24);
    p[1] = (uint8_t)(w0 >> 16);
    p[2] = (uint8_t)(w0 >> 8);
    p[3] = (uint8_t)w0;
    mtl_anc_udw_set(w, 0, mtl_anc_parity((uint8_t)a->did));
    mtl_anc_udw_set(w, 1, mtl_anc_parity((uint8_t)a->sdid));
    mtl_anc_udw_set(w, 2, mtl_anc_parity((uint8_t)count));
    for (uint32_t i = 0; i < count; i++) {
      uint16_t word;
      if (flags & MTL_ANC_UDW_10BIT) {
        word = ((const uint16_t*)udw_run)[a->udw_offset + i];
        if (word > 0x3ff || word < 0x004 || word > 0x3fb) return -MTL_EINVAL;
      } else {
        word = mtl_anc_parity(((const uint8_t*)udw_run)[a->udw_offset + i]);
      }
      mtl_anc_udw_set(w, 3 + i, word);
    }
    mtl_anc_udw_set(w, 3 + count, mtl_anc_checksum(w, 3 + count));
    off += bytes;
  }
  *len = off;
  return 0;
}

MTL_SIZE_CHECK(mtl_anc_decode_info, 16);

#if defined(__cplusplus)
}
#endif

/* clang-format on */
#endif /* MTL_EXPERIMENTAL_MTL_UTIL_H */
