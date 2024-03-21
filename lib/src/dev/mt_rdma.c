/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Intel Corporation
 * The data path based on rdma interface
 */

#include "mt_rdma.h"

#include <rdma/rdma_cma.h>
#include <rdma/rdma_verbs.h>
#include <stdint.h>

#include "../mt_log.h"
#include "../mt_stat.h"

#define MT_RDMA_MAX_WR (2048)

struct mt_rdma_tx_queue {
  enum mtl_port port;
  struct rte_mempool* mbuf_pool;
  uint16_t q;
  uint8_t* sip;

  struct rdma_event_channel* ec;
  struct rdma_cm_id* cma_id;
  struct ibv_pd* pd;
  struct ibv_cq* cq;
  struct ibv_ah* ah;
  uint32_t remote_qpn;
  struct rdma_addrinfo* rai;
  struct ibv_mr* send_mr;
  void* send_buffer;
  size_t send_buffer_size;
  bool connected;
  pthread_t connect_thread;
  uint16_t outstanding_wr;

  struct mt_tx_rdma_entry* tx_entry;

  uint64_t stat_tx_pkts;
  uint64_t stat_tx_bytes;
  uint64_t stat_tx_free;
  uint64_t stat_tx_submit;
  uint64_t stat_tx_copy;
  uint64_t stat_tx_mbuf_alloc_fail;
  uint64_t stat_tx_post_send_fail;
  uint64_t stat_tx_prod_full;
  uint64_t stat_tx_completion_fail;
};

struct mt_rdma_rx_queue {
  enum mtl_port port;
  struct rte_mempool* mbuf_pool;
  uint16_t q;
  uint8_t* sip;

  struct rdma_event_channel* ec;
  struct rdma_cm_id* listen_id;
  struct rdma_cm_id* cma_id;
  struct ibv_pd* pd;
  struct ibv_cq* cq;
  struct ibv_qp* qp;
  struct ibv_mr* recv_mr;
  void* recv_buffer;
  size_t recv_len;
  size_t recv_buffer_size;
  bool connected;
  pthread_t connect_thread;

  struct mt_rx_rdma_entry* rx_entry;
  uint64_t stat_rx_pkts;
  uint64_t stat_rx_bytes;
  uint64_t stat_rx_burst;
  uint64_t stat_rx_mbuf_alloc_fail;
  uint64_t stat_rx_post_recv_fail;

  uint32_t stat_rx_pkt_invalid;
  uint32_t stat_rx_pkt_err_udp_port;
};

struct mt_rdma_priv {
  struct mtl_main_impl* parent;
  enum mtl_port port;

  uint16_t tx_queues_cnt;
  uint16_t rx_queues_cnt;
  struct mt_rdma_tx_queue* tx_queues;
  struct mt_rdma_rx_queue* rx_queues;
  pthread_mutex_t queues_lock;
};

static int rdma_queue_tx_stat(struct mt_rdma_tx_queue* txq) {
  enum mtl_port port = txq->port;
  uint16_t q = txq->q;

  notice("%s(%d,%u), pkts %" PRIu64 " bytes %" PRIu64 " submit %" PRIu64 " free %" PRIu64
         "\n",
         __func__, port, q, txq->stat_tx_pkts, txq->stat_tx_bytes, txq->stat_tx_submit,
         txq->stat_tx_free);
  txq->stat_tx_pkts = 0;
  txq->stat_tx_bytes = 0;
  txq->stat_tx_submit = 0;
  txq->stat_tx_free = 0;
  if (txq->stat_tx_copy) {
    notice("%s(%d,%u), pkts copy %" PRIu64 "\n", __func__, port, q, txq->stat_tx_copy);
    txq->stat_tx_copy = 0;
  }

  if (txq->stat_tx_mbuf_alloc_fail) {
    warn("%s(%d,%u), mbuf alloc fail %" PRIu64 "\n", __func__, port, q,
         txq->stat_tx_mbuf_alloc_fail);
    txq->stat_tx_mbuf_alloc_fail = 0;
  }
  if (txq->stat_tx_prod_full) {
    info("%s(%d,%u), tx prod full %" PRIu64 "\n", __func__, port, q,
         txq->stat_tx_prod_full);
    txq->stat_tx_prod_full = 0;
  }
  if (txq->stat_tx_post_send_fail) {
    err("%s(%d,%u), post send fail %" PRIu64 "\n", __func__, port, q,
        txq->stat_tx_post_send_fail);
    txq->stat_tx_post_send_fail = 0;
  }
  if (txq->stat_tx_completion_fail) {
    err("%s(%d,%u), completion fail %" PRIu64 "\n", __func__, port, q,
        txq->stat_tx_completion_fail);
    txq->stat_tx_completion_fail = 0;
  }
  return 0;
}

