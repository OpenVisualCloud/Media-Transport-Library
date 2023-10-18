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

#include "../mt_flow.h"
#include "../mt_log.h"
#include "../mt_stat.h"

#ifndef XDP_UMEM_UNALIGNED_CHUNK_FLAG
#error "Please use XDP lib version with XDP_UMEM_UNALIGNED_CHUNK_FLAG support"
#endif

struct mt_xdp_queue {
  enum mtl_port port;
  struct rte_mempool* mbuf_pool;
  uint16_t q;
  uint32_t umem_ring_size;

  struct xsk_umem* umem;
  void* umem_buffer;

  struct xsk_socket* socket;
  int socket_fd;

  /* rx pkt send on this producer ring, filled by kernel */
  struct xsk_ring_prod rx_prod;
  /* rx pkt done on this consumer ring, pulled from userspace on the RX data path */
  struct xsk_ring_cons rx_cons;

  /* tx pkt done on this consumer ring, filled by kernel */
  struct xsk_ring_cons tx_cons;
  /* tx pkt send on this producer ring, filled from userspace on the TX data path */
  struct xsk_ring_prod tx_prod;
  uint32_t tx_free_thresh;

  struct mt_tx_xdp_entry* tx_entry;
  struct mt_rx_xdp_entry* rx_entry;

  uint64_t stat_tx_pkts;
  uint64_t stat_tx_bytes;
  uint64_t stat_tx_free;
  uint64_t stat_tx_submit;
  uint64_t stat_tx_copy;
  uint64_t stat_tx_wakeup;
  uint64_t stat_tx_mbuf_alloc_fail;
  uint64_t stat_tx_prod_reserve_fail;

  uint64_t stat_rx_pkts;
  uint64_t stat_rx_bytes;
  uint64_t stat_rx_burst;
  uint64_t stat_rx_mbuf_alloc_fail;
  uint64_t stat_rx_prod_reserve_fail;
};

struct mt_xdp_priv {
  struct mtl_main_impl* parent;
  enum mtl_port port;
  uint8_t start_queue;
  uint16_t queues_cnt;
  uint32_t max_combined;
  uint32_t combined_count;

  struct mt_xdp_queue* queues_info;
  pthread_mutex_t queues_lock;
};

static int xdp_queue_tx_stat(struct mt_xdp_queue* xq) {
  enum mtl_port port = xq->port;
  uint16_t q = xq->q;

  notice("%s(%d,%u), pkts %" PRIu64 " bytes %" PRIu64 " submit %" PRIu64 " free %" PRIu64
         " wakeup %" PRIu64 "\n",
         __func__, port, q, xq->stat_tx_pkts, xq->stat_tx_bytes, xq->stat_tx_submit,
         xq->stat_tx_free, xq->stat_tx_wakeup);
  xq->stat_tx_pkts = 0;
  xq->stat_tx_bytes = 0;
  xq->stat_tx_submit = 0;
  xq->stat_tx_free = 0;
  xq->stat_tx_wakeup = 0;
  if (xq->stat_tx_copy) {
    notice("%s(%d,%u), pkts copy %" PRIu64 "\n", __func__, port, q, xq->stat_tx_copy);
    xq->stat_tx_copy = 0;
  }

  uint32_t ring_sz = xq->umem_ring_size;
  uint32_t cons_avail = xsk_cons_nb_avail(&xq->tx_cons, ring_sz);
  uint32_t prod_free = xsk_prod_nb_free(&xq->tx_prod, ring_sz);
  notice("%s(%d,%u), cons_avail %u prod_free %u\n", __func__, port, q, cons_avail,
         prod_free);

  if (xq->stat_tx_mbuf_alloc_fail) {
    warn("%s(%d,%u), mbuf alloc fail %" PRIu64 "\n", __func__, port, q,
         xq->stat_tx_mbuf_alloc_fail);
    xq->stat_tx_mbuf_alloc_fail = 0;
  }
  if (xq->stat_tx_prod_reserve_fail) {
    err("%s(%d,%u), prod reserve fail %" PRIu64 "\n", __func__, port, q,
        xq->stat_tx_prod_reserve_fail);
    xq->stat_tx_prod_reserve_fail = 0;
  }
  return 0;
}

