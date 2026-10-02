/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */
/* A hand-formatted design sketch: formatted when it moves to include/ (milestone M0). */
/* clang-format off */
/*
 * mtl_sync.h - timelines and time arithmetic for the unified MTL API, revision 0.2.
 *
 * Every session runs on a timeline: an exact origin T0 and, per session, an index period,
 * so index k has media time M(k) = T0 + k * period and RTP = floor(M * rate). The index
 * period is one frame (one field when interlaced) for video and cvideo, one sample
 * (1 / sample_rate) for audio, the frame or field of the video an ANC or fastmeta session
 * follows, and the unit rate for generic RTP. An audio unit's media_index is the index of
 * its first sample. The default
 * timeline is the SMPTE epoch (T0 = 1970 TAI), which is enough for one session or for
 * sessions in different processes. Create a timeline when sessions of one process should
 * start together at a fresh T0 (playout from a file), or share one by name between
 * framework elements. Starting several sessions together is mtl_session_start() with an
 * array (mtl.h); this header adds the arithmetic around it.
 */

#ifndef MTL_EXPERIMENTAL_MTL_SYNC_H
#define MTL_EXPERIMENTAL_MTL_SYNC_H

#include "mtl.h"

#if defined(__cplusplus)
extern "C" {
#endif

/* ---- Timelines ---------------------------------------------------------------------- */

enum mtl_anchor {
  MTL_ANCHOR_AT_START = 0, /* T0 on the common grid at the first start, not before tai_ns */
  MTL_ANCHOR_AT_TAI = 1,   /* T0 = tai_ns rounded up to the grid */
  MTL_ANCHOR_EPOCH = 2,    /* T0 = the SMPTE epoch */
};
enum mtl_step_policy {
  MTL_STEP_DEFAULT = 0, /* EPOCH: keep media times; the others: re-anchor */
  MTL_STEP_REANCHOR = 1,
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
/* A second create with the same name and another config is -MTL_EEXIST. CP. */
MTL_API_CP int mtl_timeline_create(mtl_instance_h mt, const struct mtl_timeline_config* c,
                                   mtl_timeline_h* out);
/* Drops this reference; sessions using the timeline hold their own. CP. */
MTL_API_CP int mtl_timeline_close(mtl_timeline_h tl);

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
/* tl may be null (the epoch). DP. */
MTL_API_DP int mtl_timeline_get_info(mtl_timeline_h tl, struct mtl_timeline_info* info,
                                     size_t size);

/* ---- Index arithmetic (exact) ---------------------------------------------------------- */

/* k = floor((tai_ns - T0) / index period of session s) on its timeline: the unit that
   contains tai_ns. The first unit at or after tai_ns is k, or k + 1 when tai_ns is past
   its start (to align starts across processes). -MTL_EBUSY (WRONG_STATE) while T0 is
   unresolved. DP. */
MTL_API_DP int mtl_index_at(mtl_session_h s, int64_t tai_ns, int64_t* k);
/* The same without an instance: T0 = the epoch, period = 1 / unit_rate. The cross-process
   answer: two processes compute the same k for the same instant. AS. */
MTL_API_AS int mtl_epoch_index_at(int64_t tai_ns, struct mtl_rational unit_rate,
                                  int64_t* k);
/* RTP ticks <-> TAI at a media clock rate, exact (RTP = floor(M * rate)). AS. */
MTL_API_AS uint32_t mtl_media_ticks(int64_t tai_ns, uint32_t clock_rate);
MTL_API_AS int64_t mtl_media_tai(int64_t near_tai_ns, uint32_t ticks, uint32_t clock_rate);

/* Where the next TX unit lands: output; the size argument versions it. */
struct mtl_slot_hint {
  uint32_t queued; /* units queued ahead */
  uint32_t reserved;
  int64_t next_media_index;       /* the smallest feasible index after the last submitted */
  int64_t next_media_tai_ns;      /* its media time */
  int64_t submit_deadline_tai_ns; /* submit before this */
};
MTL_API_DP int mtl_tx_next_slot(mtl_session_h s, struct mtl_slot_hint* h, size_t size);
/* Rows units: the latest submit time of `row` for the slot of media index k. DP. */
MTL_API_DP int mtl_tx_row_deadline(mtl_session_h s, int64_t k, uint32_t row,
                                   int64_t* tai_ns);
/* Rows units, RX: a unit dequeued with MTL_UNITF_PARTIAL grows while it is held; wait until
   at least min_rows rows are complete (or the unit ends). *rows gets the rows ready; the
   option rx.rows_step sets how often a wake-up happens. -MTL_EAGAIN by the timeout. WT. */
MTL_API_WT int mtl_rx_wait_rows(mtl_session_h s, mtl_lease_h lease, uint32_t min_rows,
                                uint32_t* rows, int64_t timeout_ns);

/* RX alignment: the audio sample inside unit `audio` that is contemporaneous with video
   unit `video` (both received on timelines with valid media times). 0 = exact, 1 = within
   one sample, < 0 = not alignable. AS. */
MTL_API_AS int mtl_rx_align(const struct mtl_unit* video, const struct mtl_unit* audio,
                            uint32_t sample_rate, int64_t* sample_offset);

/* ---- Clocks ------------------------------------------------------------------------------ */

enum mtl_clock {
  MTL_CLOCK_TAI = 1,
  MTL_CLOCK_MONOTONIC = 2,
  MTL_CLOCK_REALTIME = 3,
};
/* Converts one instant between clocks through the published time base. DP. */
MTL_API_DP int mtl_time_convert(mtl_instance_h mt, int64_t in_ns, uint32_t from_clock,
                                uint32_t to_clock, int64_t* out_ns);
/* One consistent sample of the three clocks, for frameworks with their own clock. DP. */
MTL_API_DP int mtl_time_cross(mtl_instance_h mt, int64_t* MTL_NULLABLE tai_ns,
                              int64_t* MTL_NULLABLE monotonic_ns,
                              int64_t* MTL_NULLABLE realtime_ns);
/* What the instance cannot see when a node daemon disciplines its clock (ptp4l, read with
   pmc): the grandmaster and its quality, for the time.* stats, the SDP ts-refclk and
   IS-04. Applies to PHC, CLOCK_TAI and USER sources; the built-in client fills it itself.
   A changed grandmaster posts MTL_EVENT_GRANDMASTER; with time.phc_trust detect, `locked`
   is what tells MTL the node lost its grandmaster. port = 0 for every port, else port
   index + 1. CP. */
struct mtl_time_reference {
  uint32_t struct_size;
  uint8_t gmid[8];        /* EUI-64 clockIdentity of the grandmaster; zero = none */
  uint8_t domain;
  uint8_t traceable;      /* the grandmaster's timeTraceable */
  uint8_t clock_class;
  uint8_t clock_accuracy; /* IEEE 1588 enumeration; 0x22 = 250 ns */
  uint32_t locked;        /* 1 = the clock follows this grandmaster */
  uint32_t reserved0;
  uint64_t reserved[2];
};
MTL_API_CP int mtl_time_set_reference(mtl_instance_h mt, uint32_t port,
                                      const struct mtl_time_reference* ref);
/* MTL_TIME_SOURCE_USER: one (TAI, CLOCK_MONOTONIC) pair from the application. CP. */
MTL_API_CP int mtl_time_user_update(mtl_instance_h mt, int64_t tai_ns,
                                    int64_t monotonic_ns);

MTL_SIZE_CHECK(mtl_timeline_config, 136);
MTL_SIZE_CHECK(mtl_timeline_info, 56);
MTL_SIZE_CHECK(mtl_slot_hint, 32);
MTL_SIZE_CHECK(mtl_time_reference, 40);

#if defined(__cplusplus)
}
#endif

/* clang-format on */
#endif /* MTL_EXPERIMENTAL_MTL_SYNC_H */
