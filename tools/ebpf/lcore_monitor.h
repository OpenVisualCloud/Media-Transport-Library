/* SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0
 * Copyright(c) 2023 Intel Corporation
 */

#ifndef __LCORE_MONITOR_HEAD_H
#define __LCORE_MONITOR_HEAD_H

struct lcore_tid_cfg {
  uint32_t core_id;
  uint32_t t_pid;
  uint8_t bpf_trace;
};

enum lcore_tid_event_type {
  /* hook on tracepoint/sched/sched_switch */
  LCORE_SCHED_IN,  /* the t_pid was scheduled in */
  LCORE_SCHED_OUT, /* the t_pid was scheduled out */
  /* note: irq hook is only for io interrupt, not for system irq like timer */
  /* hook on tracepoint/irq/irq_handler_entry */
  LCORE_IRQ_ENTRY,
  /* hook on tracepoint/irq/irq_handler_exit */
  LCORE_IRQ_EXIT,
  /* hook on tracepoint/irq_vectors/irq_work_entry */
  LCORE_VECTOR_ENTRY,
  /* hook on tracepoint/irq_vectors/irq_work_exit */
  LCORE_VECTOR_EXIT,
  /* hook on tracepoint:raw_syscalls:sys_enter */
  LCORE_SYS_ENTER,
  /* hook on tracepoint:raw_syscalls:sys_exit */
  LCORE_SYS_EXIT,
};

struct lcore_tid_event {
  enum lcore_tid_event_type type;
  uint64_t ns;
  union {
    int next_pid; /* for LCORE_SCHED_IN/LCORE_SCHED_OUT */
    int irq;      /* for LCORE_IRQ_ENTRY/LCORE_IRQ_EXIT */
    int vector;   /* LCORE_VECTOR_ENTRY/LCORE_VECTOR_EXIT */
    int id;       /* for LCORE_SYS_ENTER/LCORE_SYS_EXIT */
  };
};

#endif