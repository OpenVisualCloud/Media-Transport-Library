/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Intel Corporation
 */

#include "mt_pcap.h"

#include "mt_main.h"
#include "mt_util.h"

#ifdef MT_HAS_PCAPNG_TS

int mt_pcap_close(struct mt_pcap* pcap) {
  if (pcap->pcapng) {
    rte_pcapng_close(pcap->pcapng);
    pcap->pcapng = NULL;
  }
  if (pcap->mp) {
    mt_mempool_free(pcap->mp);
    pcap->mp = NULL;
  }
  mt_free(pcap);
  return 0;
}

struct mt_pcap* mt_pcap_open(struct mtl_main_impl* impl, enum mtl_port port, int fd) {
  int ret;
  struct mt_pcap* pcap = mt_zmalloc(sizeof(*pcap));
  if (!pcap) {
    err("%s(%d,%d), malloc pcap fail\n", __func__, port, fd);
    return NULL;
  }
  pcap->fd = fd;
  pcap->port = port;
  pcap->max_len = ST_PKT_MAX_ETHER_BYTES;

  char pool_name[ST_MAX_NAME_LEN];
  snprintf(pool_name, sizeof(pool_name), "mt_pcap_p%di%d", port, fd);
  pcap->mp = mt_mempool_create(impl, port, pool_name, 512, MT_MBUF_CACHE_SIZE, 0,
                               rte_pcapng_mbuf_size(pcap->max_len));
  if (!pcap->mp) {
    err("%s(%d,%d), failed to create mempool\n", __func__, port, fd);
    mt_pcap_close(pcap);
    return NULL;
  }

  pcap->pcapng = rte_pcapng_fdopen(fd, NULL, NULL, "imtl-rx-video", NULL);
  if (!pcap->pcapng) {
    err("%s(%d,%d), pcapng fdopen fail\n", __func__, port, fd);
    mt_pcap_close(pcap);
    return NULL;
  }

#if RTE_VERSION >= RTE_VERSION_NUM(25, 11, 0, 0)
  for (int i = 0; i < mt_num_ports(impl); i++) {
    ret = rte_pcapng_add_interface(pcap->pcapng, mt_port_id(impl, i), DLT_EN10MB, NULL,
                                   NULL, NULL);
    if (ret < 0) {
      warn("%s(%d), add interface fail %d on port %d\n", __func__, fd, ret, i);
    }
  }
#elif RTE_VERSION >= RTE_VERSION_NUM(23, 3, 0, 0)
  /* add all port interfaces */
  for (int i = 0; i < mt_num_ports(impl); i++) {
    ret = rte_pcapng_add_interface(pcap->pcapng, mt_port_id(impl, i), NULL, NULL, NULL);
    if (ret < 0) {
      warn("%s(%d), add interface fail %d on port %d\n", __func__, fd, ret, i);
    }
  }
#endif

  info("%s, succ pcap %p, fd %d\n", __func__, pcap, fd);
  return pcap;
}

uint16_t mt_pcap_dump(struct mtl_main_impl* impl, enum mtl_port port,
                      struct mt_pcap* pcap, struct rte_mbuf** mbufs, uint16_t nb) {
  struct rte_mbuf* pcapng_mbuf[nb];
  int pcapng_mbuf_cnt = 0;
  uint16_t port_id = mt_port_id(impl, port);
  struct rte_mbuf* pkt;
  struct rte_mbuf* mc;

  for (uint16_t i = 0; i < nb; i++) {
    pkt = mbufs[i];
    mc = rte_pcapng_copy_ts(port_id, 0, pkt, pcap->mp, pcap->max_len,
                            RTE_PCAPNG_DIRECTION_IN, NULL,
                            mt_mbuf_time_stamp(impl, pkt, port));
    if (!mc) {
      warn("%s(%d,%d), copy packet fail\n", __func__, port, pcap->fd);
      break;
    }
    pcapng_mbuf[pcapng_mbuf_cnt++] = mc;
  }

  ssize_t len = rte_pcapng_write_packets(pcap->pcapng, pcapng_mbuf, pcapng_mbuf_cnt);
  if (len <= 0) {
    warn("%s(%d,%d), write packet fail\n", __func__, port, pcap->fd);
  }
  rte_pktmbuf_free_bulk(&pcapng_mbuf[0], pcapng_mbuf_cnt);

  return pcapng_mbuf_cnt;
}

#endif
