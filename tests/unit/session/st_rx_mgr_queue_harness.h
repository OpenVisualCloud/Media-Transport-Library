/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * C harness for the rx queue of an audio or ancillary session manager: a fake rx
 * queue entry hands out synthetic udp packets, and the dispatch records the keys and
 * sequence numbers it gets, so st_rx_mgr_queue_tasklet() runs without a NIC.
 */

#ifndef _UT_ST_RX_MGR_QUEUE_HARNESS_H_
#define _UT_ST_RX_MGR_QUEUE_HARNESS_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int ut_mq_init(void);
/* An empty manager queue on port P with no packets and no busy session. */
void ut_mq_reset(void);
/* Free the packets that the queue still holds, with st_rx_mgr_queue_uinit(). */
void ut_mq_teardown(void);
/* Give the packets of udp_port to the session of key. */
void ut_mq_map(uint16_t udp_port, uint32_t key);
/* Put one packet to udp_port into the fake rx queue, with the next sequence number. */
int ut_mq_feed(uint16_t udp_port);
/* Put one packet shorter than the udp headers into the fake rx queue. */
int ut_mq_feed_short(void);
/* The dispatch of key says the session lock is taken, times times (-1: always); key -1
 * for none. */
void ut_mq_set_busy(int64_t key, int times);
/* One st_rx_mgr_queue_tasklet() call with at most burst packets. */
int ut_mq_tasklet(uint16_t burst);
int ut_mq_got_n(void);
uint32_t ut_mq_got(int i);
/* The sequence number of the i-th packet the dispatch gave to a session. */
uint32_t ut_mq_got_seq(int i);
/* Packets left in the fake rx queue. */
int ut_mq_pending(void);
uint16_t ut_mq_deferred(void);
uint64_t ut_mq_dropped(void);
/* Mbufs of the test pool in use. */
unsigned int ut_mq_mbufs_in_use(void);
/* st_rx_mgr_queue_add() for udp_port: -EEXIST when its map entry is taken, else -EIO
 * (the harness has no port for the flow rule) */
int ut_mq_add(uint16_t udp_port, uint32_t key);
/* the map entry of udp_port: key + 1, 0 for none */
uint32_t ut_mq_map_get(uint16_t udp_port);

#ifdef __cplusplus
}
#endif

#endif /* _UT_ST_RX_MGR_QUEUE_HARNESS_H_ */
