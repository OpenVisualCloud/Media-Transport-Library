/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 *
 * Shared infrastructure for unit test harnesses.
 */

#include "ut_common.h"

#include <rte_lcore.h>
#include <rte_mbuf_dyn.h>
#include <stdlib.h>
#include <string.h>

#ifdef WINDOWSENV
/* rte_lcore_id() reads a thread-local variable of the EAL, and on Windows the
 * EAL is inside libmtl.dll: DPDK builds no shared library there, so MTL holds
 * the only copy. mingw-w64 GCC gives a __thread variable native Windows
 * thread-local storage, and the relocation of such a variable holds an offset
 * inside one module, so no binary can read the copy of a DLL. This binary keeps
 * a copy of its own.
 *
 * The value is 0, which is the lcore of the main thread: ut_eal_init() starts
 * the EAL with "-c1". It is safe for a worker thread as well, because every
 * mempool here has a cache of 0, so rte_mempool_get() and rte_mempool_put()
 * hold no per lcore cache, and two threads cannot take the same cache. A pool
 * with a cache would need the real lcore id of each thread. */
RTE_DEFINE_PER_LCORE(unsigned, _lcore_id) = 0;
#endif

/* ── globals ──────────────────────────────────────────────────────────── */

static bool g_eal_ready;
static struct rte_mempool* g_pool;

/* ── EAL init ─────────────────────────────────────────────────────────── */

int ut_eal_init(void) {
  if (g_eal_ready && g_pool) return 0;

  if (!g_eal_ready) {
    static char a0[] = "unit_test";
    static char a1[] = "--no-huge";
    static char a2[] = "--no-shconf";
    static char a3[] = "-c1";
    static char a4[] = "-n1";
    static char a5[] = "--no-pci";
    static char* args[] = {a0, a1, a2, a3, a4, a5};
    if (rte_eal_init(RTE_DIM(args), args) < 0) return -1;
    g_eal_ready = true;
  }

  if (!g_pool) {
    g_pool = rte_pktmbuf_pool_create("ut_pool", UT_POOL_SIZE, 0, 0,
                                     RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
    if (!g_pool) return -1;
  }

  return 0;
}

/* ── pool accessor ────────────────────────────────────────────────────── */

struct rte_mempool* ut_pool(void) {
  return g_pool;
}

/* ── ring factory ─────────────────────────────────────────────────────── */

struct rte_ring* ut_ring_create(const char* name, unsigned int size) {
  return rte_ring_create(name, size, rte_socket_id(), RING_F_SP_ENQ | RING_F_SC_DEQ);
}

/* ── drain helper ─────────────────────────────────────────────────────── */

void ut_ring_drain(struct rte_ring* ring) {
  if (!ring) return;
  struct rte_mbuf* pkt = NULL;
  while (rte_ring_sc_dequeue(ring, (void**)&pkt) == 0) rte_pktmbuf_free(pkt);
}

/* ── HW RX timestamp mock ─────────────────────────────────────────────── */

int ut_register_hw_rx_timestamp(void) {
  static int g_offset = -1;
  if (g_offset < 0 && rte_mbuf_dyn_rx_timestamp_register(&g_offset, NULL) < 0) return -1;
  return g_offset;
}

void ut_mbuf_set_hw_timestamp(struct rte_mbuf* mbuf, int dynfield_offset,
                              uint64_t raw_ns) {
  *RTE_MBUF_DYNFIELD(mbuf, dynfield_offset, rte_mbuf_timestamp_t*) = raw_ns;
}
