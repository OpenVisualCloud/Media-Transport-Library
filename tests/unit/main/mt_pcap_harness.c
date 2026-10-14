/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include "main/mt_pcap_harness.h"

#include <rte_dev.h>
#include <rte_ethdev.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common/ut_common.h"
#undef MTL_HAS_USDT
#include "mt_main.h"
#include "mt_pcap.h"

#define UT_PCAP_VDEV "net_null_ut_pcap"
#define UT_PCAPNG_BLOCK_EPB 6

struct ut_pcap_ctx {
  struct mtl_main_impl impl;
  uint64_t ptp_ns;
  struct mt_pcap* pcap;
  int read_fd;
};

static uint64_t ut_pcap_ptp_time(struct mtl_main_impl* impl, enum mtl_port port) {
  MTL_MAY_UNUSED(port);
  return ((ut_pcap_ctx*)impl)->ptp_ns;
}

static int ut_pcap_port_id(uint16_t* port_id) {
  if (rte_eth_dev_get_port_by_name(UT_PCAP_VDEV, port_id) == 0) return 0;
  if (rte_eal_hotplug_add("vdev", UT_PCAP_VDEV, "") < 0) return -1;
  return rte_eth_dev_get_port_by_name(UT_PCAP_VDEV, port_id);
}

ut_pcap_ctx* ut_pcap_create(uint64_t ptp_ns) {
  uint16_t port_id;

  if (ut_eal_init() < 0 || ut_pcap_port_id(&port_id) < 0) return NULL;

  ut_pcap_ctx* ctx = calloc(1, sizeof(*ctx));
  if (!ctx) return NULL;
  ctx->impl.type = MT_HANDLE_MAIN;
  ctx->impl.tsc_hz = rte_get_tsc_hz();
  ctx->impl.user_para.num_ports = 1;
  ctx->impl.inf[MTL_PORT_P].parent = &ctx->impl;
  ctx->impl.inf[MTL_PORT_P].port = MTL_PORT_P;
  ctx->impl.inf[MTL_PORT_P].port_id = port_id;
  ctx->impl.inf[MTL_PORT_P].socket_id = (int)rte_socket_id();
  ctx->impl.inf[MTL_PORT_P].ptp_get_time_fn = ut_pcap_ptp_time;
  ctx->ptp_ns = ptp_ns;
  ctx->read_fd = -1;
  return ctx;
}

void ut_pcap_destroy(ut_pcap_ctx* ctx) {
  if (!ctx) return;
  if (ctx->pcap) mt_pcap_close(ctx->pcap);
  if (ctx->read_fd >= 0) close(ctx->read_fd);
  free(ctx);
}

int ut_pcap_open(ut_pcap_ctx* ctx) {
  FILE* file = tmpfile();
  if (!file) return -1;
  int fd = dup(fileno(file));
  ctx->read_fd = dup(fileno(file));
  (void)fclose(file);
  if (fd < 0 || ctx->read_fd < 0) {
    if (fd >= 0) close(fd);
    return -1;
  }

  ctx->pcap = mt_pcap_open(&ctx->impl, MTL_PORT_P, fd);
  if (!ctx->pcap) {
    close(fd);
    return -1;
  }
  return 0;
}

int ut_pcap_dump_one(ut_pcap_ctx* ctx) {
  struct rte_mbuf* pkt = rte_pktmbuf_alloc(ut_pool());
  if (!pkt) return -1;
  char* data = rte_pktmbuf_append(pkt, 64);
  if (!data) {
    rte_pktmbuf_free(pkt);
    return -1;
  }
  memset(data, 0, 64);

  int cnt = mt_pcap_dump(&ctx->impl, MTL_PORT_P, ctx->pcap, &pkt, 1);
  rte_pktmbuf_free(pkt);
  return cnt;
}

int ut_pcap_close_and_read_ts(ut_pcap_ctx* ctx, uint64_t* ts) {
  uint32_t hdr[2];
  uint32_t epb[3];
  off_t off = 0;

  mt_pcap_close(ctx->pcap);
  ctx->pcap = NULL;

  /* pcapng block: type, total length, then for an EPB interface id, ts high, ts low */
  while (pread(ctx->read_fd, hdr, sizeof(hdr), off) == sizeof(hdr) && hdr[1] >= 12) {
    if (hdr[0] == UT_PCAPNG_BLOCK_EPB) {
      if (pread(ctx->read_fd, epb, sizeof(epb), off + (off_t)sizeof(hdr)) != sizeof(epb))
        return -1;
      *ts = ((uint64_t)epb[1] << 32) | epb[2];
      return 0;
    }
    off += hdr[1];
  }
  return -1;
}
