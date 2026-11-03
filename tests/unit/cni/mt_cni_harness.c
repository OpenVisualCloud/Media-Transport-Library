/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Includes the production mt_cni.c so the static cni_rx_handle() is callable, and
 * mt_dhcp.c so the lease timers an ACK starts can be cancelled.
 */

#include "cni/mt_cni_harness.h"

#include <stdlib.h>
#include <string.h>

#undef MTL_HAS_USDT
#include "common/ut_common.h"
#include "mt_cni.c"
#include "mt_dhcp.c"

_Static_assert(UT_CNI_DHCP_DISCOVERING == MT_DHCP_STATUS_DISCOVERING, "dhcp status");
_Static_assert(UT_CNI_DHCP_REQUESTING == MT_DHCP_STATUS_REQUESTING, "dhcp status");
_Static_assert(UT_CNI_DHCP_BOUND == MT_DHCP_STATUS_BOUND, "dhcp status");

struct ut_cni_ctx {
  struct mtl_main_impl impl;
  struct mt_cni_entry cni;
  struct mt_dp_impl dp;
  struct mt_arp_impl arp;
  struct mt_mcast_impl mcast;
  struct mt_dhcp_impl dhcp;
};

ut_cni_ctx* ut_cni_create(void) {
  if (ut_eal_init() < 0) return NULL;
  ut_cni_ctx* ctx = calloc(1, sizeof(*ctx));
  if (!ctx) return NULL;

  ctx->cni.impl = &ctx->impl;
  ctx->cni.port = MTL_PORT_P;
  MT_TAILQ_INIT(&ctx->cni.csq_queues);
  rte_spinlock_init(&ctx->cni.csq_lock);
  MT_TAILQ_INIT(&ctx->cni.udp_detect);

  const uint8_t sip[MTL_IP_ADDR_LEN] = UT_CNI_SIP_ADDR;
  const uint8_t peer[MTL_IP_ADDR_LEN] = UT_CNI_ARP_PEER_ADDR;
  memcpy(ctx->impl.user_para.sip_addr[MTL_PORT_P], sip, sizeof(sip));
  mt_pthread_mutex_init(&ctx->arp.mutex, NULL);
  memcpy(&ctx->arp.entries[0].ip, peer, sizeof(peer));
  ctx->arp.port = MTL_PORT_P;
  ctx->arp.parent = &ctx->impl;
  ctx->impl.arp[MTL_PORT_P] = &ctx->arp;

  mt_pthread_mutex_init(&ctx->mcast.group_mutex, NULL);
  TAILQ_INIT(&ctx->mcast.group_list);
  ctx->impl.mcast[MTL_PORT_P] = &ctx->mcast;

  /* the request the client answers an offer with finds no TX queue */
  ctx->impl.inf[MTL_PORT_P].tx_mbuf_pool = ut_pool();
  rte_spinlock_init(&ctx->dp.txq_sys_entry_lock);
  ctx->impl.dp[MTL_PORT_P] = &ctx->dp;

  mt_pthread_mutex_init(&ctx->dhcp.mutex, NULL);
  ctx->dhcp.status = MT_DHCP_STATUS_DISCOVERING;
  ctx->dhcp.xid = UT_CNI_DHCP_XID;
  ctx->dhcp.port = MTL_PORT_P;
  ctx->dhcp.parent = &ctx->impl;
  ctx->impl.dhcp[MTL_PORT_P] = &ctx->dhcp;
  return ctx;
}

void ut_cni_destroy(ut_cni_ctx* ctx) {
  rte_eal_alarm_cancel(dhcp_renew_handler, &ctx->dhcp);
  rte_eal_alarm_cancel(dhcp_rebind_handler, &ctx->dhcp);
  rte_eal_alarm_cancel(dhcp_lease_handler, &ctx->dhcp);
  mt_pthread_mutex_destroy(&ctx->dhcp.mutex);
  mt_pthread_mutex_destroy(&ctx->mcast.group_mutex);
  mt_pthread_mutex_destroy(&ctx->arp.mutex);
  free(ctx);
}

int ut_cni_rx(ut_cni_ctx* ctx, const uint8_t* frame, size_t len) {
  uint8_t* data = ut_guarded_copy(frame, len);
  struct rte_mbuf m;
  memset(&m, 0, sizeof(m));
  m.buf_addr = data;
  m.data_len = len;
  m.pkt_len = len;
  m.nb_segs = 1;
  int ret = cni_rx_handle(&ctx->cni, &m);
  ut_guarded_free(data, len);
  return ret;
}

bool ut_cni_arp_resolved(ut_cni_ctx* ctx) {
  return rte_atomic32_read(&ctx->arp.entries[0].mac_ready);
}

bool ut_cni_igmp_query_seen(ut_cni_ctx* ctx) {
  return ctx->mcast.has_external_query;
}

int ut_cni_dhcp_status(ut_cni_ctx* ctx) {
  return ctx->dhcp.status;
}

const uint8_t* ut_cni_dhcp_server_ip(ut_cni_ctx* ctx) {
  return ctx->dhcp.server_ip;
}

const uint8_t* ut_cni_dhcp_netmask(ut_cni_ctx* ctx) {
  return ctx->dhcp.netmask;
}
