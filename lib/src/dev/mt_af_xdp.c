/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 * The data path based on linux kernel socket interface
 */

#include "mt_af_xdp.h"

#include <linux/if_link.h>
#include <linux/if_xdp.h>
#include <sys/un.h>
#include <xdp/xsk.h>

#include "../mt_flow.h"
#include "../mt_instance.h"
#include "../mt_log.h"
#include "../mt_stat.h"
#include "../mt_util.h"

#ifndef XDP_UMEM_UNALIGNED_CHUNK_FLAG
#error "Please use XDP lib version with XDP_UMEM_UNALIGNED_CHUNK_FLAG support"
#endif

#define XDP_F_ZERO_COPY (MTL_BIT32(0))
#define XDP_F_RATE_LIMIT (MTL_BIT32(1))

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
  uint32_t tx_full_thresh;

  struct mt_tx_xdp_entry* tx_entry;
  struct mt_rx_xdp_entry* rx_entry;

  uint64_t stat_tx_pkts;
  uint64_t stat_tx_bytes;
  uint64_t stat_tx_free;
  uint64_t stat_tx_submit;
  uint64_t stat_tx_copy;
  uint64_t stat_tx_wakeup;
  uint64_t stat_tx_wakeup_fail;
  uint64_t stat_tx_mbuf_alloc_fail;
  uint64_t stat_tx_prod_reserve_fail;
  uint64_t stat_tx_prod_full;

  uint64_t stat_rx_pkts;
  uint64_t stat_rx_bytes;
  uint64_t stat_rx_burst;
  uint64_t stat_rx_mbuf_alloc_fail;
  uint64_t stat_rx_prod_reserve_fail;

  uint32_t stat_rx_pkt_invalid;
  uint32_t stat_rx_pkt_err_udp_port;
};

struct mt_xdp_priv {
  struct mtl_main_impl* parent;
  enum mtl_port port;
  char drv[32];
  uint32_t flags; /* XDP_F_* */
  unsigned int ifindex;

  uint16_t queues_cnt;

  struct mt_xdp_queue* queues_info;
  pthread_mutex_t queues_lock;

  bool has_ctrl;
};

static int xdp_queue_tx_max_rate(struct mt_xdp_priv* xdp, struct mt_xdp_queue* xq,
                                 uint32_t rate_kbps) {
  enum mtl_port port = xq->port;
  const char* if_name = mt_kernel_if_name(xdp->parent, port);

  char path[128];
  snprintf(path, sizeof(path), "/sys/class/net/%s/queues/tx-%u/tx_maxrate", if_name,
           xq->q);
  int ret = mt_sysfs_write_uint32(path, rate_kbps | MTL_BIT32(31));
  info("%s(%d), write %u to %s ret %d\n", __func__, port, rate_kbps, path, ret);
  return ret;
}

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
  if (xq->stat_tx_prod_full) {
    info("%s(%d,%u), tx prod full %" PRIu64 "\n", __func__, port, q,
         xq->stat_tx_prod_full);
    xq->stat_tx_prod_full = 0;
  }
  if (xq->stat_tx_wakeup_fail) {
    warn("%s(%d,%u), tx wakeup fail %" PRIu64 "\n", __func__, port, q,
         xq->stat_tx_wakeup_fail);
    xq->stat_tx_wakeup_fail = 0;
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
  if (xq->stat_rx_pkt_invalid) {
    err("%s(%d,%u), invalid pkt %u wrong udp port %u\n", __func__, port, q,
        xq->stat_rx_pkt_invalid, xq->stat_rx_pkt_err_udp_port);
    xq->stat_rx_pkt_invalid = 0;
    xq->stat_rx_pkt_err_udp_port = 0;
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

static void xdp_queue_clean_mbuf(struct mt_xdp_queue* xq) {
  struct xsk_ring_prod* rpq = &xq->rx_prod;
  struct rte_mempool* mp = xq->mbuf_pool;
  uint32_t size = xq->umem_ring_size * 2 + 128 /* max burst size */;
  uint32_t idx = 0;

  /* clean rx prod ring */
  xsk_ring_prod__reserve(rpq, 0, &idx);
  for (uint32_t i = 0; i < size; i++) {
    __u64* fq_addr = xsk_ring_prod__fill_addr(rpq, idx--);
    if (!fq_addr) break;
    struct rte_mbuf* m = *fq_addr + xq->umem_buffer + mp->header_size;
    rte_pktmbuf_free(m);
  }
}

static int xdp_queue_uinit(struct mt_xdp_queue* xq) {
  xdp_queue_clean_mbuf(xq);

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

      mt_instance_put_queue(xdp->parent, xdp->ifindex, xq->q);
    }
    mt_rte_free(xdp->queues_info);
    xdp->queues_info = NULL;
  }

  mt_pthread_mutex_destroy(&xdp->queues_lock);
  mt_rte_free(xdp);
  return 0;
}

