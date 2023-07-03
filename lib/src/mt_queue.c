/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#include "mt_queue.h"

#include "mt_log.h"

struct mt_rxq_entry* mt_rxq_get(struct mtl_main_impl* impl, enum mtl_port port,
                                struct mt_rxq_flow* flow) {
  struct mt_rxq_entry* entry =
      mt_rte_zmalloc_socket(sizeof(*entry), mt_socket_id(impl, port));
  if (!entry) {
    err("%s(%d), entry malloc fail\n", __func__, port);
    return NULL;
  }
  entry->parent = impl;

  if (mt_has_srss(impl, port)) {
    entry->srss = mt_srss_get(impl, port, flow);
    if (!entry->srss) goto fail;
    entry->queue_id = mt_srss_queue_id(entry->srss);
  } else if (mt_shared_queue(impl, port)) {
    entry->rsq = mt_rsq_get(impl, port, flow);
    if (!entry->rsq) goto fail;
    entry->queue_id = mt_rsq_queue_id(entry->rsq);
  } else {
    entry->rxq = mt_dev_get_rx_queue(impl, port, flow);
    if (!entry->rxq) goto fail;
    entry->queue_id = mt_dev_rx_queue_id(entry->rxq);
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
  mt_rte_free(entry);
  return 0;
}

uint16_t mt_rxq_burst(struct mt_rxq_entry* entry, struct rte_mbuf** rx_pkts,
                      const uint16_t nb_pkts) {
  uint16_t rx;
  if (entry->srss) {
    rx = 0; /* srss rx on the srss tasklet */
  } else if (entry->rsq) {
    mt_rsq_burst(entry->rsq, nb_pkts);
    rx = 0; /* the buf is handled in the callback */
  } else {
    rx = mt_dev_rx_burst(entry->rxq, rx_pkts, nb_pkts);
  }

  return rx;
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

  if (mt_shared_queue(impl, port)) {
    entry->tsq = mt_tsq_get(impl, port, flow);
    if (!entry->tsq) goto fail;
    entry->queue_id = mt_tsq_queue_id(entry->tsq);
  } else {
    entry->txq = mt_dev_get_tx_queue(impl, port, flow);
    if (!entry->txq) goto fail;
    entry->queue_id = mt_dev_tx_queue_id(entry->txq);
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

int mt_txq_flush(struct mt_txq_entry* entry, struct rte_mbuf* pad) {
  if (entry->tsq)
    return mt_tsq_flush(entry->parent, entry->tsq, pad);
  else
    return mt_dev_flush_tx_queue(entry->parent, entry->txq, pad);
}

uint16_t mt_txq_burst(struct mt_txq_entry* entry, struct rte_mbuf** tx_pkts,
                      uint16_t nb_pkts) {
  if (entry->tsq)
    return mt_tsq_burst(entry->tsq, tx_pkts, nb_pkts);
  else
    return mt_dev_tx_burst(entry->txq, tx_pkts, nb_pkts);
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