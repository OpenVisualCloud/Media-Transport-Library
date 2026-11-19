/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Includes the production mt_cni.c so the file-local cni_rx_handle() is
 * reachable. Its non-static symbols are renamed so they do not shadow libmtl's,
 * and mt_ptp_parse() is renamed to a counting mock.
 */

#include <stdlib.h>

#undef MTL_HAS_USDT
#include "common/ut_common.h"

#define mt_cni_init ut_cnih_cni_init
#define mt_cni_uinit ut_cnih_cni_uinit
#define mt_cni_start ut_cnih_cni_start
#define mt_cni_stop ut_cnih_cni_stop
#define mt_csq_get ut_cnih_csq_get
#define mt_csq_put ut_cnih_csq_put
#define mt_csq_burst ut_cnih_csq_burst
#define mt_ptp_parse ut_cnih_ptp_parse_mock
#include "mt_cni.c"

#include "datapath/mt_cni_harness.h"

struct ut_cni_ctx {
  struct mtl_main_impl impl;
  struct mt_ptp_impl ptp;
  struct mt_cni_entry cni;
};

static int ut_g_ptp_parse_calls;

int ut_cnih_ptp_parse_mock(struct mt_ptp_impl* ptp, struct mt_ptp_header* hdr,
                           bool vlan, enum mt_ptp_l_mode mode, uint16_t timesync,
                           struct mt_ipv4_udp* ipv4_hdr) {
  (void)ptp;
  (void)hdr;
  (void)vlan;
  (void)mode;
  (void)timesync;
  (void)ipv4_hdr;
  ut_g_ptp_parse_calls++;
  return 0;
}

int ut_cni_eal_init(void) {
  return ut_eal_init();
}

ut_cni_ctx* ut_cni_create(bool ptp_instance) {
  ut_cni_ctx* ctx = calloc(1, sizeof(*ctx));
  if (!ctx) return NULL;

  ctx->impl.type = MT_HANDLE_MAIN;
  ctx->ptp.port = MTL_PORT_R;
  if (ptp_instance) ctx->impl.ptp[MTL_PORT_R] = &ctx->ptp;
  ctx->cni.impl = &ctx->impl;
  ctx->cni.port = MTL_PORT_R;
  ut_g_ptp_parse_calls = 0;
  return ctx;
}

void ut_cni_destroy(ut_cni_ctx* ctx) {
  free(ctx);
}

int ut_cni_rx_l2_ptp(ut_cni_ctx* ctx, bool vlan) {
  struct rte_mbuf* m = rte_pktmbuf_alloc(ut_pool());
  if (!m) return -ENOMEM;

  size_t hdr_len = sizeof(struct rte_ether_hdr);
  if (vlan) hdr_len += sizeof(struct rte_vlan_hdr);
  size_t len = hdr_len + sizeof(struct mt_ptp_sync_msg);
  uint8_t* p = (uint8_t*)rte_pktmbuf_append(m, len);
  if (!p) {
    rte_pktmbuf_free(m);
    return -ENOMEM;
  }
  memset(p, 0, len);

  struct rte_ether_hdr* eth = (struct rte_ether_hdr*)p;
  if (vlan) {
    struct rte_vlan_hdr* vh = (struct rte_vlan_hdr*)(eth + 1);
    eth->ether_type = htons(RTE_ETHER_TYPE_VLAN);
    vh->eth_proto = htons(RTE_ETHER_TYPE_1588);
  } else {
    eth->ether_type = htons(RTE_ETHER_TYPE_1588);
  }
  struct mt_ptp_header* ptp_hdr = (struct mt_ptp_header*)(p + hdr_len);
  ptp_hdr->message_type = PTP_SYNC;

  int ret = cni_rx_handle(&ctx->cni, m);
  rte_pktmbuf_free(m);
  return ret;
}

int ut_cni_ptp_parse_calls(void) {
  return ut_g_ptp_parse_calls;
}

uint64_t ut_cni_eth_rx_bytes(const ut_cni_ctx* ctx) {
  return ctx->cni.eth_rx_bytes;
}
