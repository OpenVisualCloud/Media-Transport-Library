/* SPDX-License-Identifier: BSD-3-Clause */
#include <rte_eal.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_lcore.h>
#include <rte_log.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_ring.h>
#include <rte_udp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "st20_api.h"
#include "st2110/st_pkt.h"
#include "st2110/st_rx_ancillary_session.h"
#include "st40_api.h"
#include "st41_api.h"
#include "st_api.h"

extern int mt_set_log_global_level(enum mtl_log_level level);

static void st40_fuzz_log_printer(enum mtl_log_level level, const char* format, ...) {
  MTL_MAY_UNUSED(level);
  va_list args;
  va_start(args, format);
  vfprintf(stderr, format, args);
  va_end(args);
}

#define ST40_FUZZ_RING_SIZE 512
#define ST40_FUZZ_POOL_SIZE 2048
#define ST40_FUZZ_RING_NAME "st40_rx_fuzz_ring"
#define ST40_FUZZ_POOL_NAME "st40_rx_fuzz_pool"
#define ST40_FUZZ_MAX_PKT_SIZE 2048
#define ST40_FUZZ_MIN_PKT_SIZE (sizeof(struct st_rfc8331_anc_hdr))

static struct mtl_main_impl g_impl;
static struct st_rx_ancillary_sessions_mgr g_mgr;
static struct st_rx_ancillary_session_impl g_session;
static struct rte_mempool* g_pool;
static struct rte_ring* g_ring;
static bool g_eal_ready;

static void st40_fuzz_enable_logging(void) {
  static bool logging_ready;
  if (logging_ready) return;

  mt_set_log_global_level(MTL_LOG_LEVEL_DEBUG);
  rte_log_set_global_level(RTE_LOG_DEBUG);
  mtl_set_log_printer(st40_fuzz_log_printer);
  if (mtl_openlog_stream(stderr) < 0) {
    fprintf(stderr, "st40 fuzz: failed to route MTL logs to stderr\n");
  }
  logging_ready = true;
}

static void st40_fuzz_drain_ring(struct st_rx_ancillary_session_impl* s) {
  if (!s || !s->packet_ring) return;

  struct rte_mbuf* pkt = NULL;
  while (rte_ring_sc_dequeue(s->packet_ring, (void**)&pkt) == 0) {
    rte_pktmbuf_free(pkt);
  }
}

static int st40_fuzz_notify_rtp_ready(void* priv) {
  st40_fuzz_drain_ring((struct st_rx_ancillary_session_impl*)priv);
  return 0;
}

static void st40_fuzz_init_impl(void) {
  st40_fuzz_enable_logging();
  if (g_pool && g_ring) return;

  if (!g_eal_ready) {
    static char st40_arg0[] = "st40_rx_fuzz";
    static char st40_arg1[] = "--no-huge";
    static char st40_arg2[] = "--no-shconf";
    static char st40_arg3[] = "-c1";
    static char st40_arg4[] = "-n1";
    static char st40_arg5[] = "--no-pci";
    static char st40_arg6[] = "--vdev=net_null0";
    static char* eal_args[] = {st40_arg0, st40_arg1, st40_arg2, st40_arg3,
                               st40_arg4, st40_arg5, st40_arg6};
    static const int eal_argc = (int)(sizeof(eal_args) / sizeof(eal_args[0]));

    if (rte_eal_init(eal_argc, eal_args) < 0) {
      abort();
    }
    g_eal_ready = true;
  }

  g_pool = rte_pktmbuf_pool_create(ST40_FUZZ_POOL_NAME, ST40_FUZZ_POOL_SIZE, 0, 0,
                                   RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
  if (!g_pool) abort();

  g_ring = rte_ring_create(ST40_FUZZ_RING_NAME, ST40_FUZZ_RING_SIZE, rte_socket_id(),
                           RING_F_SP_ENQ | RING_F_SC_DEQ);
  if (!g_ring) abort();
}

static void st40_fuzz_reset_context(void) {
  if (!g_pool || !g_ring) return;

  st40_fuzz_drain_ring(&g_session);

  memset(&g_impl, 0, sizeof(g_impl));
  memset(&g_mgr, 0, sizeof(g_mgr));
  memset(&g_session, 0, sizeof(g_session));

  mt_stat_u64_init(&g_session.stat_time);

  g_impl.type = MT_HANDLE_MAIN;
  g_impl.tsc_hz = rte_get_tsc_hz();

  g_mgr.parent = &g_impl;
  g_mgr.idx = 0;

  g_session.idx = 0;
  g_session.socket_id = rte_socket_id();
  g_session.mgr = &g_mgr;
  g_session.packet_ring = g_ring;
  g_session.attached = true;
  g_session.ops.num_port = 1;
  g_session.ops.payload_type = 0;
  g_session.ops.interlaced = false;
  g_session.ops.rtp_ring_size = ST40_FUZZ_RING_SIZE;
  g_session.ops.notify_rtp_ready = st40_fuzz_notify_rtp_ready;
  g_session.ops.priv = &g_session;
  g_session.ops.name = "st40_rx_fuzz";
  g_session.redundant_error_cnt[MTL_SESSION_PORT_P] = 0;

  st_rx_ancillary_session_fuzz_reset(&g_session);
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size < ST40_FUZZ_MIN_PKT_SIZE || !data) return 0;
  size_t pkt_size = size;
  if (pkt_size > ST40_FUZZ_MAX_PKT_SIZE) pkt_size = ST40_FUZZ_MAX_PKT_SIZE;

  st40_fuzz_init_impl();
  st40_fuzz_reset_context();

  struct rte_mbuf* mbuf = rte_pktmbuf_alloc(g_pool);
  if (!mbuf) return 0;

  if (rte_pktmbuf_tailroom(mbuf) < pkt_size) {
    rte_pktmbuf_free(mbuf);
    return 0;
  }

  uint8_t* dst = rte_pktmbuf_mtod(mbuf, uint8_t*);
  memcpy(dst, data, pkt_size);
  mbuf->data_len = pkt_size;
  mbuf->pkt_len = pkt_size;

  st_rx_ancillary_session_fuzz_handle_pkt(&g_impl, &g_session, mbuf, MTL_SESSION_PORT_P);
  rte_pktmbuf_free(mbuf);
  return 0;
}
