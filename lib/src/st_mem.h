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

/* Header for ST DPDK mem usage */
#include <rte_malloc.h>

#ifndef _ST_LIB_MEM_HEAD_H_
#define _ST_LIB_MEM_HEAD_H_

#define ST_DPDK_LIB_NAME "ST_DPDK"

static inline void* st_malloc(size_t sz) { return malloc(sz); }

static inline void* st_zmalloc(size_t sz) {
  void* p = malloc(sz);
  if (p) memset(p, 0x0, sz);
  return p;
}

static inline void st_free(void* p) { free(p); }

static inline void* st_rte_malloc_socket(size_t sz, int socket) {
  return rte_malloc_socket(ST_DPDK_LIB_NAME, sz, RTE_CACHE_LINE_SIZE, socket);
}

static inline void* st_rte_zmalloc_socket(size_t sz, int socket) {
  return rte_zmalloc_socket(ST_DPDK_LIB_NAME, sz, RTE_CACHE_LINE_SIZE, socket);
}

static inline void st_rte_free(void* p) { rte_free(p); }

#endif
