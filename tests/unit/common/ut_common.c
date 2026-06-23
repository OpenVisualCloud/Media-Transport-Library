/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 *
 * Shared infrastructure for unit test harnesses.
 */

#include "ut_common.h"

#include <stdlib.h>
#include <string.h>

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
    static char a6[] = "--vdev=net_null0";
    static char* args[] = {a0, a1, a2, a3, a4, a5, a6};
    int rc = rte_eal_init(7, args);
    if (rc < 0 && rte_eal_has_hugepages() == 0) {
      /* EAL already initialised — fine */
    } else if (rc < 0) {
      return -1;
    }
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
