/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 * The data path based on linux kernel socket interface
 */

#include "mt_af_xdp.h"

#include <linux/ethtool.h>
#include <linux/if_link.h>
#include <linux/if_xdp.h>
#include <linux/sockios.h>
#include <xdp/xsk.h>

#include "../mt_log.h"

#ifndef XDP_UMEM_UNALIGNED_CHUNK_FLAG
#error "Please use XDP lib version with XDP_UMEM_UNALIGNED_CHUNK_FLAG"
#endif

struct mt_xdp_queue {
  enum mtl_port port;
  struct rte_mempool* mbuf_pool;
  uint16_t q;
  uint32_t umem_ring_size;
  uint32_t tx_free_thresh;

  struct xsk_umem* umem;
  struct xsk_ring_prod pq;
  struct rte_mbuf** pq_mbufs;
  struct xsk_ring_cons cq;
  void* umem_buffer;

  struct xsk_socket* socket;
  int socket_fd;
  struct xsk_ring_cons socket_rx;
  struct xsk_ring_prod socket_tx;

  struct mt_tx_xdp_entry* tx_entry;
  struct mt_rx_xdp_entry* rx_entry;
};

struct mt_xdp_priv {
  struct mtl_main_impl* parent;
  enum mtl_port port;
  uint8_t start_queue;
  uint16_t queues_cnt;
  uint32_t max_combined;
  uint32_t combined_count;

  struct mt_xdp_queue* queues_info;
  pthread_mutex_t tx_queues_lock;
  pthread_mutex_t rx_queues_lock;
};

static int xdp_pq_uinit(struct mt_xdp_queue* xq) {
  if (!xq->pq_mbufs) return 0;

  if (xq->pq_mbufs[0]) rte_pktmbuf_free_bulk(xq->pq_mbufs, xq->umem_ring_size);

  mt_rte_free(xq->pq_mbufs);
  xq->pq_mbufs = NULL;

  return 0;
}

static int xdp_queue_uinit(struct mt_xdp_queue* xq) {
  if (xq->socket) {
    xsk_socket__delete(xq->socket);
    xq->socket = NULL;
  }

  xdp_pq_uinit(xq);

  if (xq->umem) {
    xsk_umem__delete(xq->umem);
    xq->umem = NULL;
  }

  return 0;
}

static int xdp_free(struct mt_xdp_priv* xdp) {
  enum mtl_port port = xdp->port;

  if (xdp->queues_info) {
    for (uint16_t i = 0; i < xdp->queues_cnt; i++) {
      struct mt_xdp_queue* xq = &xdp->queues_info[i];
      xdp_queue_uinit(xq);
      if (xq->tx_entry) {
        warn("%s(%d,%u), tx_entry still active\n", __func__, port, xq->q);
        mt_tx_xdp_put(xq->tx_entry);
      }
      if (xq->rx_entry) {
        warn("%s(%d,%u), rx_entry still active\n", __func__, port, xq->q);
        mt_rx_xdp_put(xq->rx_entry);
      }
    }
    mt_rte_free(xdp->queues_info);
    xdp->queues_info = NULL;
  }

  mt_pthread_mutex_destroy(&xdp->tx_queues_lock);
  mt_pthread_mutex_destroy(&xdp->rx_queues_lock);
  mt_rte_free(xdp);
  return 0;
}

static int xdp_parse_combined_info(struct mt_xdp_priv* xdp) {
  struct mtl_main_impl* impl = xdp->parent;
  enum mtl_port port = xdp->port;
  const char* if_name = mt_kernel_if_name(impl, port);
  struct ethtool_channels channels;
  struct ifreq ifr;
  int fd, ret;

  fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) return -1;

  channels.cmd = ETHTOOL_GCHANNELS;
  ifr.ifr_data = (void*)&channels;
  strlcpy(ifr.ifr_name, if_name, IFNAMSIZ);
  ret = ioctl(fd, SIOCETHTOOL, &ifr);
  if (ret < 0) {
    warn("%s(%d), SIOCETHTOOL fail %d\n", __func__, port, ret);
    return ret;
  }

  xdp->max_combined = channels.max_combined;
  xdp->combined_count = channels.combined_count;
  info("%s(%d), combined max %u cnt %u\n", __func__, port, xdp->max_combined,
       xdp->combined_count);
  return 0;
}