static int rdma_queue_rx_stat(struct mt_rdma_rx_queue* rxq) {
  enum mtl_port port = rxq->port;
  uint16_t q = rxq->q;

  notice("%s(%d,%u), pkts %" PRIu64 " bytes %" PRIu64 " burst %" PRIu64 "\n", __func__,
         port, q, rxq->stat_rx_pkts, rxq->stat_rx_bytes, rxq->stat_rx_burst);
  rxq->stat_rx_pkts = 0;
  rxq->stat_rx_bytes = 0;
  rxq->stat_rx_burst = 0;

  if (rxq->stat_rx_mbuf_alloc_fail) {
    warn("%s(%d,%u), mbuf alloc fail %" PRIu64 "\n", __func__, port, q,
         rxq->stat_rx_mbuf_alloc_fail);
    rxq->stat_rx_mbuf_alloc_fail = 0;
  }
  if (rxq->stat_rx_post_recv_fail) {
    err("%s(%d,%u), prod reserve fail %" PRIu64 "\n", __func__, port, q,
        rxq->stat_rx_post_recv_fail);
    rxq->stat_rx_post_recv_fail = 0;
  }
  if (rxq->stat_rx_pkt_invalid) {
    err("%s(%d,%u), invalid pkt %u wrong udp port %u\n", __func__, port, q,
        rxq->stat_rx_pkt_invalid, rxq->stat_rx_pkt_err_udp_port);
    rxq->stat_rx_pkt_invalid = 0;
    rxq->stat_rx_pkt_err_udp_port = 0;
  }

  return 0;
}

static int rdma_stat_dump(void* priv) {
  struct mt_rdma_priv* rdma = priv;

  for (uint16_t i = 0; i < rdma->tx_queues_cnt; i++) {
    struct mt_rdma_tx_queue* txq = &rdma->tx_queues[i];
    if (txq->tx_entry) rdma_queue_tx_stat(txq);
  }

  for (uint16_t i = 0; i < rdma->rx_queues_cnt; i++) {
    struct mt_rdma_rx_queue* rxq = &rdma->rx_queues[i];
    if (rxq->rx_entry) rdma_queue_rx_stat(rxq);
  }

  return 0;
}

static int rdma_free(struct mt_rdma_priv* rdma) {
  enum mtl_port port = rdma->port;

  if (rdma->tx_queues) {
    for (uint16_t i = 0; i < rdma->tx_queues_cnt; i++) {
      struct mt_rdma_tx_queue* txq = &rdma->tx_queues[i];

      if (txq->tx_entry) {
        warn("%s(%d,%u), tx_entry still active\n", __func__, port, txq->q);
        mt_tx_rdma_put(txq->tx_entry);
      }
    }
    mt_rte_free(rdma->tx_queues);
    rdma->tx_queues = NULL;
  }

  if (rdma->rx_queues) {
    for (uint16_t i = 0; i < rdma->rx_queues_cnt; i++) {
      struct mt_rdma_rx_queue* rxq = &rdma->rx_queues[i];

      if (rxq->rx_entry) {
        warn("%s(%d,%u), rx_entry still active\n", __func__, port, rxq->q);
        mt_rx_rdma_put(rxq->rx_entry);
      }
    }
    mt_rte_free(rdma->rx_queues);
    rdma->rx_queues = NULL;
  }

  mt_pthread_mutex_destroy(&rdma->queues_lock);
  mt_rte_free(rdma);
  return 0;
}

static int rdma_rx_post_recv(struct mt_rdma_rx_queue* rxq, struct rte_mbuf** mbufs,
                             uint16_t sz) {
  enum mtl_port port = rxq->port;
  uint16_t q = rxq->q;
  int ret;
  struct rte_mbuf* m = NULL;

  for (int i = 0; i < sz; i++) {
    m = mbufs[i];
    void* addr = rte_pktmbuf_mtod(m, void*) - sizeof(struct ibv_grh);
    ret = rdma_post_recv(rxq->cma_id, m, addr, rxq->recv_len, rxq->recv_mr);
    if (ret) {
      rxq->stat_rx_post_recv_fail++;
      err("%s(%d,%u), rdma_post_recv %u fail %d\n", __func__, port, q, i, ret);
      return ret;
    }
  }

  return 0;
}

static inline uintptr_t rdma_mp_base_addr(struct rte_mempool* mp, uint64_t* align) {
  struct rte_mempool_memhdr* hdr;
  uintptr_t hdr_addr, aligned_addr;

  hdr = STAILQ_FIRST(&mp->mem_list);
  hdr_addr = (uintptr_t)hdr->addr;
  aligned_addr = hdr_addr & ~(getpagesize() - 1);
  *align = hdr_addr - aligned_addr;

  return aligned_addr;
}

static void rdma_tx_poll_done(struct mt_rdma_tx_queue* txq) {
  if (!txq->connected) return;
  struct ibv_cq* cq = txq->cq;
  struct ibv_wc wc[32];
  int n = ibv_poll_cq(cq, 32, wc);

  for (int i = 0; i < n; i++) {
    if (wc[i].opcode == IBV_WC_SEND && wc[i].status == IBV_WC_SUCCESS) {
      /* success */
    } else {
      txq->stat_tx_completion_fail++;
    }
    rte_pktmbuf_free((struct rte_mbuf*)wc[i].wr_id);
  }
  txq->outstanding_wr -= n;
  txq->stat_tx_free += n;
}

