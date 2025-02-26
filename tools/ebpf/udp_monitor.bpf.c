/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
 * Copyright(c) 2024 Intel Corporation
 */

// clang-format off
#include <stddef.h>
#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/in.h>

#include <bpf/bpf_helpers.h>
#include <bpf/bpf_endian.h>
// clang-format on

#include "udp_monitor.h"

struct {
  __uint(type, BPF_MAP_TYPE_RINGBUF);
  __uint(max_entries, 512 * 1024);
} udp_hdr_rb SEC(".maps");

SEC("socket")
int bpf_socket_handler(struct __sk_buff* skb) {
  struct udp_pkt_entry* e;
  __u8 verlen;
  __u16 proto;
  __u8 ip_proto;
  __u32 nhoff = ETH_HLEN;

  bpf_skb_load_bytes(skb, 12, &proto, 2);
  proto = __bpf_ntohs(proto);
  if (proto != ETH_P_IP) { /* not ipv4 */
    return 0;
  }
  bpf_skb_load_bytes(skb, nhoff + offsetof(struct iphdr, protocol), &ip_proto, 1);
  if (ip_proto != IPPROTO_UDP) { /* not udp */
    return 0;
  }

  e = bpf_ringbuf_reserve(&udp_hdr_rb, sizeof(*e), 0);
  if (!e) return 0;

  /* fill src and dst ip */
  bpf_skb_load_bytes(skb, nhoff + offsetof(struct iphdr, saddr), &(e->tuple.src_ip), 4);
  bpf_skb_load_bytes(skb, nhoff + offsetof(struct iphdr, daddr), &(e->tuple.dst_ip), 4);
  /* fill src and dst port */
  bpf_skb_load_bytes(skb, nhoff + 0, &verlen, 1);
  bpf_skb_load_bytes(skb, nhoff + ((verlen & 0xF) << 2), &(e->tuple.ports), 4);
  e->len = skb->len;

  bpf_ringbuf_submit(e, 0);

  return 0;
}

char LICENSE[] SEC("license") = "Dual BSD/GPL";