static inline uintptr_t xdp_mp_base_addr(struct rte_mempool* mp, uint64_t* align) {
  struct rte_mempool_memhdr* hdr;
  uintptr_t hdr_addr, aligned_addr;

  hdr = STAILQ_FIRST(&mp->mem_list);
  hdr_addr = (uintptr_t)hdr->addr;
  aligned_addr = hdr_addr & ~(getpagesize() - 1);
  *align = hdr_addr - aligned_addr;

  return aligned_addr;
}

static int xdp_umem_init(struct mt_xdp_queue* xq) {
  enum mtl_port port = xq->port;
  uint16_t q = xq->q;
  int ret;
  struct xsk_umem_config cfg;
  void* base_addr = NULL;
  struct rte_mempool* pool = xq->mbuf_pool;
  uint64_t umem_size, align = 0;

  memset(&cfg, 0, sizeof(cfg));
  cfg.fill_size = xq->umem_ring_size * 2;
  cfg.comp_size = xq->umem_ring_size;
  cfg.flags = XDP_UMEM_UNALIGNED_CHUNK_FLAG;

  cfg.frame_size = rte_mempool_calc_obj_size(pool->elt_size, pool->flags, NULL);
  cfg.frame_headroom = pool->header_size + sizeof(struct rte_mbuf) +
                       rte_pktmbuf_priv_size(pool) + RTE_PKTMBUF_HEADROOM;

  base_addr = (void*)xdp_mp_base_addr(pool, &align);
  umem_size = (uint64_t)pool->populated_size * (uint64_t)cfg.frame_size + align;
  dbg("%s(%d), base_addr %p umem_size %" PRIu64 "\n", __func__, port, base_addr,
      umem_size);
  ret = xsk_umem__create(&xq->umem, base_addr, umem_size, &xq->pq, &xq->cq, &cfg);
  if (ret < 0) {
    err("%s(%d,%u), umem create fail %d %s\n", __func__, port, q, ret, strerror(errno));
    return ret;
  }
  xq->umem_buffer = base_addr;

  info("%s(%d,%u), umem %p buffer %p size %" PRIu64 "\n", __func__, port, q, xq->umem,
       xq->umem_buffer, umem_size);
  return 0;
}

static int xdp_pq_init(struct mt_xdp_priv* xdp, struct mt_xdp_queue* xq) {
  enum mtl_port port = xq->port;
  uint16_t q = xq->q;
  uint32_t ring_sz = xq->umem_ring_size;
  struct xsk_ring_prod* pq = &xq->pq;
  int ret;

  xq->pq_mbufs = mt_rte_zmalloc_socket(sizeof(*xq->pq_mbufs) * ring_sz,
                                       mt_socket_id(xdp->parent, port));
  if (!xq->pq_mbufs) {
    err("%s(%d,%u), pq_mbufs alloc fail %d\n", __func__, port, q, ret);
    return -ENOMEM;
  }
  ret = rte_pktmbuf_alloc_bulk(xq->mbuf_pool, xq->pq_mbufs, ring_sz);
  if (ret < 0) {
    xdp_pq_uinit(xq);
    err("%s(%d,%u), mbuf alloc fail %d\n", __func__, port, q, ret);
    return ret;
  }

  uint32_t idx = 0;
  if (!xsk_ring_prod__reserve(pq, ring_sz, &idx)) {
    err("%s(%d,%u), reserve fail\n", __func__, port, q);
    xdp_pq_uinit(xq);
    return -EIO;
  }

  for (uint32_t i = 0; i < ring_sz; i++) {
    __u64* fq_addr;
    uint64_t addr;

    fq_addr = xsk_ring_prod__fill_addr(pq, idx++);
    addr = (uint64_t)xq->pq_mbufs[i] - (uint64_t)xq->umem_buffer -
           xq->mbuf_pool->header_size;
    *fq_addr = addr;
  }

  xsk_ring_prod__submit(pq, ring_sz);

  return 0;
}

