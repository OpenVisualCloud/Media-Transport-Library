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

struct mtl_lcore_clean_pid_info {
  uint32_t lcore;
};

/** lcore clean action */
enum mtl_lcore_clean_action {
  /** auto, no args.
   * Remove lcore usage if the PID is inactive under the same hostname and user.
   */
  MTL_LCORE_CLEAN_PID_AUTO_CHECK = 0,
  /** clean as PID info, args to struct mtl_lcore_clean_pid_info */
  MTL_LCORE_CLEAN_LCORE,
  /** max value of this enum */
  MTL_LCORE_CLEAN_MAX,
};

/**
 * Print out the legacy lcore manager(shared memory) status.
 *
 * @return
 *   - 0 if successful.
 *   - <0: Error code if fail.
 */
int mtl_lcore_shm_print(void);

/**
 * Clean the unused lcore from the legacy lcore manager(shared memory).
 * @param action
 *   The action type.
 * @param args
 *   The args to the action type.
 * @param args_sz
 *   The size of the args.
 *
 * @return
 *   - 0 if successful.
 *   - <0: Error code if fail.
 */
int mtl_lcore_shm_clean(enum mtl_lcore_clean_action action, void *args, size_t args_sz);

#if defined(__cplusplus)
}
#endif

#endif
