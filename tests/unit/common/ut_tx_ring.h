/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * A model of the DPDK iavf/ice scalar TX ring over real mbufs: ci_xmit_pkts() with one
 * ci_tx_xmit_cleanup() a call while nb_tx_free < tx_free_thresh, the RS bit on the
 * descriptor that crosses an rs bucket, and iavf_tx_done_cleanup_full(). A descriptor
 * completes drain_ns after the one before it, never on a UT_TXR_HUNG ring.
 *
 * UT_TXR_FREE_ON_CLEANUP is DPDK 26.07: the cleanup frees a done batch's mbufs and
 * leaves them in the sw_ring. UT_TXR_FREE_ON_REUSE is the PMDs before: the cleanup only
 * counts the batch free, and xmit frees an mbuf when it reuses the slot.
 *
 * The ut_eth_*() seams route rte_eth_tx_burst() and rte_eth_tx_done_cleanup() of a
 * harnessed production file to the ring attached to that port and queue, at
 * ut_clock_ns().
 */

#ifndef _UT_TX_RING_H_
#define _UT_TX_RING_H_

#include <rte_mbuf.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UT_TXR_HUNG UINT64_MAX

enum ut_txr_mode {
  UT_TXR_FREE_ON_CLEANUP,
  UT_TXR_FREE_ON_REUSE,
};

struct ut_txr;

struct ut_txr* ut_txr_create(enum ut_txr_mode mode, uint16_t nb_desc, uint16_t rs_thresh,
                             uint16_t free_thresh, uint64_t drain_ns);
/** Frees the mbufs the ring still holds. */
void ut_txr_destroy(struct ut_txr* r);
/** For the descriptors posted from now on. */
void ut_txr_set_drain_ns(struct ut_txr* r, uint64_t drain_ns);
uint16_t ut_txr_xmit(struct ut_txr* r, struct rte_mbuf** pkts, uint16_t nb_pkts,
                     uint64_t now_ns);
int ut_txr_done_cleanup(struct ut_txr* r, uint32_t free_cnt, uint64_t now_ns);

/** Descriptors xmit posted. */
uint64_t ut_txr_descs(const struct ut_txr* r);
uint64_t ut_txr_xmits(const struct ut_txr* r);
/** Slots done_cleanup freed again after the cleanup freed them; not freed twice here. */
uint64_t ut_txr_double_frees(const struct ut_txr* r);
/** The live slots still pointing at m. */
uint16_t ut_txr_holds(const struct ut_txr* r, const struct rte_mbuf* m);
uint16_t ut_txr_in_use(const struct ut_txr* r);

/** The ring rte_eth_tx_burst() and rte_eth_tx_done_cleanup() reach for that port and
 * queue, NULL to detach. */
void ut_eth_attach_tx_ring(uint16_t port_id, uint16_t queue_id, struct ut_txr* r);
uint16_t ut_eth_tx_burst(uint16_t port_id, uint16_t queue_id, struct rte_mbuf** tx_pkts,
                         uint16_t nb_pkts);
int ut_eth_tx_done_cleanup(uint16_t port_id, uint16_t queue_id, uint32_t free_cnt);
/** rte_eth_tx_done_cleanup() calls since the last reset, ring attached or not. */
int ut_eth_tx_done_cleanup_calls(void);
/** Detaches every ring and zeroes the call count. */
void ut_eth_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* _UT_TX_RING_H_ */
