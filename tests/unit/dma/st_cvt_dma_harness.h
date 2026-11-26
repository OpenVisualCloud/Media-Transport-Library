/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * C harness API for the DMA-offloaded converters: a DMA context allocation that fails on
 * demand, and a DMA lender on a dma_skeleton dmadev, whose worker thread does each copy
 * with rte_memcpy.
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

typedef struct ut_cvt_dma_skeleton ut_cvt_dma_skeleton;

/* A started dma_skeleton device with one lender; NULL on failure, logged. */
ut_cvt_dma_skeleton* ut_cvt_dma_skeleton_create(void);
/* Stops and removes the device; 0 or the first negative errno. */
int ut_cvt_dma_skeleton_destroy(ut_cvt_dma_skeleton* skel);
/* The lender, for the *_simd_dma() converters. */
mtl_udma_handle ut_cvt_dma_skeleton_handle(ut_cvt_dma_skeleton* skel);
/* The copies the device completed since it started; UINT64_MAX if unreadable. */
uint64_t ut_cvt_dma_skeleton_completed(ut_cvt_dma_skeleton* skel);

#ifdef __cplusplus
}
#endif

#endif /* TESTS_UNIT_DMA_ST_CVT_DMA_HARNESS_H */
