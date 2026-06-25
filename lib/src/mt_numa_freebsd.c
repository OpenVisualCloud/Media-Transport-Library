/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#if defined(__FreeBSD__) && !defined(MTL_HAS_NUMA)

#include <stdlib.h>
#include <string.h>

#include "mt_log.h"
#include "mt_platform_freebsd.h"

#define BITS_PER_LONG (sizeof(unsigned long) * 8)

/* NUMA stub implementation for FreeBSD when libnuma is not available */

int numa_available(void) {
  /* Pretend NUMA is available but return single socket */
  return 0;
}

int numa_max_node(void) {
  /* Single socket system */
  return 0;
}

int numa_node_of_cpu(int cpu) {
  /* All CPUs on node 0 */
  (void)cpu;
  return 0;
}

void* numa_alloc_onnode(size_t size, int node) {
  /* Fallback to regular malloc, ignore node */
  (void)node;
  return malloc(size);
}

void numa_free(void* mem, size_t size) {
  /* Use regular free */
  (void)size;
  free(mem);
}

/*
 * Bitmask helpers. These are never called in the no-libnuma stub path because
 * numa_max_node() returns 0, making numa_nodes==1 and the guard
 * "(numa_nodes > 1)" false.  The implementations are provided so that the
 * code compiles and links cleanly regardless.
 */

struct bitmask* numa_bitmask_alloc(unsigned int n) {
  struct bitmask* bmp = malloc(sizeof(struct bitmask));
  if (!bmp) return NULL;
  bmp->size = n;
  size_t words = (n + BITS_PER_LONG - 1) / BITS_PER_LONG;
  bmp->maskp = calloc(words ? words : 1, sizeof(unsigned long));
  if (!bmp->maskp) {
    free(bmp);
    return NULL;
  }
  return bmp;
}

struct bitmask* numa_bitmask_setbit(struct bitmask* bmp, unsigned int n) {
  if (bmp && n < bmp->size) {
    unsigned int word = n / BITS_PER_LONG;
    unsigned int bit = n % BITS_PER_LONG;
    bmp->maskp[word] |= (1UL << bit);
  }
  return bmp;
}

void numa_bind(struct bitmask* bmp) {
  /* No-op: single-socket FreeBSD stub */
  (void)bmp;
}

void numa_bitmask_free(struct bitmask* bmp) {
  if (bmp) {
    free(bmp->maskp);
    free(bmp);
  }
}

#endif /* __FreeBSD__ && !MTL_HAS_NUMA */
