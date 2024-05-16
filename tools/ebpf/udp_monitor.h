/* SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0
 * Copyright(c) 2024 Intel Corporation
 */

#ifndef __UDP_MONITOR_HEAD_H
#define __UDP_MONITOR_HEAD_H

struct udp_pkt_tuple {
  __be32 src_ip;
  __be32 dst_ip;
  union {
    __be32 ports;
    struct {
      __be16 src_port;
      __be16 dst_port;
    };
  };
};

struct udp_pkt_entry {
  struct udp_pkt_tuple tuple;
  __u32 len;
};

#endif