static int xdp_socket_init(struct mt_xdp_priv* xdp, struct mt_xdp_queue* xq) {
  enum mtl_port port = xq->port;
  uint16_t q = xq->q;
  struct mtl_main_impl* impl = xdp->parent;
  struct xsk_socket_config cfg;
  int ret;

  memset(&cfg, 0, sizeof(cfg));
  cfg.rx_size = mt_if_nb_rx_desc(impl, port);
  cfg.tx_size = mt_if_nb_tx_desc(impl, port);
  cfg.xdp_flags = XDP_FLAGS_UPDATE_IF_NOEXIST;
  cfg.bind_flags = XDP_USE_NEED_WAKEUP;

  const char* if_name = mt_kernel_if_name(impl, port);
  ret = xsk_socket__create(&xq->socket, if_name, q, xq->umem, &xq->socket_rx,
                           &xq->socket_tx, &cfg);
  if (ret < 0) {
    err("%s(%d,%u), xsk create fail %d\n", __func__, port, q, ret);
    return ret;
  }

  xq->socket_fd = xsk_socket__fd(xq->socket);
  return 0;
}

static int xdp_queue_init(struct mt_xdp_priv* xdp, struct mt_xdp_queue* xq) {
  enum mtl_port port = xq->port;
  uint16_t q = xq->q;
  int ret;

  ret = xdp_umem_init(xq);
  if (ret < 0) {
    err("%s(%d,%u), umem init fail %d\n", __func__, port, q, ret);
    xdp_queue_uinit(xq);
    return ret;
  }

  ret = xdp_pq_init(xdp, xq);
  if (ret < 0) {
    err("%s(%d,%u), pq init fail %d\n", __func__, port, q, ret);
    xdp_queue_uinit(xq);
    return ret;
  }

  ret = xdp_socket_init(xdp, xq);
  if (ret < 0) {
    err("%s(%d,%u), socket init fail %d\n", __func__, port, q, ret);
    xdp_queue_uinit(xq);
    return ret;
  }

  return 0;
}

static void xdp_tx_pull(struct mt_xdp_queue* xq) {
  struct xsk_ring_cons* cq = &xq->cq;
  uint32_t idx = 0;
  uint32_t size = xq->umem_ring_size;
  uint32_t n = xsk_ring_cons__peek(cq, size, &idx);

  for (uint32_t i = 0; i < n; i++) {
    uint64_t addr = *xsk_ring_cons__comp_addr(cq, idx++);
    addr = xsk_umem__extract_addr(addr);
    struct rte_mbuf* m = (struct rte_mbuf*)xsk_umem__get_data(
        xq->umem_buffer, addr + xq->mbuf_pool->header_size);
    dbg("%s(%d, %u), free mbuf %p addr 0x%" PRIu64 "\n", __func__, xq->port, xq->q, m,
        addr);
    rte_pktmbuf_free(m);
  }

  xsk_ring_cons__release(cq, n);
}

static void xdp_tx_kick(struct mt_xdp_queue* xq) {
  struct xsk_ring_prod* socket_tx = &xq->socket_tx;
  enum mtl_port port = xq->port;
  uint16_t q = xq->q;

  xdp_tx_pull(xq);
  if (xsk_ring_prod__needs_wakeup(socket_tx)) {
    int ret = send(xq->socket_fd, NULL, 0, MSG_DONTWAIT);
    dbg("%s(%d, %u), wake up %d\n", __func__, port, q, ret);
    if (ret < 0) {
      err("%s(%d, %u), wake up fail %d(%s)\n", __func__, port, q, ret, strerror(errno));
    }
  }
}

