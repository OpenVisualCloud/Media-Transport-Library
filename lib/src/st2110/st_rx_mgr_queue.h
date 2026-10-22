/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#ifndef _ST_LIB_RX_MGR_QUEUE_HEAD_H_
#define _ST_LIB_RX_MGR_QUEUE_HEAD_H_

#include "st_main.h"

/*
 * One rx queue per port for the frame level audio or ancillary sessions of one session
 * manager. Each session adds its flow rule to the queue, and the manager tasklet reads
 * the queue once per call and gives each packet to its session: polling one hardware
 * queue per session took most of the scheduler loop, mostly on empty queues.
 *
 * A session keeps an rx queue of its own, as before, on a port that cannot use the queue
 * (kernel socket, native AF_XDP, AF_PACKET, shared RSS, MTL_FLAG_SHARED_RX_QUEUE, CNI rx,
 * a flow rule without the IP), and when it is RTP level (the app ring holds the mbufs of
 * the shared pool), data path only, has the audio timing parser, or uses a udp port that
 * another session of the manager has.
 *
 * The queue and its 256 KiB map stay until the manager uinit, when the scheduler is
 * freed (for scheduler 0 that is mtl_uninit). All the flows of the manager share the
 * queue: a flood on one flow also drops the packets of the others.
 *
 * The packets of a session whose slot lock is held wait in one list of
 * ST_RX_MGR_QUEUE_DEFER_MAX per port, which all the sessions of the manager share, and
 * drop when it is full. Attach and update_src hold the slot lock while the flow rule of
 * the session already steers its packets to the queue (the iavf flow create sleeps 5 ms),
 * so the first packets of that session wait in the list.
 */

#define ST_RX_MGR_QUEUE_BURST_MAX (32)
/* the vector rx paths of iavf and ice read a multiple of 8 packets: a burst of less reads
 * none */
#define ST_RX_MGR_QUEUE_BURST_MIN (8)

/* give pkt to the session of key: 0 done, -ENOENT no such session, -EBUSY try later */
typedef int (*st_rx_mgr_queue_dispatch_fn)(void* priv, enum mtl_port port, uint32_t key,
                                           struct rte_mbuf* pkt);

/* control plane, with the manager mutex held: the queue of port, once per manager */
int st_rx_mgr_queue_prepare(struct mtl_main_impl* impl, struct st_rx_mgr_queue* q,
                            enum mtl_port port);
/* control plane: steer flow to the queue, key is what the dispatch gets */
int st_rx_mgr_queue_add(struct mtl_main_impl* impl, struct st_rx_mgr_queue* q,
                        enum mtl_port port, struct mt_rxq_flow* flow, uint32_t key,
                        struct mt_rx_flow_rsp** rsp);
void st_rx_mgr_queue_del(struct mtl_main_impl* impl, struct st_rx_mgr_queue* q,
                         enum mtl_port port, uint16_t dst_port, uint32_t key,
                         struct mt_rx_flow_rsp* rsp);
/*
 * data plane, from the manager tasklet: the kept packets first, then the queue of port.
 * A call gives at most burst kept packets to the sessions, and reads the queue only
 * when it gave less.
 */
int st_rx_mgr_queue_tasklet(struct st_rx_mgr_queue* q, enum mtl_port port, uint16_t burst,
                            st_rx_mgr_queue_dispatch_fn fn, void* priv);
/* control plane, after the tasklet is gone */
void st_rx_mgr_queue_uinit(struct st_rx_mgr_queue* q);

#endif
