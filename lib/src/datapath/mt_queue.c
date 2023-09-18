/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#include "mt_queue.h"

#include "../mt_cni.h"
#include "../mt_dev.h"
#include "../mt_log.h"
#include "mt_shared_queue.h"
#include "mt_shared_rss.h"

static uint16_t rx_socket_burst(struct mt_rxq_entry* entry, struct rte_mbuf** rx_pkts,
                                const uint16_t nb_pkts) {
  MTL_MAY_UNUSED(entry);
  MTL_MAY_UNUSED(rx_pkts);
  MTL_MAY_UNUSED(nb_pkts);
  /* todo */
  return 0;
}

static uint16_t rx_srss_burst(struct mt_rxq_entry* entry, struct rte_mbuf** rx_pkts,
                              const uint16_t nb_pkts) {
  return mt_srss_burst(entry->srss, rx_pkts, nb_pkts);
}

static uint16_t rx_rsq_burst(struct mt_rxq_entry* entry, struct rte_mbuf** rx_pkts,
                             const uint16_t nb_pkts) {
  return mt_rsq_burst(entry->rsq, rx_pkts, nb_pkts);
}

static uint16_t rx_csq_burst(struct mt_rxq_entry* entry, struct rte_mbuf** rx_pkts,
                             const uint16_t nb_pkts) {
  return mt_csq_burst(entry->csq, rx_pkts, nb_pkts);
}

static uint16_t rx_dpdk_burst(struct mt_rxq_entry* entry, struct rte_mbuf** rx_pkts,
                              const uint16_t nb_pkts) {
  return mt_dpdk_rx_burst(entry->rxq, rx_pkts, nb_pkts);
}

struct mt_rxq_entry* mt_rxq_get(struct mtl_main_impl* impl, enum mtl_port port,
                                struct mt_rxq_flow* flow) {
  struct mt_rxq_entry* entry =
      mt_rte_zmalloc_socket(sizeof(*entry), mt_socket_id(impl, port));
  if (!entry) {
    err("%s(%d), entry malloc fail\n", __func__, port);
    return NULL;
  }
  entry->parent = impl;

  if (mt_pmd_is_kernel_socket(impl, port)) {
    entry->queue_id = 0;
    entry->burst = rx_socket_burst;
  } else if (mt_has_srss(impl, port)) {
    entry->srss = mt_srss_get(impl, port, flow);
    if (!entry->srss) goto fail;
    entry->queue_id = mt_srss_queue_id(entry->srss);
    entry->burst = rx_srss_burst;
  } else if (mt_shared_rx_queue(impl, port)) {
    entry->rsq = mt_rsq_get(impl, port, flow);
    if (!entry->rsq) goto fail;
    entry->queue_id = mt_rsq_queue_id(entry->rsq);
    entry->burst = rx_rsq_burst;
  } else if (flow->use_cni_queue) {
    entry->csq = mt_csq_get(impl, port, flow);
    if (!entry->csq) goto fail;
    entry->queue_id = mt_csq_queue_id(entry->csq);
    entry->burst = rx_csq_burst;
  } else {
    entry->rxq = mt_dev_get_rx_queue(impl, port, flow);
    if (!entry->rxq) goto fail;
    entry->queue_id = mt_dev_rx_queue_id(entry->rxq);
    entry->burst = rx_dpdk_burst;
  }

  return entry;

fail:
  mt_rxq_put(entry);
  return NULL;
}

int mt_rxq_put(struct mt_rxq_entry* entry) {
  if (entry->rxq) {
    mt_dev_put_rx_queue(entry->parent, entry->rxq);
    entry->rxq = NULL;
  }
  if (entry->rsq) {
    mt_rsq_put(entry->rsq);
    entry->rsq = NULL;
  }
  if (entry->srss) {
    mt_srss_put(entry->srss);
    entry->srss = NULL;
  }
  if (entry->csq) {
    mt_csq_put(entry->csq);
    entry->csq = NULL;
  }
  mt_rte_free(entry);
  return 0;
}

uint16_t mt_rxq_burst(struct mt_rxq_entry* entry, struct rte_mbuf** rx_pkts,
                      const uint16_t nb_pkts) {
  return entry->burst(entry, rx_pkts, nb_pkts);
}

static uint16_t tx_socket_burst(struct mt_txq_entry* entry, struct rte_mbuf** tx_pkts,
                                uint16_t nb_pkts) {
  MTL_MAY_UNUSED(entry);
  /* todo */
  rte_pktmbuf_free_bulk(tx_pkts, nb_pkts);
  return nb_pkts;
}

