/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#ifndef _IPMX_JXS_H_
#define _IPMX_JXS_H_

#include <stddef.h>
#include <stdint.h>

#include "st_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/* TR-08 section 8.1.1 Table 1: High444.12 for RGB 4:4:4 */
#define IPMX_JXS_PPIH (0x4a40)
/* ISO/IEC 21122-2 Plev high byte of the levels TR-08 section 8.1.1 allows */
#define IPMX_JXS_LEVEL_1K_1 (0x04)
#define IPMX_JXS_LEVEL_2K_1 (0x10)
#define IPMX_JXS_LEVEL_4K_2 (0x24)
#define IPMX_JXS_LEVEL_8K_2 (0x34)
/* TR-08 section 8.1.1 Sublev3bpp / Sublev4bpp; 0x06 is reserved in ISO/IEC
 * 21122-2:2022 Table A.16, the code is the VSF ipmx_jxsv_validate_pcap.py
 * SDP_SUBLEVEL_TO_PLEV_LO one */
#define IPMX_JXS_SUBLEV_3BPP (0x04)
#define IPMX_JXS_SUBLEV_4BPP (0x06)

/* why ipmx_jxs_pih_check() refused a codestream, returned negated */
enum ipmx_jxs_pih_err {
  IPMX_JXS_PIH_TRUNCATED = 1,
  IPMX_JXS_PIH_NO_SOC,
  IPMX_JXS_PIH_NO_CAP,
  IPMX_JXS_PIH_NO_PIH, /* not directly after CAP */
  IPMX_JXS_PIH_LPIH,
  IPMX_JXS_PIH_REPEATED, /* a second SOC, CAP or PIH (ISO/IEC 21122-1 A.4) */
  IPMX_JXS_PIH_BAD_SEGMENT,
  IPMX_JXS_PIH_PPIH,
  IPMX_JXS_PIH_LEVEL,
  IPMX_JXS_PIH_SUBLEVEL,
  IPMX_JXS_PIH_SUBLEVEL_RATE,
  IPMX_JXS_PIH_WF,
  IPMX_JXS_PIH_HF,
  IPMX_JXS_PIH_NC,
  IPMX_JXS_PIH_CPIH,
  IPMX_JXS_PIH_NLX,
  IPMX_JXS_PIH_NLY,
  IPMX_JXS_PIH_QPIH,
  IPMX_JXS_PIH_NO_CDT,
  IPMX_JXS_PIH_LCDT,
  IPMX_JXS_PIH_B,
  IPMX_JXS_PIH_SAMPLING,
};

/**
 * The TR-08 section 8.1.1 Plev of an RGB 4:4:4 8-bit session whose codestream never
 * exceeds codestream_max bytes: 0 and *plev, or -ENOTSUP above 4 bpp or when no allowed
 * level fits.
 */
int ipmx_jxs_plev(uint32_t width, uint32_t height, enum st_fps fps, size_t codestream_max,
                  uint16_t* plev);

/**
 * Check a JPEG XS codestream of size bytes against the session: SOC, CAP, then a PIH
 * with Ppih High444.12, the level of plev and Sublev3bpp or Sublev4bpp no higher than
 * its sublevel, Sublev4bpp when the size bytes are above 3 bpp,
 * Wf width, Hf height, Nc 3, Cpih 1, NL,x 5, NL,y 2, Qpih 1, and a CDT of B 8 with
 * sx = sy = 1 for every component. 0, or the negated enum ipmx_jxs_pih_err. Never reads
 * past size.
 */
int ipmx_jxs_pih_check(const uint8_t* cs, size_t size, uint32_t width, uint32_t height,
                       uint16_t plev);

#ifdef __cplusplus
}
#endif

#endif
