/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
 * Copyright(c) 2024 Intel Corporation
 */

//clang-format off
#include <linux/bpf.h>
//clang-format off

#include <bpf/bpf_helpers.h>
#include <xdp/parsing_helpers.h>
#include <xdp/xdp_helpers.h>

char LICENSE[] SEC("license") = "Dual BSD/GPL";

struct {
  __uint(type, BPF_MAP_TYPE_HASH);
  __uint(max_entries, 256); /* max 256 filters */
  __type(key, __u16);       /* udp port: 16bit */
  __type(value, __u8);      /* only 1 or 0 */
} udp4_dp_filter SEC(".maps");

struct {
  __uint(priority, 19);
  __uint(XDP_PASS, 0);
  __uint(XDP_DROP, 1);
} XDP_RUN_CONFIG(mtl_dp_filter);

static int __always_inline lookup_udp4_dp(__u16 dp) {
  __u8 *value;

  value = bpf_map_lookup_elem(&udp4_dp_filter, &dp);
  if (value && *value != 0)
    return 1;
  return 0;
}

SEC("xdp")
int mtl_dp_filter(struct xdp_md *ctx) {
  void *data_end = (void *)(long)ctx->data_end;
  void *data = (void *)(long)ctx->data;

  struct hdr_cursor nh;
  struct ethhdr *eth;
  int eth_type;

  nh.pos = data;
  eth_type = parse_ethhdr(&nh, data_end, &eth);
  if (eth_type != bpf_htons(ETH_P_IP))
    return XDP_PASS;

  struct iphdr *iphdr;
  int ip_type;
  ip_type = parse_iphdr(&nh, data_end, &iphdr);
  if (ip_type != IPPROTO_UDP)
    return XDP_PASS;

  struct udphdr *udphdr;
  int ret;
  ret = parse_udphdr(&nh, data_end, &udphdr);
  if (ret < 0)
    return XDP_PASS;

  __u16 dst_port = bpf_ntohs(udphdr->dest);
  if (lookup_udp4_dp(dst_port) == 0)
    return XDP_PASS;

  /* go to next program: xsk_def_prog */
  return XDP_DROP;
}
