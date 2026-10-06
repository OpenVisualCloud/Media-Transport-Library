/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */
/* Hand-formatted: clang-format stays off so the milestone tags keep their place
   (check.sh lint 5). */
/* clang-format off */
/*
 * mtl_sync.h - time arithmetic for the unified MTL API, revision 0.2.
 *
 * Every session runs on the SMPTE epoch: an exact origin T0 and, per session, an index
 * period, so index k has media time M(k) = T0 + k * period and RTP = floor(M * rate). T0
 * is 1970 TAI, or the start's T0 for sessions started with MTL_WHEN_ORIGIN (mtl.h). The
 * index period is one frame (one field when interlaced) for video and cvideo, one sample
 * (1 / sample_rate) for audio, the frame or field of the video an ANC or fastmeta session
 * follows, and the unit rate for generic RTP. An audio unit's media_index is the index of
 * its first sample. Two processes compute the same epoch index for the same instant, so
 * sessions in different processes align without sharing anything. A step of the time base
 * keeps every media time: T0 never moves (MTL_EVENT_TIME_STEP reports the step). Starting several
 * sessions together is mtl_session_start() with an array (mtl.h); this header adds the
 * arithmetic around it.
 */

#ifndef MTL_EXPERIMENTAL_MTL_SYNC_H
#define MTL_EXPERIMENTAL_MTL_SYNC_H

#include "mtl.h"

