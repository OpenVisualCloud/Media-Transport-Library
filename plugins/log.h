/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

/* Header for log usage */
#include <stdio.h>

#ifndef _ST_PLUGIN_LOG_HEAD_H_
#define _ST_PLUGIN_LOG_HEAD_H_

/* log define */
#ifdef DEBUG
#define dbg(...)                                                               \
  do {                                                                         \
    printf(__VA_ARGS__);                                                       \
  } while (0)
#else
#define dbg(...)                                                               \
  do {                                                                         \
  } while (0)
#endif
#define info(...)                                                              \
  do {                                                                         \
    printf(__VA_ARGS__);                                                       \
  } while (0)
#define warn(...)                                                              \
  do {                                                                         \
    printf(__VA_ARGS__);                                                       \
  } while (0)
#define err(...)                                                               \
  do {                                                                         \
    printf(__VA_ARGS__);                                                       \
  } while (0)

#endif