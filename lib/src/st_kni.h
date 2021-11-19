/*
 * Copyright (C) 2021 Intel Corporation.
 *
 * This software and the related documents are Intel copyrighted materials,
 * and your use of them is governed by the express license under which they
 * were provided to you ("License").
 * Unless the License provides otherwise, you may not use, modify, copy,
 * publish, distribute, disclose or transmit this software or the related
 * documents without Intel's prior written permission.
 *
 * This software and the related documents are provided as is, with no
 * express or implied warranties, other than those that are expressly stated
 * in the License.
 *
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
