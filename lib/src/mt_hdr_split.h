/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#ifndef _MT_LIB_HDR_SPLIT_HEAD_H_
#define _MT_LIB_HDR_SPLIT_HEAD_H_

#include "mt_main.h"

/* 4 frames of 1080p GPM_SL and the spare slots of a frame that starts unaligned */
#define MT_HDR_SPLIT_SLOTS_DEFAULT (5 * 4320)

/* the rx_stage of the ice bulk alloc Rx path, which holds mbufs out of the ring */
#define MT_HDR_SPLIT_RX_STAGE (32)
/* ICE_ETH_OVERHEAD: ethernet header, CRC and two VLAN tags */
#define MT_HDR_SPLIT_ETH_OVERHEAD (RTE_ETHER_HDR_LEN + RTE_ETHER_CRC_LEN + 2 * 4)

/* ice writes the payload at rte_mbuf_data_iova_default(), after the headroom, and up to
 * nic_buf_len bytes, which it takes from the header pool, not from the slot */
static inline uint32_t mt_hdr_split_stride(uint32_t slot_size, uint32_t nic_buf_len) {
  return RTE_CACHE_LINE_ROUNDUP(RTE_PKTMBUF_HEADROOM + RTE_MAX(slot_size, nic_buf_len));
}

/* payload mbufs the PMD can hold out of the pool, not counting the mbufs of one burst */
static inline uint32_t mt_hdr_split_in_use_max(uint16_t nb_rx_desc,
                                               uint16_t rx_free_thresh) {
  return nb_rx_desc + rx_free_thresh + MT_HDR_SPLIT_RX_STAGE;
}

/* Registers the mempool ops of the hdr split pool. Call it before rte_eal_init(): a
 * shared DPDK build fills the 16 entries of the ops table with its drivers. */
int mt_hdr_split_ops_register(void);
/* After rte_eal_init(): -ENOENT if the hdr split ops took the entry of the MTL default
 * ops. */
int mt_hdr_split_ops_check(void);

/* A pool of nb_mbufs pinned external mbufs, unbound: the mbufs point at nb_scratch
 * scratch buffers of scratch_size bytes. */
struct mt_hdr_split_pool* mt_hdr_split_pool_create(const char* name, uint32_t nb_mbufs,
                                                   uint32_t nb_scratch,
                                                   uint32_t scratch_size, int socket_id);
/* Returns -EBUSY and keeps the pool if a mbuf is still in use. */
int mt_hdr_split_pool_free(struct mt_hdr_split_pool* hp);

/* Points mbuf i at slot i of slots and restarts the dequeue at slot 0. Every mbuf must be
 * in the pool, so the rx queue that uses the pool must be stopped. */
int mt_hdr_split_bind(struct mt_hdr_split_pool* hp, struct mt_hdr_split_slots* slots);
/* Points the mbufs back at the scratch buffers, with the same condition. */
int mt_hdr_split_unbind(struct mt_hdr_split_pool* hp);

#endif
