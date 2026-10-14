/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Intel Corporation
 */

#ifndef _MT_LIB_PCAP_HEAD_H_
#define _MT_LIB_PCAP_HEAD_H_

#include <rte_pcapng.h>
#include <rte_version.h>

#include "mt_log.h"
#include "mt_main.h"

/* Patch sets 23.11 to 26.03 give MTL_DPDK_HAS_PCAPNG_TS and rte_pcapng_copy_ts(); the
 * 26.07.92 set and DPDK 26.11 add a timestamp argument to rte_pcapng_copy(). */
#ifdef MTL_DPDK_HAS_PCAPNG_TS
#define MT_HAS_PCAPNG_TS
#define mt_pcapng_copy rte_pcapng_copy_ts
#elif RTE_VERSION >= RTE_VERSION_NUM(26, 7, 92, 0)
#define MT_HAS_PCAPNG_TS
#define mt_pcapng_copy rte_pcapng_copy
#endif

#ifdef MT_HAS_PCAPNG_TS

struct mt_pcap {
  enum mtl_port port;
  int fd;
  uint32_t max_len;
  struct rte_mempool* mp;
  struct rte_pcapng* pcapng;
};

/* note: fd will be be closed in mt_pcap_close if the open succ */
struct mt_pcap* mt_pcap_open(struct mtl_main_impl* impl, enum mtl_port port, int fd);
int mt_pcap_close(struct mt_pcap* pcap);
uint16_t mt_pcap_dump(struct mtl_main_impl* impl, enum mtl_port port,
                      struct mt_pcap* pcap, struct rte_mbuf** mbufs, uint16_t nb);
#else
static inline struct mt_pcap* mt_pcap_open(struct mtl_main_impl* impl, enum mtl_port port,
                                           int fd) {
  MTL_MAY_UNUSED(impl);
  err("%s(%d,%d), no pcap support for this build\n", __func__, port, fd);
  return NULL;
}

static inline int mt_pcap_close(struct mt_pcap* pcap) {
  MTL_MAY_UNUSED(pcap);
  return -ENOTSUP;
}

static inline uint16_t mt_pcap_dump(struct mtl_main_impl* impl, enum mtl_port port,
                                    struct mt_pcap* pcap, struct rte_mbuf** mbufs,
                                    uint16_t nb) {
  MTL_MAY_UNUSED(impl);
  MTL_MAY_UNUSED(port);
  MTL_MAY_UNUSED(pcap);
  MTL_MAY_UNUSED(mbufs);
  MTL_MAY_UNUSED(nb);
  return -ENOTSUP;
}
#endif

#endif