static uint16_t rdma_tx(struct mtl_main_impl* impl, struct mt_rdma_tx_queue* txq,
                        struct rte_mbuf** tx_pkts, uint16_t nb_pkts) {
  if (!txq->connected) return 0;
  enum mtl_port port = txq->port;
  // uint16_t q = txq->q;
  struct rte_mempool* mbuf_pool = txq->mbuf_pool;
  uint16_t tx = 0;
  struct mtl_port_status* stats = mt_if(impl, port)->dev_stats_sw;
  uint64_t tx_bytes = 0;

  rdma_tx_poll_done(txq);

  uint16_t wr_free = MT_RDMA_MAX_WR - txq->outstanding_wr;
  if (wr_free < nb_pkts) { /* tx wr is full */
    txq->stat_tx_prod_full++;
    return 0;
  }

  for (uint16_t i = 0; i < nb_pkts; i++) {
    struct rte_mbuf* m = tx_pkts[i];
    struct rte_mbuf* local = rte_pktmbuf_alloc(mbuf_pool);
    if (!local) {
      dbg("%s(%d, %u), local mbuf alloc fail\n", __func__, port, q);
      txq->stat_tx_mbuf_alloc_fail++;
      goto exit;
    }
    void* pkt = rte_pktmbuf_mtod(local, void*);
    void* data = pkt;
    struct rte_mbuf* n = m;
    uint16_t nb_segs = m->nb_segs;
    for (uint16_t seg = 0; seg < nb_segs; seg++) {
      rte_memcpy(data, rte_pktmbuf_mtod(n, void*), n->data_len);
      data += n->data_len;
      /* point to next */
      n = n->next;
    }

    if (rdma_post_ud_send(txq->cma_id, local, pkt, m->pkt_len, txq->send_mr,
                          IBV_SEND_SIGNALED, txq->ah, txq->remote_qpn)) {
      dbg("%s(%d, %u), post send fail\n", __func__, port, q);
      txq->stat_tx_post_send_fail++;
      goto exit;
    }
    txq->outstanding_wr++;

    tx_bytes += m->pkt_len;
    rte_pktmbuf_free(m);
    dbg("%s(%d, %u), tx local mbuf %p umem pkt %p addr 0x%" PRIu64 "\n", __func__, port,
        q, local, pkt, addr);
    txq->stat_tx_copy++;
    tx++;
  }

exit:
  if (tx) {
    dbg("%s(%d, %u), submit %u\n", __func__, port, q, tx);
    if (stats) {
      stats->tx_packets += tx;
      stats->tx_bytes += tx_bytes;
    }
    txq->stat_tx_submit++;
    txq->stat_tx_pkts += tx;
    txq->stat_tx_bytes += tx_bytes;

  } else {
    rdma_tx_poll_done(txq);
  }
  return tx;
}

static uint16_t rdma_rx(struct mt_rx_rdma_entry* entry, struct rte_mbuf** rx_pkts,
                        uint16_t nb_pkts) {
  if (!entry->rxq->connected) return 0;
  struct mt_rdma_rx_queue* rxq = entry->rxq;
  enum mtl_port port = entry->port;
  // uint16_t q = rxq->q;

  struct mtl_port_status* stats = mt_if(entry->parent, port)->dev_stats_sw;
  uint64_t rx_bytes = 0;
  struct ibv_wc wc[nb_pkts];
  int rx = ibv_poll_cq(rxq->cq, nb_pkts, wc);
  if (rx <= 0) return 0;

  rxq->stat_rx_burst++;

  struct rte_mbuf* fill[rx];
  int ret = rte_pktmbuf_alloc_bulk(rxq->mbuf_pool, fill, rx);
  if (ret < 0) {
    dbg("%s(%d, %u), mbuf alloc bulk %u fail\n", __func__, port, q, rx);
    rxq->stat_rx_mbuf_alloc_fail++;
    return 0;
  }

  struct rte_mbuf* pkt = NULL;
  for (int i = 0; i < rx; i++) {
    pkt = (struct rte_mbuf*)wc[i].wr_id;
    uint32_t len = wc[i].byte_len - sizeof(struct ibv_grh);
    rte_pktmbuf_pkt_len(pkt) = len;
    rte_pktmbuf_data_len(pkt) = len;
    rx_pkts[i] = pkt;
    rx_bytes += len;
  }

  /* post recv */
  rdma_rx_post_recv(rxq, fill, rx);

  if (stats) {
    stats->rx_packets += rx;
    stats->rx_bytes += rx_bytes;
  }
  rxq->stat_rx_pkts += rx;
  rxq->stat_rx_bytes += rx_bytes;

  return rx;
}

