/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

/* Header for ST App log usage */
#include <stdio.h>

#ifndef _ST_APP_LOG_HEAD_H_
#define _ST_APP_LOG_HEAD_H_

void app_set_log_level(enum mtl_log_level level);
enum mtl_log_level app_get_log_level(void);

/* log define */
#ifdef DEBUG
#define dbg(...)                                                         \
  do {                                                                   \
    if (app_get_log_level() <= MTL_LOG_LEVEL_DEBUG) printf(__VA_ARGS__); \
  } while (0)
#else
#define dbg(...) \
  do {           \
  } while (0)
#endif
#define info(...)                                                       \
  do {                                                                  \
    if (app_get_log_level() <= MTL_LOG_LEVEL_INFO) printf(__VA_ARGS__); \
  } while (0)
#define notce(...)                                                        \
  do {                                                                    \
    if (app_get_log_level() <= MTL_LOG_LEVEL_NOTICE) printf(__VA_ARGS__); \
  } while (0)
#define warn(...)                                                          \
  do {                                                                     \
    if (app_get_log_level() <= MTL_LOG_LEVEL_WARNING) printf(__VA_ARGS__); \
  } while (0)
#define err(...)                                                         \
  do {                                                                   \
    if (app_get_log_level() <= MTL_LOG_LEVEL_ERROR) printf(__VA_ARGS__); \
  } while (0)
#define critical(...)    \
  do {                   \
    printf(__VA_ARGS__); \
  } while (0)

#endif
