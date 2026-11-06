/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#ifndef _IPMX_SDP_H_
#define _IPMX_SDP_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ipmx/ipmx_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/* the addressing of one ST 2022-7 leg, as on the wire */
struct ipmx_sdp_leg {
  uint8_t sip[MTL_IP_ADDR_LEN];
  uint8_t dip[MTL_IP_ADDR_LEN];
  uint16_t udp_port;
};

/* ST_MAX_NAME_LEN, the session name with its NUL */
#define IPMX_SDP_NAME_MAX (32)

/* everything an SDP says but its o= session id and version */
struct ipmx_sdp_media {
  char name[IPMX_SDP_NAME_MAX];
  uint8_t payload_type;
  bool wide; /* ST 2110-21 type W, else type N */
  uint8_t num_port;
  struct ipmx_sdp_leg leg[MTL_SESSION_PORT_MAX];
  struct ipmx_video_info info;
  bool jxsv; /* st22: the jxs and bitrate_kbps fields are set */
  struct ipmx_jxs_info jxs;
  uint32_t bitrate_kbps;
};

/**
 * Write the IPMX SDP of m to buf, video/jxsv with m->jxsv, else video/raw: its length,
 * -ENOSPC with buf empty, or -ENOTSUP for interlaced, segmented or non-square PAR info,
 * or a jxsv profile, level or sublevel without an SDP name here.
 */
int ipmx_sdp_build(const struct ipmx_sdp_media* m, uint64_t sess_id,
                   uint64_t sess_version, char* buf, size_t len);

/**
 * The b=AS of an st22 stream, ceil kbit/s clamped to UINT32_MAX: the packets of a
 * frame_max byte frame, each pkt_len payload bytes with its IPv4, UDP, RTP and RFC 9134
 * headers, rate_num / rate_den times a second.
 */
uint32_t ipmx_sdp_jxsv_kbps(uint64_t frame_max, uint32_t pkt_len, uint32_t rate_num,
                            uint32_t rate_den);

struct st_tx_video_session_impl;
/** The SDP of s, see ipmx_st20_tx_get_sdp(). */
int ipmx_sdp_tx_get(struct st_tx_video_session_impl* s, char* buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif
