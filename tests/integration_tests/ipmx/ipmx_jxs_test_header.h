/* SPDX-License-Identifier: LicenseRef-IPMX-TBD */

/* A synthetic JPEG XS header the Kahawai test codec writes for IPMX GBRPLANAR8 sessions,
 * so the library's per-frame TR-08 picture header check passes without a real encoder. */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "st_pipeline_api.h"

/* SOC, CAP, PIH and CDT of a 3 component codestream */
#define IPMX_TEST_JXS_HDR_LEN (46)

/* Write the header of a High444.12 RGB 8-bit codestream for the encoder session req,
 * its Plev picked independently of the library from TR-08 section 8.1.1. Returns
 * IPMX_TEST_JXS_HDR_LEN, or 0 when dst_size is too small. */
size_t ipmx_test_jxs_header(uint8_t* dst, size_t dst_size,
                            const struct st22_encoder_create_req* req);
