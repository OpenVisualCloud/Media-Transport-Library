/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
 * Copyright (c) 2023 Intel Corporation
 */
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
  ET_ARG_FENTRY,
  ET_ARG_HELP,
};

struct et_ctx {
  bool not_yet;
};

static struct et_ctx g_ctx = {0};
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

static inline int et_fentry_loop() {
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

  while (!stop) {
    fprintf(stderr, ".");
    sleep(1);
  }

cleanup:
  fentry_bpf__destroy(skel);
  return ret;
}

static struct option et_args_options[] = {{"print", no_argument, 0, ET_ARG_PRINT_LIBBPF},
                                          {"fentry", no_argument, 0, ET_ARG_FENTRY},
                                          {"help", no_argument, 0, ET_ARG_HELP},
                                          {0, 0, 0, 0}};

static void et_print_help() {
  printf("\n");
  printf("##### Usage: #####\n\n");
  printf(" Params:\n");
  printf(" --help        : print this help\n");
  printf(" --print       : print libbpf output\n");
  printf(" --fentry      : attach to fentry\n");
  printf("\n");
}

static int et_parse_args(struct et_ctx* ctx, int argc, char** argv) {
  int cmd = -1, opt_idx = 0;

  while (1) {
    cmd = getopt_long_only(argc, argv, "hv", et_args_options, &opt_idx);
    if (cmd == -1) break;

    switch (cmd) {
      case ET_ARG_FENTRY:
        et_fentry_loop();
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
  et_parse_args(&g_ctx, argc, argv);
  signal(SIGINT, et_sig_handler);

  return 0;
}