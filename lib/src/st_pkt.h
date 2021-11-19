/*
 * Copyright (C) 2021 Intel Corporation.
 *
 * This software and the related documents are Intel copyrighted materials,
 * and your use of them is governed by the express license under which they
 * were provided to you ("License").
 * Unless the License provides otherwise, you may not use, modify, copy,
 * publish, distribute, disclose or transmit this software or the related
 * documents without Intel's prior written permission.
 *
 * This software and the related documents are provided as is, with no
 * express or implied warranties, other than those that are expressly stated
 * in the License.
 *
 */

#ifndef _ST_LIB_PKT_HEAD_H_
#define _ST_LIB_PKT_HEAD_H_

#define ST_LITTLE_ENDIAN /* x86 use little endian */

#define ST_RVRTP_VERSION_2 (2)

#define ST_RVRTP_PAYLOAD_TYPE_RAW_VIDEO (112)
#define ST_RARTP_PAYLOAD_TYPE_PCM_AUDIO (111)
#define ST_RANCRTP_PAYLOAD_TYPE_ANCILLARY (113)

#define ST_EBU_CINST_DRAIN_FACTOR (1.1f) /* Drain factor */

#define ST_EBU_LATENCY_MAX_US (1000)                         /* Latency in us */
#define ST_EBU_LATENCY_MAX_NS (1000 * ST_EBU_LATENCY_MAX_US) /* Latency in ns */

#define ST_EBU_RTP_OFFSET_MIN (-1) /* MIN RTP Offset */

#define ST_EBU_RTP_WRAP_AROUND (0x100000000)

#define ST_EBU_PASS_NARROW "PASSED NARROW"
#define ST_EBU_PASS_WIDE "PASSED WIDE"
#define ST_EBU_PASS_WIDE_WA "PASSED WIDE WA" /* Extend WA WIDE as no hw rx time */
#define ST_EBU_PASS "PASSED"
#define ST_EBU_FAIL "FAILED"

/* total size: 62 */
struct st_rfc4175_hdr_single {
  struct rte_ether_hdr eth;        /* size: 14 */
  struct rte_ipv4_hdr ipv4;        /* size: 20 */
  struct rte_udp_hdr udp;          /* size: 8 */
  struct st20_rfc4175_rtp_hdr rtp; /* size: 20 */
} __attribute__((__packed__)) __rte_aligned(2);

/* total size: 54 */
struct st_rfc3550_audio_hdr {
  struct rte_ether_hdr eth;        /* size: 14 */
  struct rte_ipv4_hdr ipv4;        /* size: 20 */
  struct rte_udp_hdr udp;          /* size: 8 */
  struct st30_rfc3550_rtp_hdr rtp; /* size: 12 */
} __attribute__((__packed__)) __rte_aligned(2);

struct st_rfc8331_anc_hdr {
  struct rte_ether_hdr eth;        /* size: 14 */
  struct rte_ipv4_hdr ipv4;        /* size: 20 */
  struct rte_udp_hdr udp;          /* size: 8 */
  struct st40_rfc8331_rtp_hdr rtp; /* size: 20 */
} __attribute__((__packed__)) __rte_aligned(2);

#define ST_PKT_SLN_HDR_LEN \
  (sizeof(struct st_rfc4175_hdr_single) - sizeof(struct rte_ether_hdr))

#define ST_PKT_AUDIO_HDR_LEN \
  (sizeof(struct st_rfc3550_audio_hdr) - sizeof(struct rte_ether_hdr))

#define ST_PKT_ANC_HDR_LEN \
  (sizeof(struct st_rfc8331_anc_hdr) - sizeof(struct rte_ether_hdr))

#define ST_PKT_MAX_UDP_BYTES (1460) /* standard UDP is 1460 bytes */

#endif
