/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef _MT_LIB_QUIRK_HEAD_H_
#define _MT_LIB_QUIRK_HEAD_H_

static inline unsigned int mt_rte_ring_sc_dequeue_bulk(struct rte_ring* r,
                                                       void** obj_table, unsigned int n,
                                                       unsigned int* available) {
#if defined(__clang__) /* fix for clang release build */
  /* not sure why clang has issue with variable n, probably optimized code sequence */
  if (n == 4)
    return rte_ring_sc_dequeue_bulk(r, obj_table, 4, available);
  else if (n == 1)
    return rte_ring_sc_dequeue_bulk(r, obj_table, 1, available);
  else
    return rte_ring_sc_dequeue_bulk(r, obj_table, n, available);
#else
  return rte_ring_sc_dequeue_bulk(r, obj_table, n, available);
#endif
}

#endif
