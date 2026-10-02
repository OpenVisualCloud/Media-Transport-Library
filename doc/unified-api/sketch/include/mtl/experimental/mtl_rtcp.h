/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */
/* A hand-formatted design sketch: formatted when it moves to include/ (milestone M0). */
/* clang-format off */
/*
 * mtl_rtcp.h - RTCP sender reports and the IPMX Info Block, revision 0.2.
 *
 * With rtcp.sr set (the IPMX profile sets it) a TX session sends an RFC 3550 sender report
 * with the IPMX Info Block (TR-10-1 §8.7) and SDES CNAME on every leg, to the leg's
 * address at rtcp.dst_port (udp_port + 1), with the leg's DSCP and TTL, on the TR-10-1
 * schedule: video and compressed video one per frame (per field when interlaced), ANC and
 * fast metadata one per new RTP timestamp, each queued just before that unit's first
 * packet; audio one before the first packet and then every int(10 ms / ptime) packets.
 * None is sent while the session is muted. The report is prepared off the tasklet as a
 * template and only stamped there (NTP field = the unit's media time, RTP, counts). The
 * library writes the Info Block's own fields (ts-refclk and mediaclk from the time state
 * unless set) and the Media Info Block of the essence (video 0x0001, PCM 0x0002, AES3
 * 0x0004, privacy 0x0011 from the crypto.* options), then appends the application's
 * blocks unread (JPEG XS 0x0008, HDR 0x0006, ...). With rtcp.rx an RX session receives the
 * reports, keeps the latest per leg and maps each unit's RTP onto the sender's clock:
 * unit.media_tai_ns with MTL_UNITF_SENDER_TIME. MTL's own NACK retransmission (options
 * rtx.*) shares the port; payload types tell them apart. MTL_WAIT_RTCP is in mtl.h.
 */

#ifndef MTL_EXPERIMENTAL_MTL_RTCP_H
#define MTL_EXPERIMENTAL_MTL_RTCP_H

#include "mtl.h"

#if defined(__cplusplus)
extern "C" {
#endif

#if defined(MTL_LATER) /* a later phase (implementation-plan.md §6); the design is kept */
/* ---- TX: the Info Block ----------------------------------------------------------- */

#define MTL_RTCP_REFCLK_APP 0x1u   /* send ts_refclk as given; default: from the time state */
#define MTL_RTCP_MEDIACLK_APP 0x2u /* send mediaclk as given; default: from the media mode */
#define MTL_RTCP_MIB_APP_ONLY 0x4u /* only the application's blocks, not the library's */
struct mtl_rtcp_info {
  uint32_t struct_size;
  uint32_t flags;     /* MTL_RTCP_* */
  char ts_refclk[64]; /* the wire field: "ptp=IEEE1588-2008:ec-46-70-ff-fe-10-ff-b0:127" */
  char mediaclk[16];  /* at most 12 characters on the wire: "direct=0", "sender" */
  uint32_t par_num;   /* the essence block: pixel aspect ratio; 0 = 1:1 */
  uint32_t par_den;
  uint64_t measured_rate_milli; /* measured pixel clock or sample rate x 1000; 0 = nominal */
  const char* MTL_NULLABLE channel_order; /* audio: "SMPTE2110.(ST,ST)" */
  const void* MTL_NULLABLE mib;           /* application blocks, each 32-bit aligned */
  uint32_t mib_bytes;
  uint32_t reserved0;
  uint64_t reserved[2];
};
/* Applies from the next report. When the bytes change the block version increments
   ("tx.rtcp_info_version") and MTL_EVENT_RTCP_INFO is posted, the cue to re-render SDP and
   update NMOS; a unit's meta record MTL_META_RTCP_MIB only appends to that unit's report
   and never changes the version. -MTL_ENOSPC if report, Info Block and SDES do not fit one
   datagram; -MTL_EINVAL beyond the wire field sizes. CP. */
MTL_API_CP int mtl_rtcp_set_info(mtl_session_h s, const struct mtl_rtcp_info* info);

/* ---- RX: received reports ----------------------------------------------------------- */

#define MTL_RTCP_RPT_INFO 0x1u    /* an Info Block was present (copied into buf) */
#define MTL_RTCP_RPT_CHANGED 0x2u /* its version differs from the previous report's */
/* Output; rpt_size versions it. */
struct mtl_rtcp_report {
  uint32_t ssrc;
  uint32_t rtp;
  uint32_t packets;
  uint32_t octets;
  int64_t sender_ns;      /* the NTP field as the sender's clock, ns (IPMX: PTP format) */
  int64_t arrival_tai_ns; /* on the instance clock */
  uint32_t leg;
  uint32_t flags;        /* MTL_RTCP_RPT_* */
  uint32_t info_version;
  uint32_t info_bytes;   /* Info Block bytes copied into buf, at most cap */
};
/* The oldest unread report; a full ring drops the oldest ("rx.rtcp_sr_dropped"). 1, or
   -MTL_EAGAIN (MTL_WAIT_RTCP is then armed). WT. */
MTL_API_WT int mtl_rtcp_read(mtl_session_h s, struct mtl_rtcp_report* rpt, size_t rpt_size,
                             void* MTL_NULLABLE buf, size_t cap, int64_t timeout_ns);
/* Iterates the Media Info Blocks of an Info Block: *offset starts at 0. 1 with the next
   block's type and body, 0 at the end, -MTL_EINVAL if malformed. AS. */
MTL_API_AS int mtl_rtcp_mib_next(const void* info, size_t len, size_t* offset,
                                 uint16_t* type, const void** body, uint32_t* body_bytes);
MTL_SIZE_CHECK(mtl_rtcp_info, 144);
MTL_SIZE_CHECK(mtl_rtcp_report, 48);
#endif


#if defined(__cplusplus)
}
#endif

/* clang-format on */
#endif /* MTL_EXPERIMENTAL_MTL_RTCP_H */
