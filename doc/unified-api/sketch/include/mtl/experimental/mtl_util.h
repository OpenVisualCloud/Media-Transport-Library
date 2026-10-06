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
 * essences, one-call sends from named slots, plane copies, meta records, ANC units and
 * the ST 2110-40 user-data-word arithmetic. A failure a helper detects itself (a bound, a malformed
 * payload) returns its code without setting mtl_last_error(); a failure of a call it makes
 * keeps that call's. A helper works from the milestone of the latest function it calls:
 * mtl_tx_write from MS3 (mtl_tx_get_next), mtl_tx_send_slot from MS2
 * (mtl_tx_acquire_slot), the others from MS1; before that, a call to it fails to compile,
 * naming the function it calls (mtl.h, MTL_LEVEL).
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
   to both capacities). A received unit is a valid template; one from a provided
   destination carries that destination's cookie, so set how.cookie before. */
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

/* Copy path (audio, cvideo codestreams, fastmeta: one-plane units; ANC units have two
   planes and use mtl_anc_put, else -MTL_EINVAL): acquire, copy `bytes`
   into plane 0 (splitting at the unit's capacity, rows x row_bytes), and submit, the first
   unit with the template fields of `how` (NULL = AUTO), each later one at the media time
   mtl_tx_get_next() gives, so audio advances by the samples sent. Returns the bytes
   accepted, or the first call's error when none was. A partial write returns the bytes
   accepted; to continue, call again with the rest and `how` = the next index
   (next_media_index and next_media_tai_ns of mtl_tx_get_next). WT. */
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
    if (u.plane_count != 1) {
      mtl_tx_release(s, u.lease);
      ret = -MTL_EINVAL;
      break;
    }
    size_t cap = (size_t)u.plane[0].row_bytes * u.plane[0].rows;
    size_t n = bytes - done < cap ? bytes - done : cap;
    if (n) memcpy((void*)u.plane[0].addr, src + done, n);
    if (how) mtl_unit_from_template(&u, how);
    if (done) { /* a continuation: the next media time, no discontinuity */
      struct mtl_tx_next c;
      ret = mtl_tx_get_next(s, &c, sizeof(c));
      if (ret < 0) {
        mtl_tx_release(s, u.lease);
        break;
      }
      u.media_index = c.next_media_index;
      u.media_tai_ns = c.next_media_tai_ns;
      u.flags &= ~(uint64_t)MTL_SUBMIT_DISCONTINUITY;
    }
    u.used = (uint32_t)n;
    ret = mtl_tx_submit(s, &u);
    if (ret < 0) break;
    done += n;
  } while (done < bytes);
  return done ? (int)done : ret;
}

/* Acquire slot `slot` and submit it with the template fields of `how` and its `used` (the
   slot already holds the content: a slot over another session's pool, a named framework
   surface), or do nothing: -MTL_EAGAIN while the slot is still in flight. WT. */
