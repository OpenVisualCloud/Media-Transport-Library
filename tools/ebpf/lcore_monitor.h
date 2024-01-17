/* SPDX-License-Identifier: BSD-3-Clause
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
  LCORE_SCHED_IN,
  LCORE_SCHED_OUT,
};

struct lcore_tid_event {
  enum lcore_tid_event_type type;
  uint64_t ns;
  int next_pid;
};

#endif