/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

/* Header for ST DPDK mem usage */
#include <rte_malloc.h>

#ifndef _MT_LIB_MEM_HEAD_H_
#define _MT_LIB_MEM_HEAD_H_

#define MT_DPDK_LIB_NAME "MT_DPDK"

static inline void *mt_malloc(size_t sz) { return malloc(sz); }

static inline void *mt_zmalloc(size_t sz) {
  void *p = malloc(sz);
  if (p)
    memset(p, 0x0, sz);
  return p;
}

static inline void mt_free(void *p) { free(p); }

#ifdef MTL_HAS_ASAN
int mt_asan_init(void);
int mt_asan_check(void);
void *mt_rte_malloc_socket(size_t sz, int socket);
void *mt_rte_zmalloc_socket(size_t sz, int socket);
void mt_rte_free(void *p);
#else
static inline void *mt_rte_malloc_socket(size_t sz, int socket) {
  return rte_malloc_socket(MT_DPDK_LIB_NAME, sz, RTE_CACHE_LINE_SIZE, socket);
}

static inline void *mt_rte_zmalloc_socket(size_t sz, int socket) {
  return rte_zmalloc_socket(MT_DPDK_LIB_NAME, sz, RTE_CACHE_LINE_SIZE, socket);
}

static inline void mt_rte_free(void *p) { rte_free(p); }
#endif

#endif
