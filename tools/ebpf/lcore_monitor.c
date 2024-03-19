/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

// clang-format off
#include <stdint.h>
#include "lcore_monitor.h"
// clang-format on

#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "lcore_monitor.skel.h"
#include "log.h"

struct lcore_monitor_ctx {
  struct lcore_tid_cfg cfg;
  struct lcore_tid_event sched_out;
  struct lcore_tid_event irq_entry;
  struct lcore_tid_event vector_entry;
};

enum lm_args_cmd {
  LM_ARG_UNKNOWN = 0,
  LM_ARG_CORE = 0x100, /* start from end of ascii */
  LM_ARG_T_PID,
  LM_ARG_BPF_TRACE,
  LM_ARG_HELP,
};

static struct option et_args_options[] = {{"lcore", required_argument, 0, LM_ARG_CORE},
                                          {"t_pid", required_argument, 0, LM_ARG_T_PID},
                                          {"bpf_trace", no_argument, 0, LM_ARG_BPF_TRACE},
                                          {"help", no_argument, 0, LM_ARG_HELP},
                                          {0, 0, 0, 0}};

static void lm_print_help() {
  printf("\n");
  printf("##### Usage: #####\n\n");

  printf(" Params:\n");
  printf("  --lcore <id>        Set the monitor lcore\n");
  printf("  --t_pid <id>        Set the monitor t_pid\n");
  printf("  --bpf_trace         Enable bpf trace\n");
  printf("  --help              Print help info\n");

  printf("\n");
}

static int lm_parse_args(struct lcore_monitor_ctx* ctx, int argc, char** argv) {
  int cmd = -1, opt_idx = 0;

  while (1) {
    cmd = getopt_long_only(argc, argv, "hv", et_args_options, &opt_idx);
    if (cmd == -1) break;

    switch (cmd) {
      case LM_ARG_CORE:
        ctx->cfg.core_id = atoi(optarg);
        break;
      case LM_ARG_T_PID:
        ctx->cfg.t_pid = atoi(optarg);
        break;
      case LM_ARG_BPF_TRACE:
        ctx->cfg.bpf_trace = true;
        break;
      case LM_ARG_HELP:
      default:
        lm_print_help();
        return -1;
    }
  }

  return 0;
}

static bool stop = false;

static void lm_sig_handler(int signo) {
  info("%s, signal %d\n", __func__, signo);

  switch (signo) {
    case SIGINT: /* Interrupt from keyboard */
      stop = true;
      break;
  }

  return;
}

static int get_process_name_by_pid(pid_t pid, char* process_name, size_t max_len) {
  char path[128];
  FILE* fp;

  snprintf(path, sizeof(path), "/proc/%d/comm", pid);
  fp = fopen(path, "r");

  if (!fp) {
    err("%s, Failed to open /proc/%d/comm\n", __func__, pid);
    return -EIO;
  }

  if (fgets(process_name, max_len, fp) == NULL) {
    err("%s, Failed to read process name for pid %d\n", __func__, pid);
    process_name[0] = '\0';
  }
  fclose(fp);

  size_t len = strlen(process_name);
  if (len > 0 && process_name[len - 1] == '\n') {
    process_name[len - 1] = '\0';
  }

  return 0;
}

static int lm_event_handler(void* pri, void* data, size_t data_sz) {
  struct lcore_monitor_ctx* ctx = pri;
  const struct lcore_tid_event* e = data;
  int ret;

  dbg("%s: type %d, ns %" PRIu64 "\n", __func__, e->type, e->ns);
  if (e->type == LCORE_SCHED_OUT) {
    memcpy(&ctx->sched_out, e, sizeof(ctx->sched_out));
    dbg("%s: out ns %" PRIu64 "\n", __func__, ctx->sched_out.ns);
    return 0;
  }

  if (e->type == LCORE_SCHED_IN) {
    float ns = e->ns - ctx->sched_out.ns;
    int next_pid = ctx->sched_out.next_pid;
    char process_name[64];
    ret = get_process_name_by_pid(next_pid, process_name, sizeof(process_name));
    if (ret < 0)
      info("%s: sched out %.3fus as pid: %d\n", __func__, ns / 1000, next_pid);
    else
      info("%s: sched out %.3fus as comm: %s\n", __func__, ns / 1000, process_name);
  }

  if (e->type == LCORE_IRQ_ENTRY) {
    memcpy(&ctx->irq_entry, e, sizeof(ctx->irq_entry));
    dbg("%s: irq_entry %d ns %" PRIu64 "\n", __func__, e->irq, e->ns);
    return 0;
  }

  if (e->type == LCORE_IRQ_EXIT) {
    float ns = e->ns - ctx->irq_entry.ns;
    info("%s: sched out %.3fus as irq: %d\n", __func__, ns / 1000, e->irq);
  }

  if (e->type == LCORE_VECTOR_ENTRY) {
    memcpy(&ctx->vector_entry, e, sizeof(ctx->vector_entry));
    dbg("%s: vector_entry %d ns %" PRIu64 "\n", __func__, e->vector, e->ns);
    return 0;
  }

  if (e->type == LCORE_VECTOR_EXIT) {
    float ns = e->ns - ctx->vector_entry.ns;
    info("%s: sched out %.3fus as vector: %d\n", __func__, ns / 1000, e->vector);
  }

  return 0;
}

int main(int argc, char** argv) {
  struct lcore_monitor_ctx ctx;
  int ret;

  memset(&ctx, 0, sizeof(ctx));
  ret = lm_parse_args(&ctx, argc, argv);
  if (ret < 0) return ret;
  if (!ctx.cfg.core_id) {
    err("%s, no core id define\n", __func__);
    lm_print_help();
    return -1;
  }
  if (!ctx.cfg.t_pid) {
    err("%s, no t_pid define\n", __func__);
    lm_print_help();
    return -1;
  }

  struct lcore_monitor_bpf* skel = NULL;
  struct ring_buffer* rb = NULL;

  skel = lcore_monitor_bpf__open_and_load();
  if (!skel) {
    err("%s, failed to open and load skeleton\n", __func__);
    goto exit;
  }
  uint32_t key = 0;
  ret = bpf_map__update_elem(skel->maps.lm_cfg_map, &key, sizeof(key), &ctx.cfg,
                             sizeof(ctx.cfg), BPF_ANY);
  if (ret < 0) {
    err("%s, update lm_cfg_map fail\n", __func__);
    goto exit;
  }
  int lm_events_fd = bpf_map__fd(skel->maps.lm_events_map);
  rb = ring_buffer__new(lm_events_fd, lm_event_handler, &ctx, NULL);
  if (!rb) {
    err("%s, create ring buffer fail\n", __func__);
    goto exit;
  }
  ret = lcore_monitor_bpf__attach(skel);
  if (ret) {
    err("%s, failed to attach skeleton\n", __func__);
    goto exit;
  }
  info("%s, attach skeleton succ\n", __func__);

  signal(SIGINT, lm_sig_handler);

  while (!stop) {
    ret = ring_buffer__poll(rb, 100);
    if (ret == -EINTR) {
      ret = 0;
      break;
    }
    if (ret < 0) {
      err("%s, polling fail\n", __func__);
      break;
    }
  }

  info("%s, stop now\n", __func__);
exit:
  if (rb) ring_buffer__free(rb);
  if (skel) lcore_monitor_bpf__destroy(skel);
  return 0;
}