static uint16_t tx_tsq_burst(struct mt_txq_entry* entry, struct rte_mbuf** tx_pkts,
                             uint16_t nb_pkts) {
  return mt_tsq_burst(entry->tsq, tx_pkts, nb_pkts);
}

static uint16_t tx_dpdk_burst(struct mt_txq_entry* entry, struct rte_mbuf** tx_pkts,
                              uint16_t nb_pkts) {
  return mt_dpdk_tx_burst(entry->txq, tx_pkts, nb_pkts);
}

struct mt_txq_entry* mt_txq_get(struct mtl_main_impl* impl, enum mtl_port port,
                                struct mt_txq_flow* flow) {
  struct mt_txq_entry* entry =
      mt_rte_zmalloc_socket(sizeof(*entry), mt_socket_id(impl, port));
  if (!entry) {
    err("%s(%d), entry malloc fail\n", __func__, port);
    return NULL;
  }
  entry->parent = impl;

  if (mt_pmd_is_kernel_socket(impl, port)) {
    entry->queue_id = 0;
    entry->burst = tx_socket_burst;
  } else if (mt_shared_tx_queue(impl, port)) {
    entry->tsq = mt_tsq_get(impl, port, flow);
    if (!entry->tsq) goto fail;
    entry->queue_id = mt_tsq_queue_id(entry->tsq);
    entry->burst = tx_tsq_burst;
  } else {
    entry->txq = mt_dev_get_tx_queue(impl, port, flow);
    if (!entry->txq) goto fail;
    entry->queue_id = mt_dev_tx_queue_id(entry->txq);
    entry->burst = tx_dpdk_burst;
  }

  return entry;

fail:
  mt_txq_put(entry);
  return NULL;
}

int mt_txq_put(struct mt_txq_entry* entry) {
  if (entry->txq) {
    mt_dev_put_tx_queue(entry->parent, entry->txq);
    entry->txq = NULL;
  }
  if (entry->tsq) {
    mt_tsq_put(entry->tsq);
    entry->tsq = NULL;
  }
  mt_rte_free(entry);
  return 0;
}

int mt_txq_fatal_error(struct mt_txq_entry* entry) {
  if (entry->txq) mt_dev_tx_queue_fatal_error(entry->parent, entry->txq);
  if (entry->tsq) mt_tsq_fatal_error(entry->tsq);
  return 0;
}

int mt_txq_done_cleanup(struct mt_txq_entry* entry) {
  if (entry->txq) mt_dev_tx_done_cleanup(entry->parent, entry->txq);
  if (entry->tsq) mt_tsq_done_cleanup(entry->tsq);
  return 0;
}

int mt_txq_flush(struct mt_txq_entry* entry, struct rte_mbuf* pad) {
  if (entry->tsq)
    return mt_tsq_flush(entry->parent, entry->tsq, pad);
  else if (entry->txq)
    return mt_dpdk_flush_tx_queue(entry->parent, entry->txq, pad);
  else
    return 0;
}

uint16_t mt_txq_burst(struct mt_txq_entry* entry, struct rte_mbuf** tx_pkts,
                      uint16_t nb_pkts) {
  return entry->burst(entry, tx_pkts, nb_pkts);
}

uint16_t mt_txq_burst_busy(struct mt_txq_entry* entry, struct rte_mbuf** tx_pkts,
                           uint16_t nb_pkts, int timeout_ms) {
  uint16_t sent = 0;
  struct mtl_main_impl* impl = entry->parent;
  uint64_t start_ts = mt_get_tsc(impl);

  /* Send this vector with busy looping */
  while (sent < nb_pkts) {
    if (timeout_ms > 0) {
      int ms = (mt_get_tsc(impl) - start_ts) / NS_PER_MS;
      if (ms > timeout_ms) {
        warn("%s(%u), fail as timeout to %d ms\n", __func__, entry->queue_id, timeout_ms);
        return sent;
      }
    }
    sent += mt_txq_burst(entry, &tx_pkts[sent], nb_pkts - sent);
  }

  return sent;
}

int mt_dp_queue_init(struct mtl_main_impl* impl) {
  int ret;

  ret = mt_srss_init(impl);
  if (ret < 0) {
    err("%s, srss init fail %d\n", __func__, ret);
    return ret;
  }

  ret = mt_rsq_init(impl);
  if (ret < 0) {
    err("%s, rsq init fail %d\n", __func__, ret);
    return ret;
  }
  ret = mt_tsq_init(impl);
  if (ret < 0) {
    err("%s, tsq init fail %d\n", __func__, ret);
    return ret;
  }

  return 0;
}

int mt_dp_queue_uinit(struct mtl_main_impl* impl) {
  mt_rsq_uinit(impl);
  mt_tsq_uinit(impl);
  mt_srss_uinit(impl);
  return 0;
}