static int xdp_parse_drv_name(struct mt_xdp_priv* xdp) {
  struct mtl_main_impl* impl = xdp->parent;
  enum mtl_port port = xdp->port;
  const char* if_name = mt_kernel_if_name(impl, port);
  char drv_path[128];
  char link_path[128];

  snprintf(drv_path, sizeof(drv_path), "/sys/class/net/%s/device/driver", if_name);
  ssize_t len = readlink(drv_path, link_path, sizeof(link_path));
  if (len < 0) {
    warn("%s(%d), readlink fail for %s\n", __func__, port, if_name);
    goto unknown;
  }
  link_path[len] = '\0';
  char* driver_name = basename(link_path);
  if (!driver_name) {
    warn("%s(%d), basename fail for %s\n", __func__, port, if_name);
    goto unknown;
  }

  snprintf(xdp->drv, sizeof(xdp->drv), "%s", driver_name);
  info("%s(%d), if:%s drv:%s\n", __func__, port, if_name, xdp->drv);
  return 0;

unknown:
  snprintf(xdp->drv, sizeof(xdp->drv), "%s", "unknown");
  return -EIO;
}

static int xdp_parse_pacing_ice(struct mt_xdp_priv* xdp) {
  struct mtl_main_impl* impl = xdp->parent;
  enum mtl_port port = xdp->port;
  const char* if_name = mt_kernel_if_name(impl, port);

  uint32_t rate = MTL_BIT32(31);
  char path[128];
  snprintf(path, sizeof(path), "/sys/class/net/%s/queues/tx-%u/tx_maxrate", if_name, 1);
  int ret = mt_sysfs_write_uint32(path, rate);
  info("%s(%d), rl feature %s\n", __func__, port, ret < 0 ? "no" : "yes");
  if (ret >= 0) {
    xdp->flags |= XDP_F_RATE_LIMIT;
    mt_if(impl, port)->drv_info.rl_type = MT_RL_TYPE_XDP_QUEUE_SYSFS;
  }
  return ret;
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
    if (ret == -EPERM)
      err("%s(%d,%u), please add capability for the app: sudo setcap 'cap_net_raw+ep' "
          "<app>\n",
          __func__, port, q);
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

static int xdp_socket_update_xskmap(struct mtl_main_impl* impl, struct mt_xdp_queue* xq,
                                    const char* ifname) {
  enum mtl_port port = xq->port;
  uint16_t q = xq->q;
  int ret;

  int xsks_map_fd = mt_instance_request_xsks_map_fd(impl, if_nametoindex(ifname));

  if (xsks_map_fd < 0) {
    err("%s(%d,%u), get xsks_map_fd fail\n", __func__, port, q);
    return -EIO;
  }

  ret = xsk_socket__update_xskmap(xq->socket, xsks_map_fd);
  if (ret) {
    err("%s(%d,%u), update xsks_map fail, %d\n", __func__, port, q, ret);
    return ret;
  }

  return 0;
}

static int xdp_socket_init(struct mt_xdp_priv* xdp, struct mt_xdp_queue* xq) {
  enum mtl_port port = xq->port;
  uint16_t q = xq->q;
  struct mtl_main_impl* impl = xdp->parent;
  const char* if_name = mt_kernel_if_name(impl, port);
  struct xsk_socket_config cfg;
  int ret;

  memset(&cfg, 0, sizeof(cfg));
  cfg.rx_size = mt_if_nb_rx_desc(impl, port);
  cfg.tx_size = mt_if_nb_tx_desc(impl, port);
  cfg.xdp_flags = XDP_FLAGS_UPDATE_IF_NOEXIST;
  if (xdp->has_ctrl) /* this will skip load xdp prog */
    cfg.libxdp_flags = XSK_LIBXDP_FLAGS__INHIBIT_PROG_LOAD;
  // cfg.bind_flags = XDP_USE_NEED_WAKEUP;

  if (!mt_user_af_xdp_zc(impl)) {
    warn("%s(%d,%u), user special to copy mode only\n", __func__, port, q);
    ret = -EAGAIN;
    goto copy_mode;
  }

  /* first try zero copy mode */
  cfg.bind_flags |= XDP_ZEROCOPY; /* force zero copy mode */
  ret = xsk_socket__create(&xq->socket, if_name, q, xq->umem, &xq->rx_cons, &xq->tx_prod,
                           &cfg);
  if (ret < 0) {
    if (ret == -EPERM) {
      err("%s(%d,%u), please run with mtl manager or root user\n", __func__, port, q);
      return ret;
    }
    warn("%s(%d,%u), xsk create with zero copy fail %d(%s), try copy mode\n", __func__,
         port, q, ret, strerror(ret));
  } else {
    xdp->flags |= XDP_F_ZERO_COPY;
  }

copy_mode:
  /* try copy mode */
  if (ret < 0) {
    cfg.bind_flags &= ~XDP_ZEROCOPY; /* clear zero copy */
    ret = xsk_socket__create(&xq->socket, if_name, q, xq->umem, &xq->rx_cons,
                             &xq->tx_prod, &cfg);
    if (ret < 0) {
      if (ret == -EPERM) {
        err("%s(%d,%u), please run with mtl manager or root user\n", __func__, port, q);
      }
      err("%s(%d,%u), xsk create fail %d(%s)\n", __func__, port, q, ret, strerror(ret));
      return ret;
    }
  }

  xq->socket_fd = xsk_socket__fd(xq->socket);

  if (xdp->has_ctrl) return xdp_socket_update_xskmap(impl, xq, if_name);

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
  if (xsk_ring_prod__needs_wakeup(&xq->tx_prod)) {
    int ret = send(xq->socket_fd, NULL, 0, MSG_DONTWAIT);
    xq->stat_tx_wakeup++;
    dbg("%s(%d, %u), wake up %d\n", __func__, port, q, ret);
    if (ret < 0) {
      dbg("%s(%d, %u), wake up fail %d(%s)\n", __func__, xq->port, xq->q, ret,
          strerror(errno));
      xq->stat_tx_wakeup_fail++;
    }
  }
}

static uint16_t xdp_tx(struct mtl_main_impl* impl, struct mt_xdp_queue* xq,
                       struct rte_mbuf** tx_pkts, uint16_t nb_pkts) {
  enum mtl_port port = xq->port;
  // uint16_t q = xq->q;
  struct rte_mempool* mbuf_pool = xq->mbuf_pool;
  uint16_t tx = 0;
  struct xsk_ring_prod* pd = &xq->tx_prod;
  struct mtl_port_status* stats = mt_if(impl, port)->dev_stats_sw;
  uint64_t tx_bytes = 0;

  xdp_tx_check_free(xq); /* do we need check free threshold for every tx burst */

  uint32_t prod_free = xsk_prod_nb_free(&xq->tx_prod, xq->umem_ring_size);
  if (prod_free < xq->tx_full_thresh) { /* tx_prod is full */
    xq->stat_tx_prod_full++;
    return 0;
  }

  for (uint16_t i = 0; i < nb_pkts; i++) {
    struct rte_mbuf* m = tx_pkts[i];
    struct rte_mbuf* local = rte_pktmbuf_alloc(mbuf_pool);
    if (!local) {
      dbg("%s(%d, %u), local mbuf alloc fail\n", __func__, port, q);
      xq->stat_tx_mbuf_alloc_fail++;
      goto exit;
    }

    uint32_t idx;
    if (!xsk_ring_prod__reserve(pd, 1, &idx)) {
      dbg("%s(%d, %u), socket_tx reserve fail\n", __func__, port, q);
      xq->stat_tx_prod_reserve_fail++;
      rte_pktmbuf_free(local);
      xdp_tx_wakeup(xq);
      goto exit;
    }
    struct xdp_desc* desc = xsk_ring_prod__tx_desc(pd, idx);
    desc->len = m->pkt_len;
    uint64_t addr =
        (uint64_t)local - (uint64_t)xq->umem_buffer - xq->mbuf_pool->header_size;
    uint64_t offset =
        rte_pktmbuf_mtod(local, uint64_t) - (uint64_t)local + xq->mbuf_pool->header_size;
    void* pkt = xsk_umem__get_data(xq->umem_buffer, addr + offset);
    offset = offset << XSK_UNALIGNED_BUF_OFFSET_SHIFT;
    desc->addr = addr | offset;

    struct rte_mbuf* n = m;
    uint16_t nb_segs = m->nb_segs;
    for (uint16_t seg = 0; seg < nb_segs; seg++) {
      rte_memcpy(pkt, rte_pktmbuf_mtod(n, void*), n->data_len);
      pkt += n->data_len;
      /* point to next */
      n = n->next;
    }

    tx_bytes += desc->len;
    rte_pktmbuf_free(m);
    dbg("%s(%d, %u), tx local mbuf %p umem pkt %p addr 0x%" PRIu64 "\n", __func__, port,
        q, local, pkt, addr);
    xq->stat_tx_copy++;
    tx++;
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

static bool xdp_rx_check_pkt(struct mt_rx_xdp_entry* entry, struct rte_mbuf* pkt) {
  enum mtl_port port = entry->port;
  struct mt_xdp_queue* xq = entry->xq;
  uint16_t q = entry->queue_id;
  struct mt_udp_hdr* hdr = rte_pktmbuf_mtod(pkt, struct mt_udp_hdr*);
  struct rte_ether_hdr* eth = &hdr->eth;
  struct rte_ipv4_hdr* ipv4 = &hdr->ipv4;
  struct rte_udp_hdr* udp = &hdr->udp;

  MTL_MAY_UNUSED(port);
  MTL_MAY_UNUSED(q);

  uint16_t ether_type = ntohs(eth->ether_type);
  if (ether_type != RTE_ETHER_TYPE_IPV4) {
    dbg("%s(%d, %u), wrong ether_type %u\n", __func__, port, q, ether_type);
    return false;
  }

  if (ipv4->next_proto_id != IPPROTO_UDP) {
    dbg("%s(%d, %u), wrong next_proto_id %u\n", __func__, port, q, ipv4->next_proto_id);
    return false;
  }

  if (!entry->skip_udp_port_check) {
    uint16_t dst_port = ntohs(udp->dst_port);
    if (dst_port != entry->flow.dst_port) {
      xq->stat_rx_pkt_err_udp_port++;
      dbg("%s(%d, %u), wrong dst_port %u expect %u\n", __func__, port, q, dst_port,
          entry->flow.dst_port);
      return false;
    }
  }

  return true;
}

static uint16_t xdp_rx(struct mt_rx_xdp_entry* entry, struct rte_mbuf** rx_pkts,
                       uint16_t nb_pkts) {
  struct mt_xdp_queue* xq = entry->xq;
  enum mtl_port port = entry->port;
  uint16_t q = xq->q;
  struct xsk_ring_cons* rx_cons = &xq->rx_cons;
  struct rte_mempool* mp = xq->mbuf_pool;
  struct mtl_port_status* stats = mt_if(entry->parent, port)->dev_stats_sw;
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

  uint32_t valid_rx = 0;
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
    if (entry->skip_all_check || xdp_rx_check_pkt(entry, pkt)) {
      rx_pkts[valid_rx] = pkt;
      valid_rx++;
    } else {
      rte_pktmbuf_free(pkt);
      xq->stat_rx_pkt_invalid++;
    }
    rx_bytes += len;
  }

  xsk_ring_cons__release(rx_cons, rx);
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

  return valid_rx;
}

int mt_dev_xdp_init(struct mt_interface* inf) {
  struct mtl_main_impl* impl = inf->parent;
  enum mtl_port port = inf->port;
  int ret;

  if (!mt_pmd_is_native_af_xdp(impl, port)) {
    err("%s(%d), not native af_xdp\n", __func__, port);
    return -EIO;
  }

  if (!mt_is_manager_connected(impl)) {
    err("%s(%d), AF_XDP backend must run with MTL Manager!\n", __func__, port);
    return -EIO;
  }

  struct mt_xdp_priv* xdp = mt_rte_zmalloc_socket(sizeof(*xdp), mt_socket_id(impl, port));
  if (!xdp) {
    err("%s(%d), xdp malloc fail\n", __func__, port);
    return -ENOMEM;
  }
  xdp->parent = impl;
  xdp->port = port;
  xdp->ifindex = if_nametoindex(mt_kernel_if_name(impl, port));
  xdp->queues_cnt = RTE_MAX(inf->nb_tx_q, inf->nb_rx_q);
  xdp->has_ctrl = true;
  mt_pthread_mutex_init(&xdp->queues_lock, NULL);

  xdp_parse_drv_name(xdp);

  xdp->queues_info = mt_rte_zmalloc_socket(sizeof(*xdp->queues_info) * xdp->queues_cnt,
                                           mt_socket_id(impl, port));
  if (!xdp->queues_info) {
    err("%s(%d), xdp queues_info malloc fail\n", __func__, port);
    xdp_free(xdp);
    return -ENOMEM;
  }
  for (uint16_t i = 0; i < xdp->queues_cnt; i++) {
    int q = mt_instance_get_queue(impl, xdp->ifindex);
    if (q < 0) {
      err("%s(%d), no free queue found\n", __func__, port);
      xdp_free(xdp);
      return -EIO;
    }

    struct mt_xdp_queue* xq = &xdp->queues_info[i];
    xq->port = port;
    xq->q = q;
    xq->umem_ring_size = XSK_RING_CONS__DEFAULT_NUM_DESCS;
    xq->tx_free_thresh = 0; /* default check free always */
    xq->tx_full_thresh = 1;
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

  if (0 == strncmp(xdp->drv, "ice", sizeof("ice"))) xdp_parse_pacing_ice(xdp);

  inf->port_id = inf->port;
  inf->xdp = xdp;
  inf->feature |= MT_IF_FEATURE_TX_MULTI_SEGS;
  info("%s(%d), cnt %u\n", __func__, port, xdp->queues_cnt);
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
                                      struct mt_txq_flow* flow,
                                      struct mt_tx_xdp_get_args* args) {
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

  if (args && args->queue_match) {
    mt_pthread_mutex_lock(&xdp->queues_lock);
    xq = &xdp->queues_info[args->queue_id];
    if (xq->tx_entry) {
      err("%s(%d), q %u is already used\n", __func__, port, args->queue_id);
      mt_pthread_mutex_unlock(&xdp->queues_lock);
      mt_tx_xdp_put(entry);
      return NULL;
    }
    xq->tx_entry = entry;
    mt_pthread_mutex_unlock(&xdp->queues_lock);
  } else {
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
  }

  entry->xq = xq;
  entry->queue_id = xq->q;

  /* rl settings */
  if (xdp->flags & XDP_F_RATE_LIMIT) {
    uint32_t rate_kbps = 0;
    if (mt_if(impl, port)->tx_pacing_way == ST21_TX_PACING_WAY_RL) {
      rate_kbps = flow->bytes_per_sec / 1000 * 8;
    }
    xdp_queue_tx_max_rate(xdp, xq, rate_kbps);
  }

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
  struct mt_xdp_priv* xdp = mt_if(entry->parent, port)->xdp;

  /* rl settings clear */
  if (xdp->flags & XDP_F_RATE_LIMIT) {
    xdp_queue_tx_max_rate(xdp, xq, 0);
  }

  if (xq) {
    /* poll all done buf */
    xdp_tx_poll_done(xq);
    xdp_queue_tx_stat(xq);

    xq->tx_entry = NULL;
    info("%s(%d), ip %u.%u.%u.%u, port %u, queue %u\n", __func__, port, ip[0], ip[1],
         ip[2], ip[3], flow->dst_port, entry->queue_id);
  }

  mt_rte_free(entry);
  return 0;
}

uint16_t mt_tx_xdp_burst(struct mt_tx_xdp_entry* entry, struct rte_mbuf** tx_pkts,
                         uint16_t nb_pkts) {
  return xdp_tx(entry->parent, entry->xq, tx_pkts, nb_pkts);
}

static inline int xdp_socket_update_dp(struct mtl_main_impl* impl, int ifindex,
                                       uint16_t dp, bool add) {
  return mt_instance_update_udp_dp_filter(impl, ifindex, dp, add);
}

struct mt_rx_xdp_entry* mt_rx_xdp_get(struct mtl_main_impl* impl, enum mtl_port port,
                                      struct mt_rxq_flow* flow,
                                      struct mt_rx_xdp_get_args* args) {
  if (!mt_pmd_is_native_af_xdp(impl, port)) {
    err("%s(%d), this pmd is not native xdp\n", __func__, port);
    return NULL;
  }

  MTL_MAY_UNUSED(args);

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

  if (args && args->queue_match) {
    mt_pthread_mutex_lock(&xdp->queues_lock);
    xq = &xdp->queues_info[args->queue_id];
    if (xq->rx_entry) {
      err("%s(%d), q %u is already used\n", __func__, port, args->queue_id);
      mt_pthread_mutex_unlock(&xdp->queues_lock);
      mt_rx_xdp_put(entry);
      return NULL;
    }
    xq->rx_entry = entry;
    mt_pthread_mutex_unlock(&xdp->queues_lock);
  } else {
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
      err("%s(%d), no free rx queue\n", __func__, port);
      mt_rx_xdp_put(entry);
      return NULL;
    }
  }

  entry->xq = xq;
  entry->queue_id = xq->q;
  entry->skip_udp_port_check = args ? args->skip_udp_port_check : false;

  uint16_t q = entry->queue_id;

  if (!args || !args->skip_flow) {
    /* create flow */
    entry->flow_rsp = mt_rx_flow_create(impl, port, q, flow);
    if (!entry->flow_rsp) {
      err("%s(%d,%u), create flow fail\n", __func__, port, q);
      mt_rx_xdp_put(entry);
      return NULL;
    }
    if (xdp->has_ctrl && !xdp_socket_update_dp(impl, xdp->ifindex, flow->dst_port, true))
      entry->skip_all_check = true;
    else
      entry->skip_all_check = false;
  }

  /* join multicast group, will drop automatically when socket fd closed */
  int mcast_fd = -1;
  if (mt_is_multicast_ip(flow->dip_addr)) {
    int ret;
    mcast_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (mcast_fd < 0) {
      err("%s(%d,%u), create multicast socket fail\n", __func__, port, q);
      mt_rx_xdp_put(entry);
      return NULL;
    }
    uint32_t source = *(uint32_t*)flow->sip_addr;
    if (source == 0) {
      struct ip_mreq mreq;
      memset(&mreq, 0, sizeof(mreq));
      memcpy(&mreq.imr_multiaddr.s_addr, flow->dip_addr, MTL_IP_ADDR_LEN);
      memcpy(&mreq.imr_interface.s_addr, mt_sip_addr(impl, port), MTL_IP_ADDR_LEN);
      ret = setsockopt(mcast_fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
    } else {
      struct ip_mreq_source mreq;
      memset(&mreq, 0, sizeof(mreq));
      memcpy(&mreq.imr_multiaddr.s_addr, flow->dip_addr, MTL_IP_ADDR_LEN);
      memcpy(&mreq.imr_interface.s_addr, mt_sip_addr(impl, port), MTL_IP_ADDR_LEN);
      memcpy(&mreq.imr_sourceaddr.s_addr, flow->sip_addr, MTL_IP_ADDR_LEN);
      ret =
          setsockopt(mcast_fd, IPPROTO_IP, IP_ADD_SOURCE_MEMBERSHIP, &mreq, sizeof(mreq));
    }
    if (ret < 0) {
      err("%s(%d), join multicast fail %d\n", __func__, port, ret);
      mt_rx_xdp_put(entry);
      return NULL;
    }
    info("%s(%d), join multicast succ\n", __func__, port);
  }
  entry->mcast_fd = mcast_fd;

  uint8_t* ip = flow->dip_addr;
  info("%s(%d,%u), ip %u.%u.%u.%u port %u\n", __func__, port, q, ip[0], ip[1], ip[2],
       ip[3], flow->dst_port);
  return entry;
}

int mt_rx_xdp_put(struct mt_rx_xdp_entry* entry) {
  struct mtl_main_impl* impl = entry->parent;
  enum mtl_port port = entry->port;
  struct mt_rxq_flow* flow = &entry->flow;
  uint8_t* ip = flow->dip_addr;
  struct mt_xdp_queue* xq = entry->xq;
  struct mt_xdp_priv* xdp = mt_if(impl, port)->xdp;

  if (entry->mcast_fd > 0) {
    close(entry->mcast_fd);
  }

  if (entry->flow_rsp) {
    mt_rx_flow_free(impl, port, entry->flow_rsp);
    entry->flow_rsp = NULL;
    if (xdp->has_ctrl) xdp_socket_update_dp(impl, xdp->ifindex, flow->dst_port, false);
  }
  if (xq) {
    xdp_queue_rx_stat(xq);
    xq->rx_entry = NULL;
  }
  info("%s(%d), ip %u.%u.%u.%u, port %u, queue %u\n", __func__, port, ip[0], ip[1], ip[2],
       ip[3], flow->dst_port, entry->queue_id);
  mt_rte_free(entry);
  return 0;
}

uint16_t mt_rx_xdp_burst(struct mt_rx_xdp_entry* entry, struct rte_mbuf** rx_pkts,
                         const uint16_t nb_pkts) {
  return xdp_rx(entry, rx_pkts, nb_pkts);
}
