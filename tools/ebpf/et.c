/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
 * Copyright (c) 2023 Intel Corporation
 */
#include "et.h"

#include <bpf/libbpf.h>
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

enum et_args_cmd {
  ET_ARG_UNKNOWN = 0,
  ET_ARG_PRINT_LIBBPF = 0x100, /* start from end of ascii */
  ET_ARG_PROG,
  ET_ARG_HELP,
};

enum et_prog_type {
  ET_PROG_UNKNOWN = 0,
  ET_PROG_FENTRY,
  ET_PROG_KPROBE,
  ET_PROG_TRACEPOINT,
  ET_PROG_XDP,
};

static const char* prog_type_str[] = {
    [ET_PROG_FENTRY] = "fentry",
    [ET_PROG_KPROBE] = "kprobe",
    [ET_PROG_TRACEPOINT] = "tracepoint",
    [ET_PROG_XDP] = "xdp",
};

struct et_ctx {
  enum et_prog_type prog_type;
};

static volatile bool stop = false;

static int libbpf_print_fn(enum libbpf_print_level level, const char* format,
                           va_list args) {
  return vfprintf(stderr, format, args);
}

static void et_sig_handler(int signo) {
  printf("%s, signal %d\n", __func__, signo);

  switch (signo) {
    case SIGINT: /* Interrupt from keyboard */
      stop = true;
      break;
  }

  return;
}

static int udp_send_handler(void* ctx, void* data, size_t data_sz) {
  const struct udp_send_event* e = data;

  printf("%s: pid %d, gso_size %u, bytes %u, duration_ns %llu\n", __func__, e->pid,
         e->gso_size, e->udp_send_bytes, e->duration_ns);
  return 0;
}

static inline int et_fentry_loop() {
  struct ring_buffer* rb = NULL;
  struct fentry_bpf* skel;
  int ret = 0;

  skel = fentry_bpf__open_and_load();
  if (!skel) {
    printf("failed to open BPF skeleton\n");
    return -1;
  }

  ret = fentry_bpf__attach(skel);
  if (ret) {
    printf("failed to attach BPF skeleton\n");
    goto cleanup;
  }

  printf("fentry_bpf__attach() succeeded\n");

  rb = ring_buffer__new(bpf_map__fd(skel->maps.rb), udp_send_handler, NULL, NULL);
  if (!rb) {
    ret = -1;
    fprintf(stderr, "failed to create ring buffer\n");
    goto cleanup;
  }

  while (!stop) {
    ret = ring_buffer__poll(rb, 100);
    if (ret == -EINTR) {
      ret = 0;
      break;
    }
    if (ret < 0) {
      printf("error polling perf buffer: %d\n", ret);
      break;
    }
  }

cleanup:
  ring_buffer__free(rb);
  fentry_bpf__destroy(skel);
  return ret;
}

static struct option et_args_options[] = {{"print", no_argument, 0, ET_ARG_PRINT_LIBBPF},
                                          {"prog", required_argument, 0, ET_ARG_PROG},
                                          {"help", no_argument, 0, ET_ARG_HELP},
                                          {0, 0, 0, 0}};

static void et_print_help() {
  printf("\n");
  printf("##### Usage: #####\n\n");
  printf(" Params:\n");
  printf(" --help           : print this help\n");
  printf(" --print          : print libbpf output\n");
  printf(" --prog <type>    : attach to prog <type>\n");
  printf("\n");
}

static int et_parse_args(struct et_ctx* ctx, int argc, char** argv) {
  int cmd = -1, opt_idx = 0;

  while (1) {
    cmd = getopt_long_only(argc, argv, "hv", et_args_options, &opt_idx);
    if (cmd == -1) break;

    switch (cmd) {
      case ET_ARG_PROG:
        if (strcmp(optarg, "fentry") == 0) {
          ctx->prog_type = ET_PROG_FENTRY;
        }
        break;
      case ET_ARG_PRINT_LIBBPF:
        libbpf_set_print(libbpf_print_fn);
        break;
      case ET_ARG_HELP:
      default:
        et_print_help();
        return -1;
    }
  };

  return 0;
}

int main(int argc, char** argv) {
  struct et_ctx ctx = {0};
  et_parse_args(&ctx, argc, argv);
  signal(SIGINT, et_sig_handler);

  printf("prog type is %s\n", prog_type_str[ctx.prog_type]);
  switch (ctx.prog_type) {
    case ET_PROG_FENTRY:
      et_fentry_loop();
      break;

    default:
      break;
  }

  return 0;
}