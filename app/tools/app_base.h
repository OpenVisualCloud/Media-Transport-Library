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
#ifndef WINDOWSENV
#include <arpa/inet.h>
#else
#include <winsock2.h>
#endif
#include <errno.h>
#include <signal.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef _CONV_APP_BASE_HEAD_H_
#define _CONV_APP_BASE_HEAD_H_

#define MAX_FMT_LEN 64
#define MAX_FILE_NAME_LEN 128
struct conv_app_context {
  int w;
  int h;
  char pix_fmt_in[MAX_FMT_LEN];
  char pix_fmt_out[MAX_FMT_LEN];
  char file_in[MAX_FILE_NAME_LEN];
  char file_out[MAX_FILE_NAME_LEN];
};

static inline void* zmalloc(size_t sz) {
  void* p = malloc(sz);
  if (p) memset(p, 0x0, sz);
  return p;
}

static inline void conv_app_free(void* p) { free(p); }

#endif
