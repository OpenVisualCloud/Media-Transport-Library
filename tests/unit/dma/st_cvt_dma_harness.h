/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * C harness API for the CPU fallback of the le10-to-be10 DMA converter.
 */

#ifndef TESTS_UNIT_DMA_ST_CVT_DMA_HARNESS_H
#define TESTS_UNIT_DMA_ST_CVT_DMA_HARNESS_H

#include "st_convert_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/* st20_rfc4175_422le10_to_422be10_simd_dma() over a fake DMA device, with every DMA
 * context allocation failing for the duration of the call. */
int ut_cvt_dma_le10_to_be10(struct st20_rfc4175_422_10_pg2_le* pg_le,
                            struct st20_rfc4175_422_10_pg2_be* pg_be, uint32_t w,
                            uint32_t h, enum mtl_simd_level level);

/* The DMA context allocations the last ut_cvt_dma_le10_to_be10() call failed. */
int ut_cvt_dma_failed_ctx_inits(void);

#ifdef __cplusplus
}
#endif

#endif /* TESTS_UNIT_DMA_ST_CVT_DMA_HARNESS_H */
