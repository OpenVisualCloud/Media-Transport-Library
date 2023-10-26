/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
 * Copyright (c) 2023 Intel Corporation
 */
//clang-format off
#include "vmlinux.h"
//clang-format off
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

#include "et.h"

char LICENSE[] SEC("license") = "Dual BSD/GPL";

struct {
  __uint(type, BPF_MAP_TYPE_HASH);
  __uint(max_entries, 8192);
  __type(key, u64);
  __type(value, u64);
} start_time SEC(".maps");

struct {
  __uint(type, BPF_MAP_TYPE_RINGBUF);
  __uint(max_entries, 256 * 1024);
} rb SEC(".maps");

SEC("fentry/udp_send_skb")
int BPF_PROG(udp_send_skb, struct sk_buff* skb, struct flowi4* fl4,
             struct inet_cork* cork) {
  u64 ts;
  u64 skb_addr = (u64)skb;

  ts = bpf_ktime_get_ns();
  bpf_map_update_elem(&start_time, &skb_addr, &ts, BPF_ANY);

  return 0;
}

SEC("fexit/udp_send_skb")
int BPF_PROG(udp_send_skb_exit, struct sk_buff* skb, struct flowi4* fl4,
             struct inet_cork* cork, long ret) {
  struct udp_send_event* e;
  pid_t pid;
  u64 *start_ts, duration_ns = 0;
  u64 skb_addr = (u64)skb;

  pid = bpf_get_current_pid_tgid() >> 32;
  start_ts = bpf_map_lookup_elem(&start_time, &skb_addr);
  if (start_ts) duration_ns = bpf_ktime_get_ns() - *start_ts;
  bpf_map_delete_elem(&start_time, &skb_addr);

  e = bpf_ringbuf_reserve(&rb, sizeof(*e), 0);
  if (!e) return 0;

  e->pid = pid;
  e->gso_size = cork->gso_size;
  e->duration_ns = duration_ns;
  e->udp_send_bytes = skb->len;
  e->ret = ret;

  bpf_ringbuf_submit(e, 0);

  return 0;
}