int mt_dev_rdma_init(struct mt_interface* inf) {
  struct mtl_main_impl* impl = inf->parent;
  enum mtl_port port = inf->port;
  int ret;

  if (!mt_pmd_is_rdma_ud(impl, port)) {
    err("%s(%d), not rdma\n", __func__, port);
    return -EIO;
  }

  struct mt_rdma_priv* rdma =
      mt_rte_zmalloc_socket(sizeof(*rdma), mt_socket_id(impl, port));
  if (!rdma) {
    err("%s(%d), rdma malloc fail\n", __func__, port);
    return -ENOMEM;
  }
  rdma->parent = impl;
  rdma->port = port;
  rdma->tx_queues_cnt = inf->nb_tx_q;
  rdma->rx_queues_cnt = inf->nb_rx_q;
  mt_pthread_mutex_init(&rdma->queues_lock, NULL);
  if (rdma->tx_queues_cnt) {
    rdma->tx_queues = mt_rte_zmalloc_socket(
        sizeof(*rdma->tx_queues) * rdma->tx_queues_cnt, mt_socket_id(impl, port));
    if (!rdma->tx_queues) {
      err("%s(%d), rdma tx_queues malloc fail\n", __func__, port);
      rdma_free(rdma);
      return -ENOMEM;
    }
  }
  if (rdma->rx_queues_cnt) {
    rdma->rx_queues = mt_rte_zmalloc_socket(
        sizeof(*rdma->rx_queues) * rdma->rx_queues_cnt, mt_socket_id(impl, port));
    if (!rdma->rx_queues) {
      err("%s(%d), rdma rx_queues malloc fail\n", __func__, port);
      rdma_free(rdma);
      return -ENOMEM;
    }
  }

  for (uint16_t i = 0; i < rdma->tx_queues_cnt; i++) {
    struct mt_rdma_tx_queue* txq = &rdma->tx_queues[i];
    txq->sip = mt_sip_addr(impl, port);
    txq->port = port;
    txq->q = i;
    txq->mbuf_pool = inf->rx_queues[i].mbuf_pool;
    if (!txq->mbuf_pool) {
      err("%s(%d), no mbuf_pool for txq %u\n", __func__, port, i);
      rdma_free(rdma);
      return -EIO;
    }
  }

  for (uint16_t i = 0; i < rdma->rx_queues_cnt; i++) {
    struct mt_rdma_rx_queue* rxq = &rdma->rx_queues[i];
    rxq->sip = mt_sip_addr(impl, port);
    rxq->port = port;
    rxq->q = i;
    rxq->mbuf_pool = inf->rx_queues[i].mbuf_pool;
    if (!rxq->mbuf_pool) {
      err("%s(%d), no mbuf_pool for rxq %u\n", __func__, port, i);
      rdma_free(rdma);
      return -EIO;
    }
  }

  ret = mt_stat_register(impl, rdma_stat_dump, rdma, "rdma");
  if (ret < 0) {
    err("%s(%d), stat register fail %d\n", __func__, port, ret);
    rdma_free(rdma);
    return ret;
  }

  inf->port_id = inf->port;
  inf->rdma = rdma;
  inf->feature |= MT_IF_FEATURE_TX_MULTI_SEGS;
  info("%s(%d) succ\n", __func__, port);
  return 0;
}

int mt_dev_rdma_uinit(struct mt_interface* inf) {
  struct mt_rdma_priv* rdma = inf->rdma;
  if (!rdma) return 0;
  struct mtl_main_impl* impl = inf->parent;

  mt_stat_unregister(impl, rdma_stat_dump, rdma);

  rdma_free(rdma);
  inf->rdma = NULL;
  dbg("%s(%d), succ\n", __func__, inf->port);
  return 0;
}

static int rdma_tx_mr_init(struct mt_rdma_tx_queue* txq) {
  void* base_addr = NULL;
  struct rte_mempool* pool = txq->mbuf_pool;
  size_t mr_size, align = 0;

  base_addr = (void*)rdma_mp_base_addr(pool, &align);
  mr_size = (size_t)pool->populated_size *
                (size_t)rte_mempool_calc_obj_size(pool->elt_size, pool->flags, NULL) +
            align;
  txq->send_mr = ibv_reg_mr(txq->pd, base_addr, mr_size, IBV_ACCESS_LOCAL_WRITE);
  if (!txq->send_mr) {
    err("%s(%d,%u), ibv_reg_mr fail\n", __func__, txq->port, txq->q);
    return -ENOMEM;
  }
  return 0;
}

