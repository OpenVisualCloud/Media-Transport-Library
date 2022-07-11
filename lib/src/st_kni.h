/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef _ST_LIB_KNI_HEAD_H_
#define _ST_LIB_KNI_HEAD_H_

#include "st_main.h"

#ifdef ST_HAS_KNI
int st_kni_init(struct st_main_impl* impl);
int st_kni_uinit(struct st_main_impl* impl);
int st_kni_handle(struct st_main_impl* impl, enum st_port port, struct rte_mbuf** rx_pkts,
                  uint16_t nb_pkts);
#else
static inline int st_kni_init(struct st_main_impl* impl) { return 0; }
static inline int st_kni_uinit(struct st_main_impl* impl) { return 0; }
static inline int st_kni_handle(struct st_main_impl* impl, enum st_port port,
                                struct rte_mbuf** rx_pkts, uint16_t nb_pkts) {
  return -EIO;
}
#endif

#endif