#if defined(__cplusplus)
extern "C" {
#endif

#if defined(MTL_LATER)
/* ---- Created timelines (later) ---------------------------------------------------------- */

enum mtl_anchor {
  MTL_ANCHOR_AT_START = 0, /* T0 on the common grid at the first start, not before tai_ns */
  MTL_ANCHOR_AT_TAI = 1,   /* T0 = tai_ns rounded up to the grid: at create with an explicit
                              grid, else at the first start; never re-anchored */
  MTL_ANCHOR_EPOCH = 2,    /* T0 = the SMPTE epoch */
};
enum mtl_step_policy {
  MTL_STEP_DEFAULT = 0, /* EPOCH and AT_TAI: keep media times; AT_START: re-anchor */
  MTL_STEP_REANCHOR = 1, /* AT_START only: -MTL_EINVAL with EPOCH or AT_TAI */
  MTL_STEP_KEEP = 2,
};
struct mtl_timeline_config {
  uint32_t struct_size;
  uint32_t anchor;      /* enum mtl_anchor */
  uint32_t step_policy; /* enum mtl_step_policy: what a time-base step does */
  uint32_t reserved0;
  int64_t tai_ns;           /* AT_TAI: the anchor; AT_START: a lower bound */
  struct mtl_rational grid; /* {0, 0} = the common grid of the sessions' unit periods */
  char name[MTL_NAME_MAX];  /* "" = private; else open-or-create by name, refcounted */
  uint64_t reserved[4];
};
/* A second create with the same name and another config is -MTL_EEXIST. CP. (later) */
MTL_API_CP(LATER) int mtl_timeline_create(mtl_instance_h mt, const struct mtl_timeline_config* c,
                                          mtl_timeline_h* out);
/* Drops this reference; sessions using the timeline hold their own. CP. */
static inline int mtl_timeline_close(mtl_timeline_h tl) {
  return mtl_close(mtl_obj(MTL_OBJ_TIMELINE, 0, tl.id), 0);
}

#define MTL_TIMELINE_RESOLVED 0x1u /* T0 is known */
/* Output; the size argument versions it. */
struct mtl_timeline_info {
  uint32_t flags; /* MTL_TIMELINE_* */
  uint32_t sessions;
  int64_t t0_tai_ns;
  struct mtl_rational t0_s; /* exact T0 in seconds */
  struct mtl_rational grid;
  int64_t index_offset; /* whole-unit trim kept by the timeline */
};
/* tl may be null (the epoch). DP. (later) */
MTL_API_DP(LATER) int mtl_timeline_get_info(mtl_timeline_h tl, struct mtl_timeline_info* info,
                                            size_t size);
/* k = floor((tai_ns - T0) / index period of session s) on its timeline. DP. (later) */
MTL_API_DP(LATER) int mtl_index_at(mtl_session_h s, int64_t tai_ns, int64_t* k);

MTL_SIZE_CHECK(mtl_timeline_config, 136);
MTL_SIZE_CHECK(mtl_timeline_info, 56);
#endif

/* ---- Index arithmetic (exact) ---------------------------------------------------------- */

/* k = floor((tai_ns - 1970 TAI) / period), period = 1 / unit_rate: the epoch unit that
   contains tai_ns. The first unit at or after tai_ns is k, or k + 1 when tai_ns is past its
   start (to align starts across processes). unit_rate is the index rate (fields per second
   for interlaced video, the sample rate for audio): -MTL_EINVAL unless, reduced, num x den
   <= 2^33 and num <= 2^29 x den (2^29 units per second), both checked by division so that
   no product wraps; tai_ns < 0 is -MTL_ERANGE. A unit's
   own media_tai_ns is floored to ns, so its containing unit can be the one before: use the
   "at or after" index for it, or its media_index. No instance needed. AS. (MS3) */
MTL_API_AS(3) int mtl_epoch_index_at(int64_t tai_ns, struct mtl_rational unit_rate,
                                     int64_t* k);
/* TAI to RTP ticks at a media clock rate, exact (RTP = floor(M * rate)). AS. (MS2) */
MTL_API_AS(2) uint32_t mtl_media_ticks(int64_t tai_ns, uint32_t clock_rate);
/* RTP ticks to the TAI instant nearest near_tai_ns that has them (unwraps). AS. (MS2) */
MTL_API_AS(2) int64_t mtl_media_tai(int64_t near_tai_ns, uint32_t ticks, uint32_t clock_rate);

/* Where the next TX unit lands and by when to submit it. Output; the size argument versions
   it. */
struct mtl_tx_next {
  uint32_t queued; /* units queued ahead */
  uint32_t reserved;
  int64_t next_media_index;       /* the smallest feasible index at or after the end of
                                     the last submitted unit (audio: its first sample +
                                     its samples) */
  int64_t next_media_tai_ns;      /* its media time */
  int64_t submit_deadline_tai_ns; /* submit before this */
};
/* DP. (MS3) */
MTL_API_DP(3) int mtl_tx_get_next(mtl_session_h s, struct mtl_tx_next* n, size_t size);
/* Rows units: the latest submit time of `row` for media index k; row 0 is the
   unit's deadline. DP. (MS2) */
MTL_API_DP(2) int mtl_tx_row_deadline(mtl_session_h s, int64_t k, uint32_t row,
                                      int64_t* tai_ns);
/* Rows units, RX: a unit dequeued with MTL_UNITF_PARTIAL grows while it is held; wait until
   at least min_rows rows are complete (or the unit ends). *rows gets the rows ready; the
   option rx.rows_step sets how often a wake-up happens. -MTL_EAGAIN by the timeout. WT: it
   sleeps on the session like the other waits (mtl.h), is interrupted with
   MTL_WAIT_DEQUEUE, and never arms the wait handle. (MS2) */
MTL_API_WT(2) int mtl_rx_wait_rows(mtl_session_h s, mtl_lease_h lease, uint32_t min_rows,
                                   uint32_t* rows, int64_t timeout_ns);

/* RX alignment: the audio sample inside unit `audio` that starts with video unit
   `video`, from their media indices (both MTL_UNITF_INDEX_VALID, on one epoch) and the
   exact rates: `raster` is the video session's (fps the frame rate; the index counts fields
   when scan is MTL_INTERLACED), sample_rate the audio's. *sample_offset = the audio sample
   index of the video unit's start, rounded down, minus the audio unit's first sample. 0 =
   exact, 1 = rounded down (the video unit starts inside a sample); -MTL_EINVAL: an index
   not valid, an fps outside the struct mtl_rational limits, or a sample rate of 0 or above
   2^29; -MTL_ERANGE: the video unit starts before the audio unit. The caller checks the
   offset against the samples the audio unit holds. */
static inline int mtl_rx_align(const struct mtl_unit* video, const struct mtl_raster* raster,
                               const struct mtl_unit* audio, uint32_t sample_rate,
                               int64_t* sample_offset) {
  uint64_t num = raster->fps.num, den = raster->fps.den;
  if (!(video->flags & MTL_UNITF_INDEX_VALID) || !(audio->flags & MTL_UNITF_INDEX_VALID) ||
      video->media_index < 0 || num == 0 || den == 0 || num > 4194303u || den > 1023u ||
      sample_rate == 0 || sample_rate > (1u << 29))
    return -MTL_EINVAL;
  if (raster->scan == MTL_INTERLACED) num *= 2; /* field indices */
  /* sample = floor(k x den x sample_rate / num), split: (k % num) x den x rate < 2^63 */
  uint64_t k = (uint64_t)video->media_index;
  uint64_t step = den * sample_rate;
  uint64_t rest = (k % num) * step;
  int64_t sample = (int64_t)((k / num) * step + rest / num);
  if (sample < audio->media_index) return -MTL_ERANGE;
  *sample_offset = sample - audio->media_index;
  return rest % num == 0 ? 0 : 1;
}

/* The forward offset in [0, one frame) from anchor_tai_ns to the next frame instant k /
   frame_rate on the epoch (k integer; the instant floored to ns). The TAI-mode sessions of a
   programme add the same offset, computed once, to their media times, so units at anchor +
   j frames (times exact to the ns) snap with an error of at most 1 ns and audio and ANC keep
   the video's phase (timing.md §10.5).
   frame_rate is raster.fps, frames also when interlaced, so first fields land on even
   indices. 0, or -MTL_EINVAL: a rate outside the struct mtl_rational limits, or
   anchor_tai_ns < 0. AS. */
static inline int mtl_grid_offset(int64_t anchor_tai_ns, struct mtl_rational frame_rate,
                                  int64_t* offset_ns) {
  uint64_t n = frame_rate.num, d = frame_rate.den;
  if (anchor_tai_ns < 0 || n == 0 || d == 0 || n > 4194303u || d > 1023u) return -MTL_EINVAL;
  uint64_t q = d * 1000000000u;            /* n frames take exactly d seconds: the cycle */
  uint64_t r = (uint64_t)anchor_tai_ns % q; /* the anchor's place in its cycle */
  uint64_t p = r * n;                      /* < 2^62 */
  uint64_t c = p / q + (p % q != 0);       /* the first frame of the cycle at or after it */
  *offset_ns = (int64_t)(c * q / n - r);   /* c x q <= n x q < 2^62 */
  return 0;
}

/* ---- Clocks ------------------------------------------------------------------------------ */

enum mtl_clock {
  MTL_CLOCK_TAI = 1,
  MTL_CLOCK_MONOTONIC = 2,
  MTL_CLOCK_REALTIME = 3,
};
/* Converts one instant between clocks over one mtl_time_now() sample of the three (mtl.h):
   the MTL_TIMEF_* flags of that sample (>= 0), or < 0. DP. */
static inline int mtl_time_convert(mtl_instance_h mt, int64_t in_ns, uint32_t from_clock,
                                   uint32_t to_clock, int64_t* out_ns) {
  int64_t t[4] = {0, 0, 0, 0}; /* indexed by enum mtl_clock */
  if (from_clock < MTL_CLOCK_TAI || from_clock > MTL_CLOCK_REALTIME ||
      to_clock < MTL_CLOCK_TAI || to_clock > MTL_CLOCK_REALTIME)
    return -MTL_EINVAL;
  int ret = mtl_time_now(mt, &t[MTL_CLOCK_TAI], &t[MTL_CLOCK_MONOTONIC],
                         &t[MTL_CLOCK_REALTIME]);
  if (ret >= 0) *out_ns = in_ns - t[from_clock] + t[to_clock];
  return ret;
}

#define MTL_TIMEREF_USER_PAIR 0x1u /* user_tai_ns and user_monotonic_ns are valid */
/* What the instance cannot see when a node daemon disciplines its clock (ptp4l, read with
   pmc): the grandmaster and its quality, for the time.* stats, the SDP ts-refclk and
   IS-04. A clock_class of 220 or 228, or a time_source of 0xF0, is an ARB timescale (ST
   2059-2 §5.5.4): mtl_time_now() then flags MTL_TIMEF_ARB_TIMESCALE (mtl.h). */
struct mtl_time_reference {
  uint32_t struct_size;
  uint8_t gmid[8];        /* EUI-64 clockIdentity of the grandmaster; zero = none */
  uint8_t domain;
  uint8_t traceable;      /* the grandmaster's timeTraceable */
  uint8_t clock_class;
  uint8_t clock_accuracy; /* IEEE 1588 enumeration; 0x22 = 250 ns */
  uint32_t locked;        /* 1 = the clock follows this grandmaster */
  uint32_t flags;         /* MTL_TIMEREF_* */
  int64_t user_tai_ns;
  int64_t user_monotonic_ns;
  uint8_t time_source;    /* the grandmaster's timeSource (IEEE 1588, ST 2059-2 F0h, F1h);
                             0 = unknown */
  uint8_t locking_status; /* the SM TLV masterLockingStatus (ST 2059-2 §5.13): 0 not in
                             use or unknown, 1 free run, 2 cold locking (a time step can
                             come), 3 warm locking, 4 locked */
  uint8_t reserved0[6];
  uint64_t reserved[1];
};
/* Sets the reference. Applies to PHC, CLOCK_TAI and USER sources; the built-in client
   fills it itself. A changed grandmaster posts MTL_EVENT_GRANDMASTER; with time.phc_trust
   detect, `locked` is what tells MTL the node lost its grandmaster. port = 0 for every
   port, else MTL_INDEX(port). Every call sets the whole reference. With
   MTL_TIMEREF_USER_PAIR the call also feeds MTL_TIME_SOURCE_USER one (TAI,
   CLOCK_MONOTONIC) pair (port 0; another source is -MTL_EINVAL naming user_tai_ns). CP.
   (MS2) */
MTL_API_CP(2) int mtl_time_set_reference(mtl_instance_h mt, uint32_t port,
                                         const struct mtl_time_reference* ref);

MTL_SIZE_CHECK(mtl_tx_next, 32);
MTL_SIZE_CHECK(mtl_time_reference, 56);

#if defined(__cplusplus)
}
#endif

/* clang-format on */
#endif /* MTL_EXPERIMENTAL_MTL_SYNC_H */