static void* rdma_tx_connect_thread(void* arg) {
  struct mt_rdma_tx_queue* txq = arg;
  enum mtl_port port = txq->port;
  uint16_t q = txq->q;
  struct rdma_cm_event* event;
  struct pollfd pfd;
  pfd.fd = txq->ec->fd;
  pfd.events = POLLIN;

  info("%s(%d, %u), start\n", __func__, port, q);
  while (!txq->connected) {
    int ret = poll(&pfd, 1, 200);
    if (ret > 0) {
      ret = rdma_get_cm_event(txq->ec, &event);
      if (!ret) {
        switch (event->event) {
          case RDMA_CM_EVENT_ADDR_RESOLVED:
            ret = rdma_resolve_route(txq->cma_id, 2000);
            if (ret) {
              err("%s(%d, %u), rdma_resolve_route fail\n", __func__, port, q);
              goto connect_err;
            }
            break;
          case RDMA_CM_EVENT_ROUTE_RESOLVED:
            dbg("%s(%d, %u), route resolved\n", __func__, port, q);
            struct rdma_conn_param conn_param = {};
            txq->pd = ibv_alloc_pd(txq->cma_id->verbs);
            if (!txq->pd) {
              ret = -ENOMEM;
              err("%s(%d, %u), ibv_alloc_pd fail\n", __func__, port, q);
              goto connect_err;
            }

            txq->cq = ibv_create_cq(txq->cma_id->verbs, MT_RDMA_MAX_WR, txq, NULL, 0);
            if (!txq->cq) {
              err("%s(%d, %u), ibv_create_cq fail\n", __func__, port, q);
              goto connect_err;
            }

            struct ibv_qp_init_attr init_qp_attr = {};
            init_qp_attr.cap.max_send_wr = MT_RDMA_MAX_WR;
            init_qp_attr.cap.max_recv_wr = 1;
            init_qp_attr.cap.max_send_sge = 1;
            init_qp_attr.cap.max_recv_sge = 1;
            init_qp_attr.qp_context = txq;
            init_qp_attr.send_cq = txq->cq;
            init_qp_attr.recv_cq = txq->cq;
            init_qp_attr.qp_type = IBV_QPT_UD;
            init_qp_attr.sq_sig_all = 0;
            ret = rdma_create_qp(txq->cma_id, txq->pd, &init_qp_attr);
            if (ret) {
              err("%s(%d, %u), rdma_create_qp fail %d\n", __func__, port, q, ret);
              goto connect_err;
            }

            ret = rdma_tx_mr_init(txq);
            if (ret) {
              err("%s(%d, %u), rdma_tx_mr_init fail %d\n", __func__, port, q, ret);
              goto connect_err;
            }

            conn_param.private_data = txq->rai->ai_connect;
            conn_param.private_data_len = txq->rai->ai_connect_len;
            ret = rdma_connect(txq->cma_id, &conn_param);
            if (ret) {
              err("%s(%d, %u), rdma connect fail %d\n", __func__, port, q, ret);
              goto connect_err;
            }
            break;
          case RDMA_CM_EVENT_ESTABLISHED:
            dbg("%s(%d, %u), rdma connection established\n", __func__, port, q);
            txq->remote_qpn = event->param.ud.qp_num;
            txq->ah = ibv_create_ah(txq->pd, &event->param.ud.ah_attr);
            if (!txq->ah) {
              err("%s(%d, %u), ibv_create_ah fail\n", __func__, port, q);
              goto connect_err;
            }
            info("%s(%d, %u), rdma connected\n", __func__, port, q);
            txq->connected = true;
            break;
          case RDMA_CM_EVENT_ADDR_ERROR:
          case RDMA_CM_EVENT_ROUTE_ERROR:
          case RDMA_CM_EVENT_CONNECT_ERROR:
          case RDMA_CM_EVENT_UNREACHABLE:
          case RDMA_CM_EVENT_REJECTED:
            err("%s(%d, %u), event: %s, error: %d\n", __func__, port, q,
                rdma_event_str(event->event), event->status);
            break;
          default:
            break;
        }
        rdma_ack_cm_event(event);
      }
    } else if (ret == 0) {
      /* timeout */
    } else {
      err("%s(%d, %u), event poll error\n", __func__, port, q);
      break;
    }
  }

  info("%s(%d, %u), stop\n", __func__, port, q);
  return NULL;

connect_err:
  rdma_ack_cm_event(event);
  err("%s(%d, %u), err stop\n", __func__, port, q);
  return NULL;
}

static int rdma_tx_queue_uinit(struct mt_rdma_tx_queue* txq) {
  if (txq->ah) ibv_destroy_ah(txq->ah);
  if (txq->send_mr) ibv_dereg_mr(txq->send_mr);
  if (txq->cma_id->qp) rdma_destroy_qp(txq->cma_id);
  if (txq->pd) ibv_dealloc_pd(txq->pd);
  if (txq->rai) rdma_freeaddrinfo(txq->rai);
  if (txq->cma_id) rdma_destroy_id(txq->cma_id);
  txq->connected = true;
  pthread_join(txq->connect_thread, NULL);
  if (txq->ec) rdma_destroy_event_channel(txq->ec);

  return 0;
}

