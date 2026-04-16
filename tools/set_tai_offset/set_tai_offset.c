/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

/*
 * set_tai_offset — reads the current leap second count from
 * /usr/share/zoneinfo/leap-seconds.list and sets the kernel
 * TAI offset via adjtimex(ADJ_TAI).
 *
 * Requires CAP_SYS_TIME or root.
 *
 * Usage: sudo ./set_tai_offset [-v] [-0]
 *   -v  verbose output
 *   -0  reset TAI offset to 0
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/timex.h>

#define LEAP_SECONDS_PATH "/usr/share/zoneinfo/leap-seconds.list"

static int get_kernel_tai_offset(void) {
  struct timex tx = {0};
  adjtimex(&tx);
  return tx.tai;
}

static int set_kernel_tai_offset(int offset) {
  struct timex tx = {0};
  tx.modes = ADJ_TAI;
  tx.constant = offset;
  if (adjtimex(&tx) < 0) return -errno;
  return 0;
}

static int parse_tai_offset(const char* path) {
  FILE* f = fopen(path, "r");
  if (!f) {
    fprintf(stderr, "Failed to open %s: %s\n", path, strerror(errno));
    return -1;
  }

  int tai_offset = -1;
  char line[256];
  while (fgets(line, sizeof(line), f)) {
    if (line[0] == '#' || line[0] == '\n') continue;
    int offset = 0;
    if (sscanf(line, "%*s %d", &offset) == 1) tai_offset = offset;
  }
  fclose(f);
  return tai_offset;
}

int main(int argc, char** argv) {
  int verbose = 0;
  int reset = 0;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-v") == 0)
      verbose = 1;
    else if (strcmp(argv[i], "-0") == 0)
      reset = 1;
  }

  int current = get_kernel_tai_offset();

  if (verbose) printf("Current kernel TAI offset: %d\n", current);

  if (reset) {
    if (current == 0) {
      printf("TAI offset already 0, nothing to do.\n");
      return EXIT_SUCCESS;
    }
    int ret = set_kernel_tai_offset(0);
    if (ret < 0) {
      fprintf(stderr, "Failed to reset TAI offset to 0: %s\n", strerror(-ret));
      fprintf(stderr, "Run as root or: sudo setcap 'cap_sys_time+ep' %s\n", argv[0]);
      return EXIT_FAILURE;
    }
    printf("Kernel TAI offset reset: %d -> 0\n", current);
    return EXIT_SUCCESS;
  }

  if (current > 0) {
    printf("TAI offset already set to %d, nothing to do.\n", current);
    return EXIT_SUCCESS;
  }

  int tai_offset = parse_tai_offset(LEAP_SECONDS_PATH);
  if (tai_offset < 0) {
    fprintf(stderr, "Failed to parse TAI offset from %s\n", LEAP_SECONDS_PATH);
    return EXIT_FAILURE;
  }

  if (verbose) printf("Parsed TAI offset from leap-seconds.list: %d\n", tai_offset);

  int ret = set_kernel_tai_offset(tai_offset);
  if (ret < 0) {
    fprintf(stderr, "Failed to set TAI offset to %d: %s\n", tai_offset, strerror(-ret));
    fprintf(stderr, "Run as root or: sudo setcap 'cap_sys_time+ep' %s\n", argv[0]);
    return EXIT_FAILURE;
  }

  int verify = get_kernel_tai_offset();
  if (verify != tai_offset) {
    fprintf(stderr, "Verify failed: expected %d got %d\n", tai_offset, verify);
    return EXIT_FAILURE;
  }

  printf("Kernel TAI offset set: %d -> %d\n", current, tai_offset);
  return EXIT_SUCCESS;
}