static int xdp_queue_rx_stat(struct mt_xdp_queue* xq) {
  enum mtl_port port = xq->port;
  uint16_t q = xq->q;

  notice("%s(%d,%u), pkts %" PRIu64 " bytes %" PRIu64 " burst %" PRIu64 "\n", __func__,
         port, q, xq->stat_rx_pkts, xq->stat_rx_bytes, xq->stat_rx_burst);
  xq->stat_rx_pkts = 0;
  xq->stat_rx_bytes = 0;
  xq->stat_rx_burst = 0;

  uint32_t ring_sz = xq->umem_ring_size;
  uint32_t cons_avail = xsk_cons_nb_avail(&xq->rx_cons, ring_sz);
  uint32_t prod_free = xsk_prod_nb_free(&xq->rx_prod, ring_sz);
  notice("%s(%d,%u), cons_avail %u prod_free %u\n", __func__, port, q, cons_avail,
         prod_free);

  if (xq->stat_rx_mbuf_alloc_fail) {
    warn("%s(%d,%u), mbuf alloc fail %" PRIu64 "\n", __func__, port, q,
         xq->stat_rx_mbuf_alloc_fail);
    xq->stat_rx_mbuf_alloc_fail = 0;
  }
  if (xq->stat_rx_prod_reserve_fail) {
    err("%s(%d,%u), prod reserve fail %" PRIu64 "\n", __func__, port, q,
        xq->stat_rx_prod_reserve_fail);
    xq->stat_rx_prod_reserve_fail = 0;
  }

  return 0;
}

static int xdp_stat_dump(void* priv) {
  struct mt_xdp_priv* xdp = priv;

  for (uint16_t i = 0; i < xdp->queues_cnt; i++) {
    struct mt_xdp_queue* xq = &xdp->queues_info[i];
    if (xq->tx_entry) xdp_queue_tx_stat(xq);
    if (xq->rx_entry) xdp_queue_rx_stat(xq);
  }

  return 0;
}

static int xdp_queue_uinit(struct mt_xdp_queue* xq) {
  if (xq->socket) {
    xsk_socket__delete(xq->socket);
    xq->socket = NULL;
  }

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

  mt_pthread_mutex_destroy(&xdp->queues_lock);
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
  ret =
      xsk_umem__create(&xq->umem, base_addr, umem_size, &xq->rx_prod, &xq->tx_cons, &cfg);
  if (ret < 0) {
    err("%s(%d,%u), umem create fail %d %s\n", __func__, port, q, ret, strerror(errno));
    return ret;
  }
  xq->umem_buffer = base_addr;

  info("%s(%d,%u), umem %p buffer %p size %" PRIu64 "\n", __func__, port, q, xq->umem,
       xq->umem_buffer, umem_size);
  return 0;
}

static inline int xdp_rx_prod_reserve(struct mt_xdp_queue* xq, struct rte_mbuf** mbufs,
                                      uint16_t sz) {
  enum mtl_port port = xq->port;
  uint16_t q = xq->q;
  uint32_t idx = 0;
  struct xsk_ring_prod* pq = &xq->rx_prod;
  int ret;

  ret = xsk_ring_prod__reserve(pq, sz, &idx);
  if (ret < 0) {
    err("%s(%d,%u), prod reserve %u fail %d\n", __func__, port, q, sz, ret);
    return ret;
  }

  for (uint32_t i = 0; i < sz; i++) {
    __u64* fq_addr;
    uint64_t addr;

    fq_addr = xsk_ring_prod__fill_addr(pq, idx++);
    addr = (uint64_t)mbufs[i] - (uint64_t)xq->umem_buffer - xq->mbuf_pool->header_size;
    *fq_addr = addr;
  }

  xsk_ring_prod__submit(pq, sz);
  return 0;
}

