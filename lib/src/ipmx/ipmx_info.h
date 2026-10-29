/* SPDX-License-Identifier: LicenseRef-IPMX-TBD */

#ifndef _IPMX_INFO_H_
#define _IPMX_INFO_H_

#include <stdint.h>

#include "ipmx/ipmx_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/* TR-10-2 section 10: the IPMX Info Block with one uncompressed video Media Info Block */
#define IPMX_INFO_BLOCK_LEN (176)
#define IPMX_INFO_BLOCK_VERSION_OFFSET (4)

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
