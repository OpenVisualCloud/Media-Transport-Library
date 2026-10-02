/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * C harness API for the mt_map_add()/mt_map_remove() IOVA map unit tests.
 */

#ifndef TESTS_UNIT_DMA_MT_MAP_HARNESS_H
#define TESTS_UNIT_DMA_MT_MAP_HARNESS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ut_map_ctx ut_map_ctx;

/* A main handle with an initialised, empty map manager; NULL on failure. */
ut_map_ctx* ut_map_create(void);
void ut_map_destroy(ut_map_ctx* ctx);

/* mt_map_add() of [vaddr, vaddr + size); on success *iova is the assigned IOVA. */
int ut_map_add(ut_map_ctx* ctx, uintptr_t vaddr, size_t size, uint64_t* iova);

#ifdef __cplusplus
}
#endif

#endif /* TESTS_UNIT_DMA_MT_MAP_HARNESS_H */