static uint16_t xdp_tx(struct mtl_main_impl* impl, struct mt_xdp_queue* xq,
                       struct rte_mbuf** tx_pkts, uint16_t nb_pkts) {
  struct xsk_ring_cons* cq = &xq->cq;
  uint32_t free_thresh = xq->tx_free_thresh;
  enum mtl_port port = xq->port;
  uint16_t q = xq->q;
  struct rte_mempool* mbuf_pool = xq->mbuf_pool;
  uint16_t tx = 0;
  struct xsk_ring_prod* socket_tx = &xq->socket_tx;
  struct mtl_port_status* stats = mt_if(impl, port)->dev_stats_sw;
  uint64_t tx_bytes = 0;

  uint32_t cq_avail = xsk_cons_nb_avail(cq, free_thresh);
  dbg("%s(%d, %u), cq_avail %u\n", __func__, port, q, cq_avail);
  if (cq_avail >= free_thresh) {
    xdp_tx_pull(xq);
  }

  for (uint16_t i = 0; i < nb_pkts; i++) {
    struct rte_mbuf* m = tx_pkts[i];

    if (m->pool == mbuf_pool) {
      warn("%s(%d, %u), same mbuf_pool todo\n", __func__, port, q);
      goto exit;
    } else {
      struct rte_mbuf* local = rte_pktmbuf_alloc(mbuf_pool);
      if (!local) {
        err("%s(%d, %u), local mbuf alloc fail\n", __func__, port, q);
        goto exit;
      }

      uint32_t idx;
      if (!xsk_ring_prod__reserve(socket_tx, 1, &idx)) {
        err("%s(%d, %u), socket_tx reserve fail\n", __func__, port, q);
        rte_pktmbuf_free(local);
        goto exit;
      }
      struct xdp_desc* desc = xsk_ring_prod__tx_desc(socket_tx, idx);
      desc->len = m->pkt_len;
      uint64_t addr =
          (uint64_t)local - (uint64_t)xq->umem_buffer - xq->mbuf_pool->header_size;
      uint64_t offset = rte_pktmbuf_mtod(local, uint64_t) - (uint64_t)local +
                        xq->mbuf_pool->header_size;
      void* pkt = xsk_umem__get_data(xq->umem_buffer, addr + offset);
      offset = offset << XSK_UNALIGNED_BUF_OFFSET_SHIFT;
      desc->addr = addr | offset;
      rte_memcpy(pkt, rte_pktmbuf_mtod(m, void*), desc->len);
      tx_bytes += m->data_len;
      rte_pktmbuf_free(m);
      dbg("%s(%d, %u), tx local mbuf %p umem pkt %p addr 0x%" PRIu64 "\n", __func__, port,
          q, local, pkt, addr);
      tx++;
    }
  }

exit:
  if (tx) {
    dbg("%s(%d, %u), submit %u\n", __func__, port, q, tx);
    xsk_ring_prod__submit(socket_tx, tx);
    xdp_tx_kick(xq);
    if (stats) {
      stats->tx_packets += tx;
      stats->tx_bytes += tx_bytes;
    }
  }
  return tx;
}

