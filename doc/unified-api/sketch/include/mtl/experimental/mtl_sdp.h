/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */
/* A hand-formatted design sketch: formatted when it moves to include/ (milestone M0). */
/* clang-format off */
/*
 * mtl_sdp.h - SDP for ST 2110, ST 2022-6 and IPMX sessions (RFC 4566, 4570, 7104, 7273),
 * revision 0.2.
 *
 * Built only on public calls, like mtl_util.h, and allocates nothing: MTL itself never
 * needs SDP. Render writes what a created session is now: granted values, every existing
 * leg (a muted session renders its legs as configured) (ST 2022-7 as two m-lines under a=group:DUP, ST 2110-10 §8.5), the time reference,
 * colorimetry (always, as ST 2110-20 requires), the IPMX keyword and TP under the IPMX
 * profile, a=rtcp, and the a=privacy and a=extmap lines of the crypto.* options. An NMOS or
 * IPMX sender re-renders on every activation and on MTL_EVENT_GRANDMASTER or
 * MTL_EVENT_RTCP_INFO, and the o= version then changes. Parse fills a session
 * configuration for a receiver. NMOS JSON stays outside MTL.
 */

#ifndef MTL_EXPERIMENTAL_MTL_SDP_H
#define MTL_EXPERIMENTAL_MTL_SDP_H

#include "mtl.h"

#if defined(__cplusplus)
extern "C" {
#endif

#if defined(MTL_LATER) /* a later phase (implementation-plan.md §6); the design is kept */
#define MTL_SDP_NO_SOURCE_FILTER 0x1u /* render: omit a=source-filter (not under IPMX) */
#define MTL_SDP_IPMX 0x2u             /* parse output: the stream declares IPMX */
/* What MTL does not know; all optional ("" and 0 = the default). Render reads it; parse
   fills it, every string copied and NUL-terminated (-MTL_ENOSPC naming a field that does
   not fit). */
struct mtl_sdp_meta {
  uint32_t struct_size;
  uint32_t sdp_flags;       /* MTL_SDP_* */
  uint64_t session_id;      /* o=; 0 = from the session's creation time */
  uint64_t session_version; /* o=; 0 = a counter of activations and clock or Info Block
                               changes */
  char session_name[64];      /* s=; "" = the session name */
  char mid[MTL_MAX_LEGS][16]; /* a=mid; "" = "primary", "secondary" */
  char channel_order[64];     /* audio */
  char ts_refclk[64];         /* parse output: the sender's reference clock */
  char fmtp_extra[128];       /* appended to a=fmtp / read from it: profile, level,
                                 sublevel, PAR, DID_SDID, measuredpixclk */
  uint32_t rtcp_port;         /* parse output: a=rtcp; 0 = none */
  uint32_t option_count;      /* parse output: entries of options */
  struct mtl_option options[8]; /* parse output: the per-stream option keys */
  uint64_t reserved[4];
};
/* The SDP of a created session as it is now. The length written (without the NUL),
   -MTL_ENOSPC if cap is short, -MTL_EBUSY (WRONG_STATE) while a value it needs is not
   known yet (an ANC raster before start). CP. */
MTL_API_CP int mtl_sdp_render(mtl_session_h s, const struct mtl_sdp_meta* MTL_NULLABLE meta,
                              char* buf, size_t cap);
/* An SDP into sc (direction as given; flows, payload types, source filters, the essence
   member) and meta (rx.rtp_offset, rx.mediaclk and the crypto.* options into
   meta.options, for the caller to pass in sc->options). Legs from a=group:DUP, as two m-lines or one m-line with two source filters; legs
   beyond those parsed are zeroed and their legs_disabled bits set (reserved), so a
   one-leg SDP never leaves a stale second leg. The leg count; unknown attributes are
   ignored, a malformed required one is -MTL_EINVAL naming it. CP. */
MTL_API_CP int mtl_sdp_parse(const char* sdp, size_t len, struct mtl_session_config* sc,
                             struct mtl_sdp_meta* MTL_NULLABLE meta);
MTL_SIZE_CHECK(mtl_sdp_meta, 608);
#endif


#if defined(__cplusplus)
}
#endif

/* clang-format on */
#endif /* MTL_EXPERIMENTAL_MTL_SDP_H */