static int rdma_tx_queue_init(struct mt_rdma_tx_queue* txq) {
  int ret = 0;
  enum mtl_port port = txq->port;
  uint16_t q = txq->q;
  txq->ec = rdma_create_event_channel();
  if (!txq->ec) {
    err("%s(%d, %u), rdma_create_event_channel fail\n", __func__, port, q);
    rdma_tx_queue_uinit(txq);
    return -EIO;
  }
  ret = rdma_create_id(txq->ec, &txq->cma_id, txq, RDMA_PS_UDP);
  if (ret) {
    err("%s(%d, %u), rdma_create_id fail %d\n", __func__, port, q, ret);
    rdma_tx_queue_uinit(txq);
    return ret;
  }

  struct rdma_addrinfo hints = {};
  struct rdma_addrinfo *res, *rai;
  hints.ai_port_space = RDMA_PS_UDP;
  hints.ai_flags = RAI_PASSIVE;
  char ip[16];
  snprintf(ip, 16, "%d.%d.%d.%d", txq->sip[0], txq->sip[1], txq->sip[2], txq->sip[3]);
  ret = rdma_getaddrinfo(ip, NULL, &hints, &res);
  if (ret) {
    err("%s(%d, %u), rdma_getaddrinfo fail %d\n", __func__, port, q, ret);
    rdma_tx_queue_uinit(txq);
    return ret;
  }
  hints.ai_src_addr = res->ai_src_addr;
  hints.ai_src_len = res->ai_src_len;
  hints.ai_flags &= ~RAI_PASSIVE;
  uint8_t* dip = txq->tx_entry->flow.dip_addr;
  snprintf(ip, 16, "%d.%d.%d.%d", dip[0], dip[1], dip[2], dip[3]);
  char dport[6];
  snprintf(dport, 6, "%d", txq->tx_entry->flow.dst_port);
  ret = rdma_getaddrinfo(ip, dport, &hints, &rai);
  if (ret) {
    err("%s(%d, %u), rdma_getaddrinfo fail %d\n", __func__, port, q, ret);
    rdma_tx_queue_uinit(txq);
    rdma_freeaddrinfo(res);
    return ret;
  }
  txq->rai = rai;
  rdma_freeaddrinfo(res);

  /* connect to server */
  ret = rdma_resolve_addr(txq->cma_id, rai->ai_src_addr, rai->ai_dst_addr, 2000);
  if (ret) {
    err("%s(%d, %u), rdma_resolve_addr fail %d\n", __func__, port, q, ret);
    rdma_tx_queue_uinit(txq);
    return ret;
  }

  ret = pthread_create(&txq->connect_thread, NULL, rdma_tx_connect_thread, txq);
  if (ret) {
    err("%s(%d, %u), pthread_create fail %d\n", __func__, port, q, ret);
    rdma_tx_queue_uinit(txq);
    return ret;
  }

  return 0;
}

static int rdma_rx_mr_init(struct mt_rdma_rx_queue* rxq) {
  void* base_addr = NULL;
  struct rte_mempool* pool = rxq->mbuf_pool;
  size_t mr_size, align = 0;

  rxq->recv_len =
      rte_pktmbuf_data_room_size(pool) + sizeof(struct ibv_grh) - RTE_PKTMBUF_HEADROOM;
  base_addr = (void*)rdma_mp_base_addr(pool, &align);
  mr_size = (size_t)pool->populated_size *
                (size_t)rte_mempool_calc_obj_size(pool->elt_size, pool->flags, NULL) +
            align;
  rxq->recv_mr = ibv_reg_mr(rxq->pd, base_addr, mr_size, IBV_ACCESS_LOCAL_WRITE);
  if (!rxq->recv_mr) {
    err("%s(%d, %u), ibv_reg_mr fail\n", __func__, rxq->port, rxq->q);
    return -ENOMEM;
  }
  return 0;
}

static void* rdma_rx_connect_thread(void* arg) {
  struct mt_rdma_rx_queue* rxq = arg;
  enum mtl_port port = rxq->port;
  uint16_t q = rxq->q;
  struct rdma_cm_event* event;
  struct pollfd pfd;
  pfd.fd = rxq->ec->fd;
  pfd.events = POLLIN;

  info("%s(%d, %u), start\n", __func__, port, q);
  while (!rxq->connected) {
    int ret = poll(&pfd, 1, 200);
    if (ret > 0) {
      ret = rdma_get_cm_event(rxq->ec, &event);
      if (!ret) {
        switch (event->event) {
          case RDMA_CM_EVENT_CONNECT_REQUEST:
            rxq->pd = ibv_alloc_pd(event->id->verbs);
            if (!rxq->pd) {
              err("%s(%d, %u), ibv_alloc_pd fail\n", __func__, port, q);
              goto connect_err;
            }

            rxq->cq = ibv_create_cq(event->id->verbs, MT_RDMA_MAX_WR, rxq, NULL, 0);
            if (!rxq->cq) {
              err("%s(%d, %u), ibv_create_cq fail\n", __func__, port, q);
              goto connect_err;
            }

            struct ibv_qp_init_attr init_qp_attr = {};
            init_qp_attr.cap.max_send_wr = 1;
            init_qp_attr.cap.max_recv_wr = MT_RDMA_MAX_WR;
            init_qp_attr.cap.max_send_sge = 1;
            init_qp_attr.cap.max_recv_sge = 1;
            init_qp_attr.qp_context = rxq;
            init_qp_attr.send_cq = rxq->cq;
            init_qp_attr.recv_cq = rxq->cq;
            init_qp_attr.qp_type = IBV_QPT_UD;
            init_qp_attr.sq_sig_all = 0;
            ret = rdma_create_qp(event->id, rxq->pd, &init_qp_attr);
            if (ret) {
              err("%s(%d, %u), rdma_create_qp fail %d\n", __func__, port, q, ret);
              goto connect_err;
            }
            rxq->qp = event->id->qp;
            rxq->cma_id = event->id;

            ret = rdma_rx_mr_init(rxq);
            if (ret) {
              err("%s(%d, %u), rdma_rx_mr_init fail %d\n", __func__, port, q, ret);
              goto connect_err;
            }

            struct rte_mbuf* mbufs[1024];
            ret = rte_pktmbuf_alloc_bulk(rxq->mbuf_pool, mbufs, 1024);
            if (ret < 0) {
              err("%s(%d, %u), mbuf alloc fail %d\n", __func__, port, q, ret);
              goto connect_err;
            }

            rdma_rx_post_recv(rxq, mbufs, 1024);

            struct rdma_conn_param conn_param = {};
            conn_param.qp_num = event->id->qp->qp_num;
            ret = rdma_accept(event->id, &conn_param);
            if (ret) {
              err("%s(%d, %u), rdma_accept fail %d\n", __func__, port, q, ret);
              goto connect_err;
            }
            info("%s(%d, %u), rdma connected\n", __func__, port, q);
            rxq->connected = true;
          default:
            break;
        }
        rdma_ack_cm_event(event);
      }
    } else if (ret == 0) {
      /* timeout */
    } else {
      err("%s(%d, %u), event poll error\n", __func__, port, q);
    }
  }

  info("%s(%d, %u), stop\n", __func__, port, q);
  return NULL;

connect_err:
  rdma_ack_cm_event(event);
  err("%s(%d, %u), err stop\n", __func__, port, q);
  return NULL;
}

