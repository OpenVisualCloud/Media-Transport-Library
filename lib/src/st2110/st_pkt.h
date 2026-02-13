/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef _ST_LIB_PKT_HEAD_H_
#define _ST_LIB_PKT_HEAD_H_

#include "st40_api.h"
#include "st41_api.h"

#define MTL_LITTLE_ENDIAN /* x86 use little endian */

#define ST_RVRTP_VERSION_2 (2)

#define ST_RVRTP_PAYLOAD_TYPE_RAW_VIDEO (112)
#define ST_RARTP_PAYLOAD_TYPE_PCM_AUDIO (111)
#define ST_RANCRTP_PAYLOAD_TYPE_ANCILLARY (113)
#define ST_RFMDRTP_PAYLOAD_TYPE_FASTMETADATA (115)

#define ST_TP_CINST_DRAIN_FACTOR (1.1f) /* Drain factor */

#define ST_TP_RTP_WRAP_AROUND (0x100000000)

/* total size: 54 */
struct st_rfc3550_hdr {
  struct rte_ether_hdr eth;      /* size: 14 */
  struct rte_ipv4_hdr ipv4;      /* size: 20 */
  struct rte_udp_hdr udp;        /* size: 8 */
  struct st_rfc3550_rtp_hdr rtp; /* size: 12 */
} __attribute__((__packed__)) __rte_aligned(2);

/* if this rfc4175 rtp carry a user meta data */
#define ST20_LEN_USER_META (0x1 << 15)

/* total size: 62 */
struct st_rfc4175_video_hdr {
  struct rte_ether_hdr eth;        /* size: 14 */
  struct rte_ipv4_hdr ipv4;        /* size: 20 */
  struct rte_udp_hdr udp;          /* size: 8 */
  struct st20_rfc4175_rtp_hdr rtp; /* size: 20 */
} __attribute__((__packed__)) __rte_aligned(2);

/* total size: 58 */
struct st22_rfc9134_video_hdr {
  struct rte_ether_hdr eth;        /* size: 14 */
  struct rte_ipv4_hdr ipv4;        /* size: 20 */
  struct rte_udp_hdr udp;          /* size: 8 */
  struct st22_rfc9134_rtp_hdr rtp; /* size: 16 */
} __attribute__((__packed__)) __rte_aligned(2);

/* total size: 54 */
struct st_rfc3550_audio_hdr {
  struct rte_ether_hdr eth;      /* size: 14 */
  struct rte_ipv4_hdr ipv4;      /* size: 20 */
  struct rte_udp_hdr udp;        /* size: 8 */
  struct st_rfc3550_rtp_hdr rtp; /* size: 12 */
} __attribute__((__packed__)) __rte_aligned(2);

/* total size: 62 */
struct st_rfc8331_anc_hdr {
  struct rte_ether_hdr eth;        /* size: 14 */
  struct rte_ipv4_hdr ipv4;        /* size: 20 */
  struct rte_udp_hdr udp;          /* size: 8 */
  struct st40_rfc8331_rtp_hdr rtp; /* size: 20 */
} __attribute__((__packed__)) __rte_aligned(2);

/* total size: 58 */
struct st41_fmd_hdr {
  struct rte_ether_hdr eth; /* size: 14 */
  struct rte_ipv4_hdr ipv4; /* size: 20 */
  struct rte_udp_hdr udp;   /* size: 8 */
  struct st41_rtp_hdr rtp;  /* size: 16 */
} __attribute__((__packed__)) __rte_aligned(2);

#define ST_PKT_VIDEO_HDR_LEN \
  (sizeof(struct st_rfc4175_video_hdr) - sizeof(struct rte_ether_hdr))

#define ST22_PKT_VIDEO_HDR_LEN \
  (sizeof(struct st22_rfc9134_video_hdr) - sizeof(struct rte_ether_hdr))

#define ST_PKT_AUDIO_HDR_LEN \
  (sizeof(struct st_rfc3550_audio_hdr) - sizeof(struct rte_ether_hdr))

#define ST_PKT_ANC_HDR_LEN \
  (sizeof(struct st_rfc8331_anc_hdr) - sizeof(struct rte_ether_hdr))

#define ST_PKT_FMD_HDR_LEN (sizeof(struct st41_fmd_hdr) - sizeof(struct rte_ether_hdr))

/* standard UDP is 1460 bytes */
#define ST_PKT_MAX_ETHER_BYTES \
  (1460 + sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr))

#endif