static inline int mtl_tx_send_slot(mtl_session_h s, uint32_t slot,
                                   const struct mtl_unit* MTL_NULLABLE how,
                                   int64_t timeout_ns) {
  struct mtl_unit u;
  MTL_INIT(&u);
  int ret = mtl_tx_acquire_slot(s, slot, &u, timeout_ns);
  if (ret < 0) return ret;
  if (how) {
    mtl_unit_from_template(&u, how);
    u.used = how->used;
  }
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
   after it when one fits. 0; -MTL_EEXIST if (kind, tag) is there; -MTL_ENOSPC beyond the capacity;
   -MTL_EINVAL for kind NONE, a malformed area or a unit without one. DP. */
static inline int mtl_meta_put(struct mtl_unit* u, uint32_t kind, uint32_t tag,
                               const void* data, uint32_t n) {
  uint8_t* base = (uint8_t*)u->meta;
  uint64_t cap = u->meta_capacity, off = 0;
  uint64_t padded = ((uint64_t)n + 3) & ~(uint64_t)3;
  struct mtl_meta_hdr h;
  if (!base || kind == MTL_META_NONE || kind > 0xffff) return -MTL_EINVAL;
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
  h.count = 1;
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

/* ---- ANC units (mtl.h, contract.md §5.7) ----------------------------------------------- */

/* The packet table of an ANC unit: plane 0, u->used entries valid. */
static inline struct mtl_anc_packet* mtl_anc_table(const struct mtl_unit* u) {
  return (struct mtl_anc_packet*)u->plane[0].addr;
}
/* The user data words of entry p in plane 1: uint8_t (8-bit) or uint16_t. */
static inline void* mtl_anc_words(const struct mtl_unit* u, const struct mtl_anc_packet* p) {
  return (uint8_t*)u->plane[1].addr + (size_t)p->udw_offset * u->plane[1].stride;
}
/* RAW units: the four words DID, SDID or DBN, Data_Count, Checksum_Word of entry i
   (plane 2); NULL on a unit without plane 2. */
static inline uint16_t* mtl_anc_raw_hdr(const struct mtl_unit* u, uint32_t i) {
  return u->plane_count == 3 ? (uint16_t*)((uint8_t*)u->plane[2].addr + (size_t)i * 8) : NULL;
}
/* Appends an ANC packet to an acquired TX unit. The word mode is the unit's: 1-byte words in
   plane 1 are 8-bit, 2-byte words 10-bit, and a unit with plane 2 is RAW. Entry u->used becomes
   *p; its p->udw_count words (words_bytes bytes at `words`, at least udw_count words of the
   unit's size) are copied after the highest word any entry uses (put scans the table, so it
   never overwrites an entry written by hand: O(used) per call, so build tables of thousands of
   entries directly); on a RAW unit raw_hdr (4 words) goes to plane 2, elsewhere it must be NULL.
   u->used grows by one. 0; -MTL_ENOSPC when the table or plane 1 is full (u unchanged);
   -MTL_EINVAL for a unit that is not ANC, short words or a raw_hdr that does not match the mode.
   DPC. */
static inline int mtl_anc_put(struct mtl_unit* u, const struct mtl_anc_packet* p,
                              const void* words, size_t words_bytes, const uint16_t* raw_hdr) {
  uint32_t wb = u->plane[1].stride, raw = u->plane_count == 3;
  uint64_t n = p->udw_count, end = 0;
  if ((u->plane_count != 2 && !raw) || u->plane[0].row_bytes != sizeof(struct mtl_anc_packet) ||
      u->plane[0].stride != sizeof(struct mtl_anc_packet) || (wb != 1 && wb != 2) ||
      u->plane[1].row_bytes != wb || (raw && (wb != 2 || u->plane[2].stride != 8)) ||
      (raw != (raw_hdr != NULL)) || words_bytes < n * wb)
    return -MTL_EINVAL;
  if (u->used >= u->plane[0].rows) return -MTL_ENOSPC;
  struct mtl_anc_packet* t = mtl_anc_table(u);
  for (uint32_t i = 0; i < u->used; i++) {
    uint64_t e = (uint64_t)t[i].udw_offset + t[i].udw_count;
    if (e > end) end = e;
  }
  if (end + n > u->plane[1].rows) return -MTL_ENOSPC;
  struct mtl_anc_packet e = *p;
  e.udw_offset = (uint32_t)end;
  if (n) memcpy((uint8_t*)u->plane[1].addr + end * wb, words, (size_t)(n * wb));
  if (raw) memcpy(mtl_anc_raw_hdr(u, u->used), raw_hdr, 8);
  t[u->used++] = e;
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
  uint32_t p = value ^ (value >> 4u);
  p ^= p >> 2;
  p = (p ^ (p >> 1)) & 1;
  return (uint16_t)(value | p << 8 | (p ^ 1) << 9);
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
/* What a decode found. */
struct mtl_anc_decode_info {
  uint32_t pkts;      /* entries written */
  uint32_t udw_words; /* words written (RAW: the runs' words) */
  uint32_t skipped;   /* corrupt ANC packets skipped, every cause together */
  uint16_t truncated; /* well-formed ANC packets beyond max_pkts or the words' capacity */
  uint16_t gap_after; /* 1: a gap is still pending (packets were skipped or truncated
                         after the last entry written, or gap_in was set and no entry was
                         written): the caller's next entry carries MTL_ANCF_GAP_BEFORE */
};
/* The ANC packets of one RTP packet to table entries and their words, as the library's
   dequeue does for frame units: `data` is the RFC 8331 Length bytes after struct
   mtl_rfc8331_hdr (mtl_packet.h), anc_count its ANC_Count, word_mode the session's (0 = 8-bit).
   Decoding stops after anc_count packets, so word_align padding or trailing bytes are never read
   as packets. User data words go to `words` back to back, udw_cap words at most; each entry's
   udw_offset counts words from `words`; RAW also writes four words per entry to raw_hdr
   (max_pkts x 4). Outside RAW a corrupt packet is skipped and counted in skipped: DID, SDID or
   Data_Count parity, the checksum, DID 00h or a Type 2 SDID 00h (ST 291-1 §6.1, §6.2), a 10-bit
   word 000h-003h or 3FCh-3FFh (§9.1), an 8-bit word whose b8/b9 are not its parity, or past
   Length; a Data_Count parity error or a packet past Length also skips the rest of ANC_Count, as
   the next start is unknown. In RAW only a packet past Length is skipped, and the others are
   delivered with MTL_ANCF_PARITY_ERR or MTL_ANCF_CHECKSUM_ERR. The library's dequeue also
   counts each cause in the session's anc.pkts_skipped{cause} stats key (contract.md §11.3). A
   packet that does not fit is counted in truncated and decoding goes on. The first entry carries
   MTL_ANCF_GAP_BEFORE when gap_in is set, and an entry after skipped or truncated packets always
   does. rtp_index is 0. 0. */
static inline int mtl_anc_rfc8331_decode(const uint8_t* data, uint32_t len, uint32_t anc_count,
                                         uint32_t word_mode, int gap_in,
                                         struct mtl_anc_packet* pkts, uint32_t max_pkts,
                                         void* words, uint32_t udw_cap,
                                         uint16_t* MTL_NULLABLE raw_hdr,
                                         struct mtl_anc_decode_info* info) {
  uint32_t off = 0, gap = gap_in ? 1 : 0, raw = word_mode == MTL_ANC_WORDS_RAW;
  uint32_t ten = word_mode == MTL_ANC_WORDS_10BIT;
  memset(info, 0, sizeof(*info));
  for (uint32_t k = 0; k < anc_count; k++) {
    const uint8_t* p = data + off;
    const uint8_t* w = p + 4;
    uint32_t count = 0, bytes = 0, bad = 0, eflags = 0;
    uint16_t dc = 0;
    if (len - off >= mtl_anc_rfc8331_bytes(0)) {
      dc = mtl_anc_udw_get(w, 2);
      count = dc & 0xff;
      bytes = mtl_anc_rfc8331_bytes(count);
    }
    if (bytes == 0 || bytes > len - off || (!raw && !mtl_anc_parity_ok(dc))) {
      info->skipped += anc_count - k; /* the next packet's start is unknown */
      gap = 1;
      break;
    }
    off += bytes;
    uint16_t did = mtl_anc_udw_get(w, 0), sdid = mtl_anc_udw_get(w, 1);
    uint16_t cs = mtl_anc_udw_get(w, 3 + count);
    if (!mtl_anc_parity_ok(did) || !mtl_anc_parity_ok(sdid) || !mtl_anc_parity_ok(dc))
      bad = 1, eflags |= MTL_ANCF_PARITY_ERR;
    if (cs != mtl_anc_checksum(w, 3 + count)) {
      bad = 1, eflags |= MTL_ANCF_CHECKSUM_ERR;
    }
    if ((did & 0xff) == 0 || ((did & 0x80) == 0 && (sdid & 0xff) == 0)) bad = 1;
    for (uint32_t i = 0; i < count && !bad && !raw; i++) {
      uint16_t udw = mtl_anc_udw_get(w, 3 + i);
      if (ten ? (udw < 0x004 || udw > 0x3fb) : !mtl_anc_parity_ok(udw))
        bad = 1;
    }
    if (!raw && bad) {
      info->skipped++;
      gap = 1;
      continue;
    }
    if (info->pkts == max_pkts || count > udw_cap - info->udw_words) {
      info->truncated++;
      gap = 1;
      continue;
    }
    uint32_t idx = info->pkts++;
    struct mtl_anc_packet* a = &pkts[idx];
    uint32_t w0 = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
    a->did = (uint8_t)did;
    a->sdid = (uint8_t)sdid;
    a->udw_count = (uint8_t)count;
    a->flags = (uint8_t)((w0 >> 31 ? MTL_ANCF_C : 0) | (w0 & 0x80 ? MTL_ANCF_S : 0) |
                         (gap ? MTL_ANCF_GAP_BEFORE : 0) | (raw ? eflags : 0));
    a->line = (uint16_t)((w0 >> 20) & 0x7ff);
    a->hoffset = (uint16_t)((w0 >> 8) & 0xfff);
    a->stream = (uint8_t)(w0 & 0x7f);
    a->reserved = 0;
    a->rtp_index = 0;
    a->udw_offset = info->udw_words;
    if (raw) {
      raw_hdr[idx * 4] = did;
      raw_hdr[idx * 4 + 1] = sdid;
      raw_hdr[idx * 4 + 2] = dc;
      raw_hdr[idx * 4 + 3] = cs;
    }
    for (uint32_t i = 0; i < count; i++) {
      uint16_t udw = mtl_anc_udw_get(w, 3 + i);
      if (raw || ten)
        ((uint16_t*)words)[info->udw_words + i] = udw;
      else
        ((uint8_t*)words)[info->udw_words + i] = (uint8_t)udw;
    }
    info->udw_words += count;
    gap = 0;
  }
  info->gap_after = (uint16_t)gap;
  return 0;
}
/* The reverse, for the ANC packets of one RTP packet (n_pkts is its ANC_Count, <= 255), as the
   library's submit does for frame units: table entries, their user data words (udw_cap words at
   `words`) and, RAW, their four header words (raw_hdr, n_pkts x 4) to RFC 8331 ANC data
   packets; *len receives the bytes written. One pass: each entry and its four RAW header words
   are read once into locals and every word is checked as it is read, then written,
   so on a failure the payload holds a partial write and must not be sent. Outside RAW it writes
   DID, SDID, DC with parity, 8-bit words with parity and the checksum; in RAW the ten bits of
   every word as given. -MTL_ENOSPC if cap is short; -MTL_EINVAL for n_pkts above 255, a word
   range outside udw_cap, a line or hoffset outside its field, a stream above 127, a flag above
   MTL_ANCF_CHECKSUM_ERR or a non-zero reserved; outside RAW a DID of 00h or a Type 2 SDID of
   00h (ST 291-1 §6.1, §6.2) and, 10-bit, a protected word (000h-003h, 3FCh-3FFh, §9.1); in RAW
   a word above 3FFh, a missing raw_hdr, or header words whose low bytes are not the entry's
   did, sdid, udw_count. The raster order and the field of located packets (ST 2110-40 §5.2.2)
   depend on the format and are the caller's. */
static inline int mtl_anc_rfc8331_encode(const struct mtl_anc_packet* pkts, uint32_t n_pkts,
                                         uint32_t word_mode, const void* words,
                                         uint32_t udw_cap,
                                         const uint16_t* MTL_NULLABLE raw_hdr,
                                         uint8_t* payload, uint32_t cap, uint32_t* len) {
  uint32_t total = 0, raw = word_mode == MTL_ANC_WORDS_RAW;
  uint32_t ten = word_mode == MTL_ANC_WORDS_10BIT;
  const uint16_t* w16 = (const uint16_t*)words;
  const uint8_t* w8 = (const uint8_t*)words;
  if (n_pkts > 255 || word_mode > MTL_ANC_WORDS_RAW || (raw && !raw_hdr)) return -MTL_EINVAL;
  uint8_t* p = payload;
  for (uint32_t k = 0; k < n_pkts; k++) {
    const struct mtl_anc_packet e = pkts[k]; /* read once: what is checked is what is written */
    const struct mtl_anc_packet* a = &e;
    uint32_t count = a->udw_count, bytes = mtl_anc_rfc8331_bytes(count);
    uint16_t h[4] = {0, 0, 0, 0};
    if (raw) memcpy(h, raw_hdr + (size_t)k * 4, sizeof(h));
    if ((uint64_t)a->udw_offset + count > udw_cap || a->line > 0x7ff || a->hoffset > 0xfff ||
        a->stream > 0x7f || a->reserved || a->flags > 0x7f)
      return -MTL_EINVAL;
    if (raw ? (h[0] > 0x3ff || h[1] > 0x3ff || h[2] > 0x3ff || h[3] > 0x3ff ||
               (h[0] & 0xff) != a->did || (h[1] & 0xff) != a->sdid || (h[2] & 0xff) != count)
            : (a->did == 0 || (a->did < 0x80 && a->sdid == 0)))
      return -MTL_EINVAL;
    if (bytes > cap - total) return -MTL_ENOSPC;
    uint32_t w0 = (uint32_t)(a->flags & MTL_ANCF_C) << 31 | (uint32_t)a->line << 20 |
                  (uint32_t)a->hoffset << 8 | (a->flags & MTL_ANCF_S ? 0x80u : 0) | a->stream;
    p[0] = (uint8_t)(w0 >> 24);
    p[1] = (uint8_t)(w0 >> 16);
    p[2] = (uint8_t)(w0 >> 8);
    p[3] = (uint8_t)w0;
    uint8_t* o = p + 4;
    uint64_t acc = 0;
    uint32_t bits = 0, sum = 0;
    for (uint32_t i = 0; i < count + 4; i++) { /* 10-bit words, big endian */
      uint32_t word;
      if (i < 3)
        word = raw ? h[i] : mtl_anc_parity(i == 0 ? a->did : i == 1 ? a->sdid : (uint8_t)count);
      else if (i < count + 3) {
        if (raw || ten) {
          word = w16[a->udw_offset + i - 3];
          if (word > 0x3ff || (ten && (word < 0x004 || word > 0x3fb))) return -MTL_EINVAL;
        } else {
          word = mtl_anc_parity(w8[a->udw_offset + i - 3]);
        }
      } else {
        word = raw ? h[3] : (sum & 0x1ff) | ((((sum >> 8) & 1) ^ 1) << 9);
      }
      sum += word & 0x1ff;
      acc = acc << 10 | word;
      bits += 10;
      while (bits >= 8) {
        bits -= 8;
        *o++ = (uint8_t)(acc >> bits);
      }
    }
    if (bits) *o++ = (uint8_t)(acc << (8 - bits));
    while (o < p + bytes) *o++ = 0; /* word_align */
    p += bytes;
    total += bytes;
  }
  *len = total;
  return 0;
}

MTL_SIZE_CHECK(mtl_anc_decode_info, 16);

#if defined(__cplusplus)
}
#endif

/* clang-format on */
#endif /* MTL_EXPERIMENTAL_MTL_UTIL_H */
