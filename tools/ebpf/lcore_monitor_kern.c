/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

//clang-format off
#include "vmlinux.h"
//clang-format off

#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

#include "lcore_monitor.h"

struct {
  __uint(type, BPF_MAP_TYPE_ARRAY);
  __uint(max_entries, 1);
  __type(key, u32);
  __type(value, struct lcore_tid_cfg);
} lm_cfg_map SEC(".maps");

struct {
  __uint(type, BPF_MAP_TYPE_RINGBUF);
  __uint(max_entries, 512 * 1024);
} lm_events_map SEC(".maps");

static int lm_event_submit(enum lcore_tid_event_type type,
                           struct trace_event_raw_sched_switch* args) {
  struct lcore_tid_event* e;

  e = bpf_ringbuf_reserve(&lm_events_map, sizeof(*e), 0);
  if (!e) {
    char fmt[] = "lm event ringbuf reserve fail\n";
    bpf_trace_printk(fmt, sizeof(fmt));
    return 0;
  }

  e->type = type;
  e->ns = bpf_ktime_get_ns();
  e->next_pid = args->next_pid;

  bpf_ringbuf_submit(e, 0);
  return 0;
}

SEC("tracepoint/sched/sched_switch")
int bpf_prog_sched_switch(struct trace_event_raw_sched_switch* args) {
  uint32_t key = 0;
  struct lcore_tid_cfg* cfg = bpf_map_lookup_elem(&lm_cfg_map, &key);
  if (!cfg) return 0;

  /* core id check */
  if (bpf_get_smp_processor_id() != cfg->core_id) return 0;

  if (cfg->bpf_trace) {
    char fmt[] = "prev_pid %d next_pid in\n";
    bpf_trace_printk(fmt, sizeof(fmt), args->prev_pid, args->next_pid);
  }

  if (args->prev_pid == cfg->t_pid) {
    lm_event_submit(LCORE_SCHED_OUT, args);
  }

  if (args->next_pid == cfg->t_pid) {
    lm_event_submit(LCORE_SCHED_IN, args);
  }

  return 0;
}

char LICENSE[] SEC("license") = "Dual BSD/GPL";