int mt_dev_xdp_init(struct mt_interface* inf) {
  struct mtl_main_impl* impl = inf->parent;
  enum mtl_port port = inf->port;
  struct mtl_init_params* p = mt_get_user_params(impl);
  int ret;

  if (!mt_pmd_is_native_af_xdp(impl, port)) {
    err("%s(%d), not native af_xdp\n", __func__, port);
    return -EIO;
  }

  struct mt_xdp_priv* xdp = mt_rte_zmalloc_socket(sizeof(*xdp), mt_socket_id(impl, port));
  if (!xdp) {
    err("%s(%d), xdp malloc fail\n", __func__, port);
    return -ENOMEM;
  }
  xdp->parent = impl;
  xdp->port = port;
  xdp->max_combined = 1;
  xdp->combined_count = 1;
  xdp->start_queue = p->xdp_info[port].start_queue;
  xdp->queues_cnt = RTE_MAX(inf->max_tx_queues, inf->max_rx_queues);
  mt_pthread_mutex_init(&xdp->tx_queues_lock, NULL);
  mt_pthread_mutex_init(&xdp->rx_queues_lock, NULL);

  xdp_parse_combined_info(xdp);
  if ((xdp->start_queue + xdp->queues_cnt) > xdp->combined_count) {
    err("%s(%d), too many queues requested, start_queue %u queues_cnt %u combined_count "
        "%u\n",
        __func__, port, xdp->start_queue, xdp->queues_cnt, xdp->combined_count);
    xdp_free(xdp);
    return -ENOTSUP;
  }

  xdp->queues_info = mt_rte_zmalloc_socket(sizeof(*xdp->queues_info) * xdp->queues_cnt,
                                           mt_socket_id(impl, port));
  if (!xdp->queues_info) {
    err("%s(%d), xdp queues_info malloc fail\n", __func__, port);
    xdp_free(xdp);
    return -ENOMEM;
  }
  for (uint16_t i = 0; i < xdp->queues_cnt; i++) {
    struct mt_xdp_queue* xq = &xdp->queues_info[i];
    uint16_t q = i + xdp->start_queue;

    xq->port = port;
    xq->q = q;
    xq->umem_ring_size = XSK_RING_CONS__DEFAULT_NUM_DESCS;
    xq->tx_free_thresh = xq->umem_ring_size / 2;
    xq->mbuf_pool = inf->rx_queues[i].mbuf_pool;
    if (!xq->mbuf_pool) {
      err("%s(%d), no mbuf_pool for q %u\n", __func__, port, q);
      xdp_free(xdp);
      return -EIO;
    }

    ret = xdp_queue_init(xdp, xq);
    if (ret < 0) {
      err("%s(%d), queue init fail %d for q %u\n", __func__, port, ret, q);
      xdp_free(xdp);
      return ret;
    }
  }

  inf->xdp = xdp;
  info("%s(%d), start queue %u cnt %u\n", __func__, port, xdp->start_queue,
       xdp->queues_cnt);
  return 0;
}

int mt_dev_xdp_uinit(struct mt_interface* inf) {
  struct mt_xdp_priv* xdp = inf->xdp;
  if (!xdp) return 0;

  xdp_free(xdp);
  inf->xdp = NULL;
  dbg("%s(%d), succ\n", __func__, inf->port);
  return 0;
}

struct mt_tx_xdp_entry* mt_tx_xdp_get(struct mtl_main_impl* impl, enum mtl_port port,
                                      struct mt_txq_flow* flow) {
  if (!mt_pmd_is_native_af_xdp(impl, port)) {
    err("%s(%d), this pmd is not native xdp\n", __func__, port);
    return NULL;
  }

  struct mt_tx_xdp_entry* entry =
      mt_rte_zmalloc_socket(sizeof(*entry), mt_socket_id(impl, port));
  if (!entry) {
    err("%s(%d), entry malloc fail\n", __func__, port);
    return NULL;
  }
  entry->parent = impl;
  entry->port = port;
  rte_memcpy(&entry->flow, flow, sizeof(entry->flow));

  struct mt_xdp_priv* xdp = mt_if(impl, port)->xdp;
  struct mt_xdp_queue* xq = NULL;
  /* find a null slot */
  mt_pthread_mutex_lock(&xdp->tx_queues_lock);
  for (uint16_t i = 0; i < xdp->queues_cnt; i++) {
    if (!xdp->queues_info[i].tx_entry) {
      xq = &xdp->queues_info[i];
      xq->tx_entry = entry;
      break;
    }
  }
  mt_pthread_mutex_unlock(&xdp->tx_queues_lock);
  if (!xq) {
    err("%s(%d), no free tx queue\n", __func__, port);
    mt_tx_xdp_put(entry);
    return NULL;
  }
  entry->xq = xq;
  entry->queue_id = xq->q;

  uint8_t* ip = flow->dip_addr;
  info("%s(%d), ip %u.%u.%u.%u, port %u, queue %u\n", __func__, port, ip[0], ip[1], ip[2],
       ip[3], flow->dst_port, entry->queue_id);
  return entry;
}

