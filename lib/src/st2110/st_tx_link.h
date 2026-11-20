/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#ifndef _ST_LIB_TX_LINK_HEAD_H_
#define _ST_LIB_TX_LINK_HEAD_H_

#include "../dev/mt_dev.h"
#include "../mt_log.h"
#include "st_header.h"

/* the most time between two link reads of a TX port that fails its bursts or is down */
#define ST_TX_LINK_CHECK_NS ((uint64_t)100 * NS_PER_MS)

/* the burst fail time after which the transmitter reads its link */
static inline uint64_t st_tx_link_check_ns(uint64_t hang_detect_ns) {
  return RTE_MIN(hang_detect_ns, ST_TX_LINK_CHECK_NS);
}

/* For a port that fails its bursts: true when the link reads down. From then on the
 * transmitter drops its pkts at their time until st_tx_link_drop() reads the link up.
 * What the NIC ring holds, at most nb_tx_desc pkts, leaves late when the link is back:
 * no runtime queue stop is safe, the ixgbe stop of an E610 frees the ring while the HW
 * still holds it, and an iavf stop breaks the TM tree of the port. */
static inline bool st_tx_link_check_down(struct mtl_main_impl* impl, enum mtl_port port,
                                         struct st_tx_link* link, uint64_t now,
                                         const char* tag, int idx, int s_port) {
  if (link->down) return true;
  /* 0: never read */
  if (link->check_tsc && now - link->check_tsc < ST_TX_LINK_CHECK_NS) return false;
  link->check_tsc = now;
  if (!mt_dev_link_down(impl, port)) return false;
  link->down = true;
  link->down_tsc = now;
  warn("%s(%d,%d), link down, TX drops the pkts until the link is up\n", tag, idx,
       s_port);
  return true;
}

/* while the link is down: true to drop the burst; one link read a ST_TX_LINK_CHECK_NS */
static inline bool st_tx_link_drop(struct mtl_main_impl* impl, enum mtl_port port,
                                   struct st_tx_link* link, uint64_t now, const char* tag,
                                   int idx, int s_port) {
  if (now - link->check_tsc < ST_TX_LINK_CHECK_NS) return true;
  link->check_tsc = now;
  if (mt_dev_link_down(impl, port)) return true;
  link->down = false;
  notice("%s(%d,%d), link up, TX resumes after %" PRIu64 " ms\n", tag, idx, s_port,
         (now - link->down_tsc) / NS_PER_MS);
  return false;
}

#endif
