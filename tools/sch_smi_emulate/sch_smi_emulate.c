/*
 * Copyright (C) 2021 Intel Corporation.
 *
 * This software and the related documents are Intel copyrighted materials,
 * and your use of them is governed by the express license under which they
 * were provided to you ("License").
 * Unless the License provides otherwise, you may not use, modify, copy,
 * publish, distribute, disclose or transmit this software or the related
 * documents without Intel's prior written permission.
 *
 * This software and the related documents are provided as is, with no
 * express or implied warranties, other than those that are expressly stated
 * in the License.
 *
 */

#include <getopt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum se_args_cmd {
  SE_ARG_UNKNOWN = 0,
  SE_ARG_SLEEP_MS = 0x100, /* start from end of ascii */
  SE_ARG_WORK_US,
};

#ifndef NS_PER_S
#define NS_PER_S (1000000000)
#endif

struct se_context {
  int sleep_time_ms;
  int work_time_us;
};

static struct option se_args_options[] = {
    {"sleep_ms", required_argument, 0, SE_ARG_SLEEP_MS},
    {"work_us", required_argument, 0, SE_ARG_WORK_US},

    {0, 0, 0, 0}};

static int se_parse_args(struct se_context* ctx, int argc, char** argv) {
  int cmd = -1, opt_idx = 0;

  while (1) {
    cmd = getopt_long_only(argc, argv, "hv", se_args_options, &opt_idx);
    if (cmd == -1) break;

    switch (cmd) {
      case SE_ARG_SLEEP_MS:
        ctx->sleep_time_ms = atoi(optarg);
        break;
      case SE_ARG_WORK_US:
        ctx->work_time_us = atoi(optarg);
        break;
      default:
        break;
    }
  };

  return 0;
}

static int se_init(struct se_context* ctx) {
  ctx->sleep_time_ms = 100;
  ctx->work_time_us = 100;
  return 0;
}

static inline uint64_t se_get_monotonic_time() {
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
  return ((uint64_t)ts.tv_sec * NS_PER_S) + ts.tv_nsec;
}

static int se_loop(struct se_context* ctx) {
  uint64_t start, end;
  volatile int sum;

  printf("sleep_time_ms %d work_time_us %d\n", ctx->sleep_time_ms, ctx->work_time_us);

  while (1) {
    usleep(ctx->sleep_time_ms * 1000);
    start = se_get_monotonic_time();
    end = start + (ctx->work_time_us * 1000);
    while (se_get_monotonic_time() < end) {
      /* a busy worker */
      for (int i = 0; i < 1000 * 10; i++) {
        sum += i * i;
      }
    }
  }
  return 0;
}

int main(int argc, char** argv) {
  struct se_context ctx;

  se_init(&ctx);

  se_parse_args(&ctx, argc, argv);

  se_loop(&ctx);

  return 0;
}
