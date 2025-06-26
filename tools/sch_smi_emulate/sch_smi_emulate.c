/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include <getopt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <stdbool.h>
#define FAKE_FRAME_SIZE (1920*1080*3/2) // e.g., 1080p YUV420 frame

enum se_args_cmd {
  SE_ARG_UNKNOWN = 0,
  SE_ARG_SLEEP_MS = 0x100, /* start from end of ascii */
  SE_ARG_WORK_US,
  SE_ARG_ENCODE, /* optional argument */
};

#ifndef NS_PER_S
#define NS_PER_S (1000000000)
#endif

struct se_context {
  int sleep_time_ms;
  int work_time_us;
  bool encode; /* optional argument */
};

static struct option se_args_options[] = {
  {"sleep_ms", required_argument, 0, SE_ARG_SLEEP_MS},
  {"work_us", required_argument, 0, SE_ARG_WORK_US},
  {"encode", optional_argument, 0, SE_ARG_ENCODE},
  {0, 0, 0, 0}};

void se_simulate_memory_access(uint8_t *buffer, size_t size) {
  for (size_t i = 0; i < size; i++) {
    buffer[i] = (buffer[i] + 1) % 256;
  }
}

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
      case SE_ARG_ENCODE:
        ctx->encode = true;
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

  printf("sleep_time ms %d work_time %d ms\n", ctx->sleep_time_ms, ctx->work_time_us / 1000);

  while (1) {
    usleep(ctx->sleep_time_ms * 1000);
    start = se_get_monotonic_time();
    end = start + (ctx->work_time_us * 1000);
    while (se_get_monotonic_time() < end) {

      /* Encode scenario */
      if (ctx->encode) {
        uint8_t *buffer = malloc(FAKE_FRAME_SIZE);
        if (!buffer) {
          perror("malloc failed");
          return -1;
        }
        se_simulate_memory_access(buffer, FAKE_FRAME_SIZE);
        free(buffer);
      } else {
        /* a busy worker */
        for (int i = 0; i < 1000 * 10; i++) {
          sum += i * i;
        }
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
