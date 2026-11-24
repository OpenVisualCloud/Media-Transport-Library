/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * mt_shared_rss.c with no RX: its traffic thread would hit DPDK's stack-dumping stub.
 */

#undef MTL_HAS_USDT
#include "mt_main.h"

static uint16_t ut_srss_rx_burst(uint16_t port_id, uint16_t queue_id,
                                 struct rte_mbuf** rx_pkts, const uint16_t nb_pkts) {
  MTL_MAY_UNUSED(port_id);
  MTL_MAY_UNUSED(queue_id);
  MTL_MAY_UNUSED(rx_pkts);
  MTL_MAY_UNUSED(nb_pkts);
  return 0;
}

#define rte_eth_rx_burst ut_srss_rx_burst
#include "datapath/mt_shared_rss.c"
#undef rte_eth_rx_burst
