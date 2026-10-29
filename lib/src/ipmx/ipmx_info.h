/* SPDX-License-Identifier: LicenseRef-IPMX-TBD */

#ifndef _IPMX_INFO_H_
#define _IPMX_INFO_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* TR-10-2 section 10: the IPMX Info Block with one uncompressed video Media Info Block */
#define IPMX_INFO_BLOCK_LEN (176)

/* The IPMX Info Block content of an st20 stream, the strings zero padded */
struct ipmx_video_info {
  uint8_t version;
  char ts_refclk[64];
  char mediaclk[12];
  char sampling[16];
  bool floating;
  uint8_t depth;
  bool gpm;
  bool interlaced;
  bool segmented;
  uint8_t par_w;
  uint8_t par_h;
  char range[12];
  char colorimetry[20];
  char tcs[16];
  uint16_t width;
  uint16_t height;
  uint32_t rate_num;
  uint32_t rate_den;
  uint64_t pixclk;
  uint16_t htotal;
  uint16_t vtotal;
};

struct mtl_main_impl;
struct st_tx_video_session_impl;
/** Fill info from the session ops and its primary port MAC, 0 or negative errno. */
int ipmx_video_info_from_session(struct mtl_main_impl* impl,
                                 struct st_tx_video_session_impl* s,
                                 struct ipmx_video_info* info);
/** Write the IPMX Info Block of info to buf, returns IPMX_INFO_BLOCK_LEN. */
uint16_t ipmx_info_block_build(const struct ipmx_video_info* info, uint8_t* buf);

#ifdef __cplusplus
}
#endif

#endif
