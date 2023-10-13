/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 * The data path based on linux kernel socket interface
 */

#include "mt_af_xdp.h"

#include <linux/ethtool.h>
#include <linux/if_xdp.h>
#include <linux/sockios.h>
#include <xdp/xsk.h>

#include "../mt_log.h"

#ifndef XDP_UMEM_UNALIGNED_CHUNK_FLAG
#error "Please use XDP lib version with XDP_UMEM_UNALIGNED_CHUNK_FLAG"
#endif

struct mt_xdp_queue {
  struct rte_mempool* mbuf_pool;
  struct xsk_umem* umem;
  struct xsk_ring_prod pq;
  struct xsk_ring_cons cq;
  void* umem_buffer;

  struct xsk_socket* xsk;
};

struct mt_xdp_priv {
  struct mtl_main_impl* parent;
  enum mtl_port port;
  uint8_t start_queue;
  uint16_t queues_cnt;
  uint32_t max_combined;
  uint32_t combined_count;

  struct mt_xdp_queue* queues_info;
};

static int xdp_free(struct mt_xdp_priv* xdp) {
  if (xdp->queues_info) {
    for (uint16_t q = 0; q < xdp->queues_cnt; q++) {
      struct mt_xdp_queue* xq = &xdp->queues_info[q];

      if (xq->umem) {
        xsk_umem__delete(xq->umem);
        xq->umem = NULL;
      }
    }
    mt_rte_free(xdp->queues_info);
    xdp->queues_info = NULL;
  }

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

static int xdp_umem_init(struct mt_xdp_priv* xdp, struct mt_xdp_queue* xq) {
  enum mtl_port port = xdp->port;
  int ret;
  struct xsk_umem_config cfg;
  void* base_addr = NULL;
  struct rte_mempool* pool = xq->mbuf_pool;
  uint64_t umem_size, align = 0;

  memset(&cfg, 0, sizeof(cfg));
  cfg.fill_size = XSK_RING_CONS__DEFAULT_NUM_DESCS * 2;
  cfg.comp_size = XSK_RING_CONS__DEFAULT_NUM_DESCS;
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
    err("%s(%d), umem create fail %d %s\n", __func__, port, ret, strerror(errno));
    return ret;
  }
  xq->umem_buffer = base_addr;

  info("%s(%d), umem %p buffer %p size %" PRIu64 "\n", __func__, port, xq->umem,
       xq->umem_buffer, umem_size);
  return 0;
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
  for (uint16_t q = 0; q < xdp->queues_cnt; q++) {
    struct mt_xdp_queue* xq = &xdp->queues_info[q];

    xq->mbuf_pool = inf->rx_queues[q].mbuf_pool;
    if (!xq->mbuf_pool) {
      err("%s(%d), no mbuf_pool for q %u\n", __func__, port, q);
      xdp_free(xdp);
      return -EIO;
    }
    ret = xdp_umem_init(xdp, xq);
    if (ret < 0) {
      err("%s(%d), umem init fail for q %u\n", __func__, port, q);
      xdp_free(xdp);
      return -EIO;
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

  uint8_t* ip = flow->dip_addr;
  info("%s(%d), ip %u.%u.%u.%u, port %u, queue %u\n", __func__, port, ip[0], ip[1], ip[2],
       ip[3], flow->dst_port, entry->queue_id);
  return entry;
}

int mt_tx_xdp_put(struct mt_tx_xdp_entry* entry) {
  mt_rte_free(entry);
  return 0;
}

uint16_t mt_tx_xdp_burst(struct mt_tx_xdp_entry* entry, struct rte_mbuf** tx_pkts,
                         uint16_t nb_pkts) {
  MTL_MAY_UNUSED(entry);
  rte_pktmbuf_free_bulk(tx_pkts, nb_pkts);
  return nb_pkts;
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

  uint8_t* ip = flow->dip_addr;
  info("%s(%d), ip %u.%u.%u.%u port %u queue %d\n", __func__, port, ip[0], ip[1], ip[2],
       ip[3], flow->dst_port, entry->queue_id);
  return entry;
}

int mt_rx_xdp_put(struct mt_rx_xdp_entry* entry) {
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
