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
    entry->queue_id = 0; /* not known the queue id */
  } else if (mt_has_rss(impl, port)) {
    entry->rss = mt_rss_get(impl, port, flow);
    if (!entry->rss) goto fail;
    entry->queue_id = mt_rss_queue_id(entry->rss);
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
  if (entry->rss) {
    mt_rss_put(entry->rss);
    entry->rss = NULL;
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
  } else if (entry->rss) {
    mt_rss_burst(entry->rss, nb_pkts);
    rx = 0; /* the buf is handled in the callback */
  } else {
    rx = mt_dev_rx_burst(entry->rxq, rx_pkts, nb_pkts);
  }

  return rx;
}
