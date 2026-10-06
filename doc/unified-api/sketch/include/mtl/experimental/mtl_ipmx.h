/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */
/* Hand-formatted: clang-format stays off so the milestone tags keep their place
   (check.sh lint 5). */
/* clang-format off */
/*
 * mtl_ipmx.h - RTCP sender reports and the IPMX Info Block, and payload encryption (IPMX
 * PEP), revision 0.2. Phase 7 (implementation-plan.md §6.7): everything here is
 * under MTL_LATER, with the option keys rtcp.*, crypto.* and session.profile
 * (mtl_options.h). TX sender reports driven by the rtcp.* options, with the library's own
 * Info Block, come in MS5.
 *
 * RTCP. With rtcp.sr set (the IPMX profile sets it) a TX session sends an RFC 3550 sender
 * report with the IPMX Info Block (TR-10-1 §8.7) and SDES CNAME on every leg, to the
 * leg's address at rtcp.dst_port (udp_port + 1), with the leg's DSCP and TTL, on the
 * TR-10-1 schedule: video and compressed video one per frame (per field when interlaced),
 * ANC and fast metadata one per new RTP timestamp, each queued just before that unit's
 * first packet; audio one before the first packet and then every int(10 ms / ptime)
 * packets. None is sent while the session is muted. The report is prepared off the
 * tasklet as a template and only stamped there (NTP field = the unit's media time, RTP,
 * counts). The library writes the Info Block's own fields (ts-refclk and mediaclk from the
 * time state unless set) and the Media Info Block of the essence (video 0x0001, PCM
 * 0x0002, AES3 0x0004, privacy 0x0011 from the crypto.* options), then appends the
 * application's blocks unread (JPEG XS 0x0008, HDR 0x0006, ...). With rtcp.rx an RX
 * session receives the reports, keeps the latest per leg and maps each unit's RTP onto
 * the sender's clock: unit.media_tai_ns with MTL_UNITF_SENDER_TIME. MTL's own NACK
 * retransmission (options rtx.*) shares the port; payload types tell them apart.
 * MTL_WAIT_RTCP is in mtl.h (under MTL_LATER).
 *
 * SDP is mtl_sdp.h, the header of the companion library libmtl_sdp.
 *
 * Encryption (TR-10-13). Set the option crypto.scheme (mtl_options.h, keys crypto.*) and
 * the session encrypts (TX) or decrypts (RX) its payloads with AES in counter mode. The
 * parameters are options, none of them secret, so an IS-05 activation can change them at
 * its boundary and the SDP helper can render and parse them. The library writes and
 * parses the RFC 8285 counter extensions, keeps the counters, sizes packets for the
 * extension, and runs the cipher off the tasklet: in the caller at submit and dequeue
 * (both become DPC), or on crypto.workers threads. ST 2022-7 legs carry the same
 * ciphertext. Keys never are options: they go in with mtl_crypto_set_key(), are kept in
 * locked, non-dumpable memory, zeroised on replace and close, and never reach logs, stats
 * or captures. Key derivation (pre-shared keys, KDF, ECDH) stays with the application,
 * which must use a fresh key generator per boot. A TX unit without a key is DROPPED
 * (NO_KEY), never sent in clear; an RX packet that fails authentication is a lost packet
 * ("rx.crypto_auth_fail"). HDCP keys never enter MTL: packet units, with the vendor's
 * code encrypting, carry HDCP streams.
 */

#ifndef MTL_EXPERIMENTAL_MTL_IPMX_H
#define MTL_EXPERIMENTAL_MTL_IPMX_H

#include "mtl.h"

#if defined(__cplusplus)
extern "C" {
#endif

#if defined(MTL_LATER) /* Phase 7 (implementation-plan.md §6.7) */
/* ---- RTCP: Media Info Blocks --------------------------------------------------------- */

/* Iterates Media Info Blocks (a 16-bit type and a 16-bit body length in 32-bit words, big
   endian, then the body): *offset starts at mtl_rtcp_report.mib_offset in the Info Block
   mtl_rtcp_read() copied. 1 with the next block's type and body, 0 at the end, -MTL_EINVAL
   if malformed. Exported: it parses bytes from the network (sketch/README.md). AS.
   (Phase 7) */
MTL_API_AS(LATER) int mtl_rtcp_mib_next(const void* info, size_t len, size_t* offset,
                                        uint16_t* type, const void** body,
                                        uint32_t* body_bytes);

/* ---- Encryption: values of crypto.scheme and crypto.mode ------------------------------ */

enum mtl_crypto_scheme {
  MTL_CRYPTO_PEP_RTP = 1,    /* one key per activation */
  MTL_CRYPTO_PEP_RTP_KV = 2, /* key versions, switched at unit boundaries */
};
enum mtl_crypto_mode {
  MTL_CRYPTO_AES128_CTR = 1, /* mandatory in TR-10-13 */
  MTL_CRYPTO_AES256_CTR = 2,
  MTL_CRYPTO_AES128_CTR_CMAC64 = 3, /* MAC modes always take the copy path */
  MTL_CRYPTO_AES256_CTR_CMAC64 = 4,
  MTL_CRYPTO_AES128_CTR_CMAC64_AAD = 5,
  MTL_CRYPTO_AES256_CTR_CMAC64_AAD = 6,
};

/* ---- RTCP TX: the Info Block --------------------------------------------------------- */

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
   datagram; -MTL_EINVAL beyond the wire field sizes. CP. (Phase 7) */
MTL_API_CP(LATER) int mtl_rtcp_set_info(mtl_session_h s, const struct mtl_rtcp_info* info);

/* ---- RTCP RX: received reports --------------------------------------------------------- */

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
  uint32_t mib_offset;   /* the first Media Info Block in buf (mtl_rtcp_mib_next) */
  uint32_t reserved0;
};
/* The oldest unread report; a full ring drops the oldest ("rx.rtcp_sr_dropped"). 1, or
   -MTL_EAGAIN; an event loop arms MTL_WAIT_RTCP on a queue (mtl_queue_arm). WT. (Phase 7) */
MTL_API_WT(LATER) int mtl_rtcp_read(mtl_session_h s, struct mtl_rtcp_report* rpt, size_t rpt_size,
                                    void* MTL_NULLABLE buf, size_t cap, int64_t timeout_ns);

/* ---- Encryption: keys ------------------------------------------------------------------ */

/* Installs key `key_version` (16 or 32 bytes, copied); key NULL zeroises every key, and TX
   units are then DROPPED (NO_KEY). TX: used from the first unit at or after `when` (NULL =
   the next unit). The counter restarts at 0 only for key bytes this session never used;
   re-installing a used key continues its counter, so no IV and counter pair repeats
   under one key. RX: applies to units whose media time is at or after `when` (the update
   rule of mtl.h), kept beside the current key and chosen per packet by the extension's key
   version (RTP_KV); a packet with an unknown version counts in "rx.crypto_unknown_key"
   and posts MTL_EVENT_KEY_NEEDED. Never readable back. CP. (Phase 7) */
MTL_API_CP(LATER) int mtl_crypto_set_key(mtl_session_h s, uint32_t key_version,
                                         const uint8_t* MTL_NULLABLE key, uint32_t key_bytes,
                                         const struct mtl_when* MTL_NULLABLE when);

MTL_SIZE_CHECK(mtl_rtcp_info, 144);
MTL_SIZE_CHECK(mtl_rtcp_report, 56);
#endif

#if defined(__cplusplus)
}
#endif

/* clang-format on */
#endif /* MTL_EXPERIMENTAL_MTL_IPMX_H */
