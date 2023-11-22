/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

/**
 * @file mtl_sch_api.h
 *
 * This header define the public interfaces of sch.
 *
 */

#include "mtl_api.h"

#ifndef _MTL_SCH_API_HEAD_H_
#define _MTL_SCH_API_HEAD_H_

#if defined(__cplusplus)
extern "C" {
#endif

/**
 * Handle to mtl sch context
 */
typedef struct mtl_sch_impl* mtl_sch_handle;
typedef struct mt_sch_tasklet_impl* mtl_tasklet_handle;

/** the tasklet is likely has pending task */
#define MTL_TASKLET_HAS_PENDING (1)
/** the tasklet is likely has finished all task */
#define MTL_TASKLET_ALL_DONE (0)

/**
 * Tasklets share the time slot on a sch, only non-block method can be used in handler
 * routine.
 */
struct mtl_sch_tasklet_ops {
  /** name */
  const char* name;
  /** private data to the callback */
  void* priv;

  /** the callback at the time when the sch started */
  int (*start)(void* priv);
  /** the callback at the time when the sch stopped */
  int (*stop)(void* priv);
  /**
   * the callback for task routine, only non-block method can be used for this callback
   * since all tasklets share the CPU time. Return MTL_TASKLET_ALL_DONE if no any pending
   * task, all are done. Return MTL_TASKLET_HAS_PENDING if it has pending tasks.
   */
  int (*handler)(void* priv);
  /**
   * the recommend sleep time(us) if all tasklet report MTL_TASKLET_ALL_DONE.
   * also this value can be set by mtl_tasklet_set_sleep at runtime.
   * leave to zero if you don't know.
   */
  uint64_t advice_sleep_us;
};

/**
 * Create one sch from media transport context.
 *
 * @param mt
 *   The handle to the media transport context.
 * @return
 *   - NULL on error.
 *   - Otherwise, the handle to the sch.
 */
mtl_sch_handle mtl_sch_create(mtl_handle mt);

/**
 * Start the sch.
 *
 * @param sch
 *   The handle to sch context.
 * @return
 *   - 0: Success.
 *   - <0: Error code.
 */
int mtl_sch_start(mtl_sch_handle sch);

/**
 * Stop the sch.
 *
 * @param sch
 *   The handle to sch context.
 * @return
 *   - 0: Success.
 *   - <0: Error code.
 */
int mtl_sch_stop(mtl_sch_handle sch);

/**
 * Free the sch.
 *
 * @param sch
 *   The handle to sch context.
 * @return
 *   - 0: Success.
 *   - <0: Error code.
 */
int mtl_sch_free(mtl_sch_handle sch);

/**
 * Register one tasklet into the sch. One tasklet can be registered at runtime after
 * mtl_sch_start.
 *
 * @param sch
 *   The sch context.
 * @param tasklet_ops
 *   The tasklet ops
 * @return
 *   - NULL on error.
 *   - Otherwise, the handle to the tasklet.
 */
mtl_tasklet_handle mtl_sch_register_tasklet(struct mtl_sch_impl* sch,
                                            struct mtl_sch_tasklet_ops* tasklet_ops);

/**
 * Unregister the tasklet from the bind sch. One tasklet can be unregistered at runtime
 * before mtl_sch_start.
 *
 * @param sch
 *   The handle to sch context.
 * @return
 *   - 0: Success.
 *   - <0: Error code.
 */
int mtl_sch_unregister_tasklet(mtl_tasklet_handle tasklet);

#if defined(__cplusplus)
}
#endif

#endif