int mt_tx_xdp_put(struct mt_tx_xdp_entry* entry) {
  enum mtl_port port = entry->port;
  struct mt_txq_flow* flow = &entry->flow;
  uint8_t* ip = flow->dip_addr;
  struct mt_xdp_queue* xq = entry->xq;

  /* pull all buf sent */
  xdp_tx_pull(xq);

  xq->tx_entry = NULL;
  info("%s(%d), ip %u.%u.%u.%u, port %u, queue %u\n", __func__, port, ip[0], ip[1], ip[2],
       ip[3], flow->dst_port, entry->queue_id);
  mt_rte_free(entry);
  return 0;
}

uint16_t mt_tx_xdp_burst(struct mt_tx_xdp_entry* entry, struct rte_mbuf** tx_pkts,
                         uint16_t nb_pkts) {
  return xdp_tx(entry->parent, entry->xq, tx_pkts, nb_pkts);
}

struct mt_rx_xdp_entry* mt_rx_xdp_get(struct mtl_main_impl* impl, enum mtl_port port,
                                      struct mt_rxq_flow* flow) {
  if (!mt_pmd_is_native_af_xdp(impl, port)) {
    err("%s(%d), this pmd is not native xdp\n", __func__, port);
    return NULL;
  }

  struct mt_rx_xdp_entry* entry =
      mt_rte_zmalloc_socket(sizeof(*entry), mt_socket_id(impl, port));
  if (!entry) {
    err("%s(%d), entry malloc fail\n", __func__, port);
    return NULL;
  }
  entry->parent = impl;
  entry->port = port;
  rte_memcpy(&entry->flow, flow, sizeof(entry->flow));

  struct mt_xdp_priv* xdp = mt_if(impl, port)->xdp;
  struct mt_xdp_queue* xq = NULL;
  /* find a null slot */
  mt_pthread_mutex_lock(&xdp->rx_queues_lock);
  for (uint16_t i = 0; i < xdp->queues_cnt; i++) {
    if (!xdp->queues_info[i].rx_entry) {
      xq = &xdp->queues_info[i];
      xq->rx_entry = entry;
      break;
    }
  }
  mt_pthread_mutex_unlock(&xdp->rx_queues_lock);
  if (!xq) {
    err("%s(%d), no free tx queue\n", __func__, port);
    mt_rx_xdp_put(entry);
    return NULL;
  }
  entry->xq = xq;
  entry->queue_id = xq->q;

  uint8_t* ip = flow->dip_addr;
  info("%s(%d), ip %u.%u.%u.%u port %u queue %d\n", __func__, port, ip[0], ip[1], ip[2],
       ip[3], flow->dst_port, entry->queue_id);
  return entry;
}

int mt_rx_xdp_put(struct mt_rx_xdp_entry* entry) {
  enum mtl_port port = entry->port;
  struct mt_rxq_flow* flow = &entry->flow;
  uint8_t* ip = flow->dip_addr;
  struct mt_xdp_queue* xq = entry->xq;

  xq->rx_entry = NULL;
  info("%s(%d), ip %u.%u.%u.%u, port %u, queue %u\n", __func__, port, ip[0], ip[1], ip[2],
       ip[3], flow->dst_port, entry->queue_id);
  mt_rte_free(entry);
  return 0;
}

uint16_t mt_rx_xdp_burst(struct mt_rx_xdp_entry* entry, struct rte_mbuf** rx_pkts,
                         const uint16_t nb_pkts) {
  MTL_MAY_UNUSED(entry);
  MTL_MAY_UNUSED(rx_pkts);
  MTL_MAY_UNUSED(nb_pkts);
  return 0;
}