static int rdma_rx_queue_uinit(struct mt_rdma_rx_queue* rxq) {
  if (rxq->recv_mr) ibv_dereg_mr(rxq->recv_mr);
  if (rxq->qp) ibv_destroy_qp(rxq->qp);
  if (rxq->pd) ibv_dealloc_pd(rxq->pd);
  if (rxq->listen_id) rdma_destroy_id(rxq->listen_id);
  rxq->connected = true;
  pthread_join(rxq->connect_thread, NULL);
  if (rxq->ec) rdma_destroy_event_channel(rxq->ec);

  return 0;
}

static int rdma_rx_queue_init(struct mt_rdma_rx_queue* rxq) {
  int ret = 0;
  enum mtl_port port = rxq->port;
  uint16_t q = rxq->q;
  rxq->ec = rdma_create_event_channel();
  if (!rxq->ec) {
    err("%s(%d, %u), rdma_create_event_channel fail\n", __func__, port, q);
    rdma_rx_queue_uinit(rxq);
    return -1;
  }
  ret = rdma_create_id(rxq->ec, &rxq->listen_id, rxq, RDMA_PS_UDP);
  if (ret) {
    err("%s(%d, %u), rdma_create_id fail %d\n", __func__, port, q, ret);
    rdma_rx_queue_uinit(rxq);
    return ret;
  }

  struct rdma_addrinfo hints = {};
  hints.ai_port_space = RDMA_PS_UDP;
  hints.ai_flags = RAI_PASSIVE;
  char ip[16];
  snprintf(ip, 16, "%d.%d.%d.%d", rxq->sip[0], rxq->sip[1], rxq->sip[2], rxq->sip[3]);
  char dport[6];
  snprintf(dport, 6, "%d", rxq->rx_entry->flow.dst_port);
  struct rdma_addrinfo* rai;
  ret = rdma_getaddrinfo(ip, dport, &hints, &rai);
  if (ret) {
    err("%s(%d, %u), rdma_getaddrinfo fail %d\n", __func__, port, q, ret);
    rdma_rx_queue_uinit(rxq);
    return ret;
  }

  ret = rdma_bind_addr(rxq->listen_id, rai->ai_src_addr);
  if (ret) {
    err("%s(%d, %u), rdma_bind_addr fail %d\n", __func__, port, q, ret);
    rdma_rx_queue_uinit(rxq);
    rdma_freeaddrinfo(rai);
    return ret;
  }
  rdma_freeaddrinfo(rai);

  ret = rdma_listen(rxq->listen_id, 0);
  if (ret) {
    err("%s(%d, %u), rdma_listen fail %d\n", __func__, port, q, ret);
    rdma_rx_queue_uinit(rxq);
    return ret;
  }

  ret = pthread_create(&rxq->connect_thread, NULL, rdma_rx_connect_thread, rxq);
  if (ret) {
    err("%s(%d, %u), pthread_create fail %d\n", __func__, port, q, ret);
    rdma_rx_queue_uinit(rxq);
    return ret;
  }

  return 0;
}

struct mt_tx_rdma_entry* mt_tx_rdma_get(struct mtl_main_impl* impl, enum mtl_port port,
                                        struct mt_txq_flow* flow,
                                        struct mt_tx_rdma_get_args* args) {
  MTL_MAY_UNUSED(args);
  if (!mt_pmd_is_rdma_ud(impl, port)) {
    err("%s(%d), this pmd is not rdma ud\n", __func__, port);
    return NULL;
  }

  struct mt_tx_rdma_entry* entry =
      mt_rte_zmalloc_socket(sizeof(*entry), mt_socket_id(impl, port));
  if (!entry) {
    err("%s(%d), entry malloc fail\n", __func__, port);
    return NULL;
  }
  entry->parent = impl;
  entry->port = port;
  rte_memcpy(&entry->flow, flow, sizeof(entry->flow));

  struct mt_rdma_priv* rdma = mt_if(impl, port)->rdma;
  struct mt_rdma_tx_queue* txq = NULL;

  /* find a null slot */
  mt_pthread_mutex_lock(&rdma->queues_lock);
  for (uint16_t i = 0; i < rdma->tx_queues_cnt; i++) {
    if (!rdma->tx_queues[i].tx_entry) {
      txq = &rdma->tx_queues[i];
      txq->tx_entry = entry;
      break;
    }
  }
  mt_pthread_mutex_unlock(&rdma->queues_lock);
  if (!txq) {
    err("%s(%d), no free tx queue\n", __func__, port);
    mt_tx_rdma_put(entry);
    return NULL;
  }

  entry->txq = txq;
  entry->queue_id = txq->q;

  if (rdma_tx_queue_init(txq)) {
    err("%s(%d), rdma tx queue init fail\n", __func__, port);
    mt_tx_rdma_put(entry);
    return NULL;
  }

  uint8_t* ip = flow->dip_addr;
  info("%s(%d), ip %u.%u.%u.%u, port %u, queue %u\n", __func__, port, ip[0], ip[1], ip[2],
       ip[3], flow->dst_port, entry->queue_id);
  return entry;
}

