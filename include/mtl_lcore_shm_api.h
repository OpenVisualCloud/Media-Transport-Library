/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

/**
 * @file mtl_lcore_shm_api.h
 *
 * Interfaces to the legacy lcore shared memory manager.
 *
 */

#include "mtl_api.h"

#ifndef _MTL_LCORE_SHM_API_HEAD_H_
#define _MTL_LCORE_SHM_API_HEAD_H_

#if defined(__cplusplus)
extern "C" {
#endif

/**
 * Print out the legacy lcore manager(shared memory) status.
 *
 * @return
 *   - 0 if successful.
 *   - <0: Error code if fail.
 */
int mtl_lcore_shm_print(void);

#if defined(__cplusplus)
}
#endif

#endif
