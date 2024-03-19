/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Intel Corporation
 */

#ifndef __UDP_MONITOR_HEAD_H
#define __UDP_MONITOR_HEAD_H

struct udp_pkt_entry {
  __be32 src_ip;
  __be32 dst_ip;
};

#endif
