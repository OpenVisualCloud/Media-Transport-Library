/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#ifndef _ST_LIB_TX_LINK_HEAD_H_
#define _ST_LIB_TX_LINK_HEAD_H_

#include "../dev/mt_dev.h"
#include "../mt_log.h"
#include "st_header.h"

/* Follows the link flag of port into link; true when it changed. While down the
 * transmitter drops its pkts at their time. What the NIC ring holds, at most nb_tx_desc
 * pkts, leaves late when the link is back: no runtime TX queue stop is safe. */
static inline bool st_tx_link_changed(struct mtl_main_impl* impl, enum mtl_port port,
                                      struct st_tx_link* link, const char* tag, int idx,
                                      int s_port) {
  bool down = mt_dev_link_down(impl, port);

  if (likely(down == link->down)) return false;
  uint64_t now = mt_get_tsc(impl);
  if (down)
    warn("%s(%d,%d), link down, TX drops the pkts until the link is up\n", tag, idx,
         s_port);
  else
    notice("%s(%d,%d), link up, TX resumes after %" PRIu64 " ms\n", tag, idx, s_port,
           (now - link->tsc) / NS_PER_MS);
  link->down = down;
  link->tsc = now;
  return true;
}

#endif