static int xdp_rx_prod_init(struct mt_xdp_queue* xq) {
  enum mtl_port port = xq->port;
  uint16_t q = xq->q;
  uint32_t ring_sz = xq->umem_ring_size;
  int ret;

  struct rte_mbuf* mbufs[ring_sz];
  ret = rte_pktmbuf_alloc_bulk(xq->mbuf_pool, mbufs, ring_sz);
  if (ret < 0) {
    err("%s(%d,%u), mbufs alloc fail %d\n", __func__, port, q, ret);
    return ret;
  }

  ret = xdp_rx_prod_reserve(xq, mbufs, ring_sz);
  if (ret < 0) {
    err("%s(%d,%u), fill fail %d\n", __func__, port, q, ret);
    return ret;
  }

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
  // cfg.bind_flags = XDP_USE_NEED_WAKEUP;

  const char* if_name = mt_kernel_if_name(impl, port);
  ret = xsk_socket__create(&xq->socket, if_name, q, xq->umem, &xq->rx_cons, &xq->tx_prod,
                           &cfg);
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

  ret = xdp_rx_prod_init(xq);
  if (ret < 0) {
    err("%s(%d,%u), rx prod init fail %d\n", __func__, port, q, ret);
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

static void xdp_tx_poll_done(struct mt_xdp_queue* xq) {
  struct xsk_ring_cons* cq = &xq->tx_cons;
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
  xq->stat_tx_free += n;

  xsk_ring_cons__release(cq, n);
}

static inline void xdp_tx_check_free(struct mt_xdp_queue* xq) {
  struct xsk_ring_cons* cq = &xq->tx_cons;
  uint32_t cq_avail = xsk_cons_nb_avail(cq, xq->umem_ring_size);
  dbg("%s(%d, %u), cq_avail %u\n", __func__, port, q, cq_avail);
  if (cq_avail >= xq->tx_free_thresh) {
    xdp_tx_poll_done(xq);
  }
}

static void xdp_tx_wakeup(struct mt_xdp_queue* xq) {
  enum mtl_port port = xq->port;
  uint16_t q = xq->q;

  if (xsk_ring_prod__needs_wakeup(&xq->tx_prod)) {
    int ret = send(xq->socket_fd, NULL, 0, MSG_DONTWAIT);
    xq->stat_tx_wakeup++;
    dbg("%s(%d, %u), wake up %d\n", __func__, port, q, ret);
    if (ret < 0) {
      err("%s(%d, %u), wake up fail %d(%s)\n", __func__, port, q, ret, strerror(errno));
    }
  }
}

static uint16_t xdp_tx(struct mtl_main_impl* impl, struct mt_xdp_queue* xq,
                       struct rte_mbuf** tx_pkts, uint16_t nb_pkts) {
  enum mtl_port port = xq->port;
  uint16_t q = xq->q;
  struct rte_mempool* mbuf_pool = xq->mbuf_pool;
  uint16_t tx = 0;
  struct xsk_ring_prod* pd = &xq->tx_prod;
  struct mtl_port_status* stats = mt_if(impl, port)->dev_stats_sw;
  uint64_t tx_bytes = 0;

  xdp_tx_check_free(xq); /* do we need check free threshold for every tx burst */

  for (uint16_t i = 0; i < nb_pkts; i++) {
    struct rte_mbuf* m = tx_pkts[i];

    if (m->pool == mbuf_pool) {
      warn("%s(%d, %u), same mbuf_pool todo\n", __func__, port, q);
      goto exit;
    } else {
      struct rte_mbuf* local = rte_pktmbuf_alloc(mbuf_pool);
      if (!local) {
        dbg("%s(%d, %u), local mbuf alloc fail\n", __func__, port, q);
        xq->stat_tx_mbuf_alloc_fail++;
        goto exit;
      }

      uint32_t idx;
      if (!xsk_ring_prod__reserve(pd, 1, &idx)) {
        err("%s(%d, %u), socket_tx reserve fail\n", __func__, port, q);
        xq->stat_tx_prod_reserve_fail++;
        rte_pktmbuf_free(local);
        goto exit;
      }
      struct xdp_desc* desc = xsk_ring_prod__tx_desc(pd, idx);
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
      xq->stat_tx_copy++;
      tx++;
    }
  }

exit:
  if (tx) {
    dbg("%s(%d, %u), submit %u\n", __func__, port, q, tx);
    xsk_ring_prod__submit(pd, tx);
    xdp_tx_wakeup(xq); /* do we need wakeup for every submit? */
    if (stats) {
      stats->tx_packets += tx;
      stats->tx_bytes += tx_bytes;
    }
    xq->stat_tx_submit++;
    xq->stat_tx_pkts += tx;
    xq->stat_tx_bytes += tx_bytes;
  } else {
    xdp_tx_poll_done(xq);
  }
  return tx;
}

static uint16_t xdp_rx(struct mtl_main_impl* impl, struct mt_xdp_queue* xq,
                       struct rte_mbuf** rx_pkts, uint16_t nb_pkts) {
  enum mtl_port port = xq->port;
  uint16_t q = xq->q;
  struct xsk_ring_cons* rx_cons = &xq->rx_cons;
  struct rte_mempool* mp = xq->mbuf_pool;
  struct mtl_port_status* stats = mt_if(impl, port)->dev_stats_sw;
  uint64_t rx_bytes = 0;
  uint32_t idx = 0;
  uint32_t rx = xsk_ring_cons__peek(rx_cons, nb_pkts, &idx);
  if (!rx) return 0;

  xq->stat_rx_burst++;

  struct rte_mbuf* fill[rx];
  int ret = rte_pktmbuf_alloc_bulk(xq->mbuf_pool, fill, rx);
  if (ret < 0) {
    dbg("%s(%d, %u), mbuf alloc bulk %u fail\n", __func__, port, q, rx);
    xq->stat_rx_mbuf_alloc_fail++;
    xsk_ring_cons__cancel(rx_cons, rx);
    return 0;
  }

  for (uint32_t i = 0; i < rx; i++) {
    const struct xdp_desc* desc;
    uint64_t addr;
    uint32_t len;
    uint64_t offset;

    desc = xsk_ring_cons__rx_desc(rx_cons, idx++);
    addr = desc->addr;
    len = desc->len;
    offset = xsk_umem__extract_offset(addr);
    addr = xsk_umem__extract_addr(addr);
    struct rte_mbuf* pkt = xsk_umem__get_data(xq->umem_buffer, addr + mp->header_size);
    pkt->data_off =
        offset - sizeof(struct rte_mbuf) - rte_pktmbuf_priv_size(mp) - mp->header_size;
    rte_pktmbuf_pkt_len(pkt) = len;
    rte_pktmbuf_data_len(pkt) = len;
    rx_pkts[i] = pkt;
    rx_bytes += len;
  }

  xsk_ring_cons__release(rx_cons, nb_pkts);
  ret = xdp_rx_prod_reserve(xq, fill, rx);
  if (ret < 0) { /* should never happen */
    err("%s(%d, %u), prod fill bulk %u fail\n", __func__, port, q, rx);
    xq->stat_rx_prod_reserve_fail++;
  }

  if (stats) {
    stats->rx_packets += rx;
    stats->rx_bytes += rx_bytes;
  }
  xq->stat_rx_pkts += rx;
  xq->stat_rx_bytes += rx_bytes;

  return rx;
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
  xdp->queues_cnt = RTE_MAX(inf->nb_tx_q, inf->nb_rx_q);
  mt_pthread_mutex_init(&xdp->queues_lock, NULL);

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
    xq->tx_free_thresh = 0; /* default check free always */
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

  ret = mt_stat_register(impl, xdp_stat_dump, xdp, "xdp");
  if (ret < 0) {
    err("%s(%d), stat register fail %d\n", __func__, port, ret);
    xdp_free(xdp);
    return ret;
  }

  inf->xdp = xdp;
  info("%s(%d), start queue %u cnt %u\n", __func__, port, xdp->start_queue,
       xdp->queues_cnt);
  return 0;
}

int mt_dev_xdp_uinit(struct mt_interface* inf) {
  struct mt_xdp_priv* xdp = inf->xdp;
  if (!xdp) return 0;
  struct mtl_main_impl* impl = inf->parent;

  mt_stat_unregister(impl, xdp_stat_dump, xdp);

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
  mt_pthread_mutex_lock(&xdp->queues_lock);
  for (uint16_t i = 0; i < xdp->queues_cnt; i++) {
    if (!xdp->queues_info[i].tx_entry) {
      xq = &xdp->queues_info[i];
      xq->tx_entry = entry;
      break;
    }
  }
  mt_pthread_mutex_unlock(&xdp->queues_lock);
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

  /* poll all done buf */
  xdp_tx_poll_done(xq);
  xdp_queue_tx_stat(xq);

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
  mt_pthread_mutex_lock(&xdp->queues_lock);
  for (uint16_t i = 0; i < xdp->queues_cnt; i++) {
    if (!xdp->queues_info[i].rx_entry) {
      xq = &xdp->queues_info[i];
      xq->rx_entry = entry;
      break;
    }
  }
  mt_pthread_mutex_unlock(&xdp->queues_lock);
  if (!xq) {
    err("%s(%d), no free tx queue\n", __func__, port);
    mt_rx_xdp_put(entry);
    return NULL;
  }
  entry->xq = xq;
  entry->queue_id = xq->q;

  uint16_t q = entry->queue_id;
  /* create flow */
  entry->flow_rsp = mt_rx_flow_create(impl, port, q - xdp->start_queue, flow);
  if (!entry->flow_rsp) {
    err("%s(%d,%u), create flow fail\n", __func__, port, q);
    mt_rx_xdp_put(entry);
    return NULL;
  }

  uint8_t* ip = flow->dip_addr;
  info("%s(%d,%u), ip %u.%u.%u.%u port %u\n", __func__, port, q, ip[0], ip[1], ip[2],
       ip[3], flow->dst_port);
  return entry;
}

int mt_rx_xdp_put(struct mt_rx_xdp_entry* entry) {
  enum mtl_port port = entry->port;
  struct mt_rxq_flow* flow = &entry->flow;
  uint8_t* ip = flow->dip_addr;
  struct mt_xdp_queue* xq = entry->xq;

  if (entry->flow_rsp) {
    mt_rx_flow_free(entry->parent, port, entry->flow_rsp);
    entry->flow_rsp = NULL;
  }
  xdp_queue_rx_stat(xq);
  if (xq) xq->rx_entry = NULL;
  info("%s(%d), ip %u.%u.%u.%u, port %u, queue %u\n", __func__, port, ip[0], ip[1], ip[2],
       ip[3], flow->dst_port, entry->queue_id);
  mt_rte_free(entry);
  return 0;
}

uint16_t mt_rx_xdp_burst(struct mt_rx_xdp_entry* entry, struct rte_mbuf** rx_pkts,
                         const uint16_t nb_pkts) {
  return xdp_rx(entry->parent, entry->xq, rx_pkts, nb_pkts);
}
