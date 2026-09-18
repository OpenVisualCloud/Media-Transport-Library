/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef _MT_LIB_ST_MAIN_H_
#define _MT_LIB_ST_MAIN_H_

#include "../dev/mt_dev.h"
#include "../mt_dma.h"
#include "../mt_main.h"
#include "../mt_mcast.h"
#include "../mt_sch.h"
#include "../mt_simd.h"
#include "../mt_util.h"

/* A claim that succeeded without waiting must still spend the wake posted for
 * it, else the next blocking get_frame() returns at once with no frame. */
#define ST_BLOCK_WAKE_CONSUME(ctx)                     \
  do {                                                 \
    mt_pthread_mutex_lock(&(ctx)->block_wake_mutex);   \
    (ctx)->block_wake_pending = false;                 \
    mt_pthread_mutex_unlock(&(ctx)->block_wake_mutex); \
  } while (0)

static inline enum st21_tx_pacing_way st_tx_pacing_way(struct mtl_main_impl* impl,
                                                       enum mtl_port port) {
  return mt_if(impl, port)->tx_pacing_way;
}

#endif
