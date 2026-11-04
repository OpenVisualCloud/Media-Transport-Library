/* SPDX-License-Identifier: LicenseRef-IPMX-TBD */

#ifndef _IPMX_INFO_H_
#define _IPMX_INFO_H_

#include <stdbool.h>
#include <stdint.h>

#include "ipmx/ipmx_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/* TR-10-2 section 10: the IPMX Info Block with one uncompressed video Media Info Block */
#define IPMX_INFO_TAG (0x5831)
#define IPMX_INFO_BLOCK_LEN (176)
/* TR-10-11 section 12 and TR-10-15 Part 1 section 9: 0x0003 then the JPEG XS 0x0008 */
#define IPMX_INFO_BLOCK_JXS_LEN (IPMX_INFO_BLOCK_LEN + IPMX_MEDIA_INFO_JXS_LEN)
#define IPMX_INFO_HDR_LEN (84)
#define IPMX_INFO_BLOCK_VERSION_OFFSET (4)
#define IPMX_TS_REFCLK_OFFSET (8)
#define IPMX_TS_REFCLK_LEN (64)
#define IPMX_MEDIACLK_OFFSET (72)
#define IPMX_MEDIA_INFO_VIDEO (0x0001)
#define IPMX_MEDIA_INFO_VIDEO_LEN (92)
#define IPMX_MEDIA_INFO_CBR_VIDEO (0x0003)
#define IPMX_MEDIA_INFO_JXS (0x0008)
#define IPMX_MEDIA_INFO_JXS_LEN (12)

struct mtl_main_impl;
struct st_tx_video_session_impl;
/** Fill info from the session ops and its primary port MAC, 0 or negative errno. */
int ipmx_video_info_from_session(struct mtl_main_impl* impl,
                                 struct st_tx_video_session_impl* s,
                                 struct ipmx_video_info* info);
/** Write the IPMX Info Block of info to buf, returns IPMX_INFO_BLOCK_LEN. */
uint16_t ipmx_info_block_build(const struct ipmx_video_info* info, uint8_t* buf);
/** Write the st22 Info Block, 0x0003 of info then 0x0008 of jxs; its length. */
uint16_t ipmx_info_block_jxs_build(const struct ipmx_video_info* info,
                                   const struct ipmx_jxs_info* jxs, uint8_t* buf);

/* The built-in PTP state of one port, as the ts-refclk decision reads it */
struct ipmx_ptp_snap {
  bool builtin;         /* the built-in PTP gives the port its PTP time */
  bool connected;       /* at least one delay request/response exchange */
  uint64_t announce_ns; /* TSC ns of the last Announce of the master, 0 for none */
  int8_t log_announce_interval;
  uint64_t gm_id;
  uint8_t domain;
};

/* The Common Reference Clock a ts-refclk names, localmac when ptp is false */
struct ipmx_refclk {
  bool ptp;
  uint8_t domain;
  uint64_t gm_id;
};

/**
 * TR-10-1 section 10.4: the PTP grandmaster when the built-in PTP has a master it
 * exchanged delays with and whose last Announce is within 3 announce intervals of now_ns.
 */
struct ipmx_refclk ipmx_refclk_from_ptp(const struct ipmx_ptp_snap* snap,
                                        uint64_t now_ns);
/** Write the ts-refclk of clk zero padded to its 64 bytes, localmac copied if not ptp. */
void ipmx_refclk_print(const struct ipmx_refclk* clk, const char* localmac,
                       char* ts_refclk);

#ifdef __cplusplus
}
#endif

#endif