int mt_tx_rdma_put(struct mt_tx_rdma_entry* entry) {
  enum mtl_port port = entry->port;
  struct mt_txq_flow* flow = &entry->flow;
  uint8_t* ip = flow->dip_addr;
  struct mt_rdma_tx_queue* txq = entry->txq;

  if (txq) {
    /* poll all done buf */
    rdma_tx_poll_done(txq);
    rdma_queue_tx_stat(txq);
    rdma_tx_queue_uinit(txq);

    txq->tx_entry = NULL;
    info("%s(%d), ip %u.%u.%u.%u, port %u, queue %u\n", __func__, port, ip[0], ip[1],
         ip[2], ip[3], flow->dst_port, entry->queue_id);
  }

  mt_rte_free(entry);
  return 0;
}

uint16_t mt_tx_rdma_burst(struct mt_tx_rdma_entry* entry, struct rte_mbuf** tx_pkts,
                          uint16_t nb_pkts) {
  return rdma_tx(entry->parent, entry->txq, tx_pkts, nb_pkts);
}

struct mt_rx_rdma_entry* mt_rx_rdma_get(struct mtl_main_impl* impl, enum mtl_port port,
                                        struct mt_rxq_flow* flow,
                                        struct mt_rx_rdma_get_args* args) {
  if (!mt_pmd_is_rdma_ud(impl, port)) {
    err("%s(%d), this pmd is not rdma\n", __func__, port);
    return NULL;
  }

  MTL_MAY_UNUSED(args);

  struct mt_rx_rdma_entry* entry =
      mt_rte_zmalloc_socket(sizeof(*entry), mt_socket_id(impl, port));
  if (!entry) {
    err("%s(%d), entry malloc fail\n", __func__, port);
    return NULL;
  }
  entry->parent = impl;
  entry->port = port;
  rte_memcpy(&entry->flow, flow, sizeof(entry->flow));

  struct mt_rdma_priv* rdma = mt_if(impl, port)->rdma;
  struct mt_rdma_rx_queue* rxq = NULL;

  /* find a null slot */
  mt_pthread_mutex_lock(&rdma->queues_lock);
  for (uint16_t i = 0; i < rdma->rx_queues_cnt; i++) {
    if (!rdma->rx_queues[i].rx_entry) {
      rxq = &rdma->rx_queues[i];
      rxq->rx_entry = entry;
      break;
    }
  }
  mt_pthread_mutex_unlock(&rdma->queues_lock);
  if (!rxq) {
    err("%s(%d), no free rx queue\n", __func__, port);
    mt_rx_rdma_put(entry);
    return NULL;
  }

  entry->rxq = rxq;
  entry->queue_id = rxq->q;
  uint16_t q = entry->queue_id;

  if (rdma_rx_queue_init(rxq)) {
    err("%s(%d), rdma tx queue init fail\n", __func__, port);
    mt_rx_rdma_put(entry);
    return NULL;
  }

  uint8_t* ip = flow->dip_addr;
  info("%s(%d,%u), ip %u.%u.%u.%u port %u\n", __func__, port, q, ip[0], ip[1], ip[2],
       ip[3], flow->dst_port);
  return entry;
}

int mt_rx_rdma_put(struct mt_rx_rdma_entry* entry) {
  enum mtl_port port = entry->port;
  struct mt_rxq_flow* flow = &entry->flow;
  uint8_t* ip = flow->dip_addr;
  struct mt_rdma_rx_queue* rxq = entry->rxq;

  if (rxq) {
    rdma_queue_rx_stat(rxq);
    rdma_rx_queue_uinit(rxq);
    rxq->rx_entry = NULL;
  }
  info("%s(%d), ip %u.%u.%u.%u, port %u, queue %u\n", __func__, port, ip[0], ip[1], ip[2],
       ip[3], flow->dst_port, entry->queue_id);
  mt_rte_free(entry);
  return 0;
}

uint16_t mt_rx_rdma_burst(struct mt_rx_rdma_entry* entry, struct rte_mbuf** rx_pkts,
                          const uint16_t nb_pkts) {
  return rdma_rx(entry, rx_pkts, nb_pkts);
}
