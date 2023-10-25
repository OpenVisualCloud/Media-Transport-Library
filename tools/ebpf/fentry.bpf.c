/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
 * Copyright (c) 2023 Intel Corporation
 */
//clang-format off
#include "vmlinux.h"
//clang-format off
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

char LICENSE[] SEC("license") = "Dual BSD/GPL";

SEC("fentry/udp_send_skb")
int BPF_PROG(udp_send_skb, struct sk_buff* skb, struct flowi4* fl4,
             struct inet_cork* cork) {
  pid_t pid;

  pid = bpf_get_current_pid_tgid() >> 32;
  bpf_printk("fentry: pid = %d, gso size = %u\n", pid, cork->gso_size);
  return 0;
}

SEC("fexit/udp_send_skb")
int BPF_PROG(udp_send_skb_exit, struct sk_buff* skb, struct flowi4* fl4,
             struct inet_cork* cork, long ret) {
  pid_t pid;

  pid = bpf_get_current_pid_tgid() >> 32;
  bpf_printk("fexit: pid = %d, gso size = %u, ret = %ld\n", pid, cork->gso_size, ret);
  return 0;
}