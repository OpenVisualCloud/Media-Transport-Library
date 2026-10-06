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
 * essences, one-call sends from named slots, plane copies, meta records, ANC units, the
 * ST 2110-40 user-data-word arithmetic and the RX reserve of a framework source. A parser of
 * bytes from the network is never here (the RFC 8331 codec is exported, mtl_packet.h). A
 * failure a helper detects itself (a bound, a malformed record) returns its code without
 * setting mtl_last_error(); a failure of a call it makes keeps that call's. A helper works from the milestone of the latest function it calls:
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

/* ---- RX sources that lend library slots (migration.md §12.8, D-150) ------------------- */

/* The slots R a framework source keeps for MTL: R = ceil((L + c) / U) + 1, with
   L = max(latency_ns, info->latency_min_ns + info->convert_ns), latency_ns the framework's
   configured latency (GStreamer: GST_EVENT_LATENCY; 0 = none), c = copy_ns, the source's
   measured copy of one unit, and U the unit period: 1 / rate->fps, half of it when rate->scan
   is MTL_INTERLACED. rate NULL = info->raster (video, cvideo, ANC, fastmeta); a detect session
   passes the unit's mtl_rx_detail.raster, and audio a raster with fps = {sample_rate,
   unit_samples}. The source copies into a buffer of its own once pool_count - R units are lent
   out, so a slow consumer costs a copy, never a lost unit; a pool_count of R + H, H the units
   held downstream at once, never copies. Call it again at every latency or format change.
   R >= 1, or -MTL_EINVAL: an fps term 0 or above 4194303, latency_ns or copy_ns negative or
   above 1000 s, or R above 65535. AS. */
static inline int mtl_rx_reserve(const struct mtl_session_info* info,
                                 const struct mtl_raster* MTL_NULLABLE rate, int64_t latency_ns,
                                 int64_t copy_ns) {
  const struct mtl_raster* r = rate ? rate : &info->raster;
  uint64_t num = r->fps.num, den = r->fps.den;
  int64_t lib = info->latency_min_ns + info->convert_ns, l = latency_ns > lib ? latency_ns : lib;
  if (num == 0 || den == 0 || num > 4194303u || den > 4194303u || latency_ns < 0 ||
      copy_ns < 0 || l > MTL_SEC(1000) || copy_ns > MTL_SEC(1000))
    return -MTL_EINVAL;
  if (r->scan == MTL_INTERLACED) num *= 2; /* field units */
  uint64_t t = (uint64_t)(l + copy_ns) * num; /* (L + c) / U = t / q, t < 2^64 */
  uint64_t q = den * 1000000000u;
  uint64_t n = t / q + (t % q != 0) + 1;
  return n > 65535u ? -MTL_EINVAL : (int)n;
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

#if defined(__cplusplus)
}
#endif

/* clang-format on */
#endif /* MTL_EXPERIMENTAL_MTL_UTIL_H */
