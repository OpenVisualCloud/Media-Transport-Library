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

/* Header for ST App log usage */
#include <stdio.h>

#ifndef _ST_APP_LOG_HEAD_H_
#define _ST_APP_LOG_HEAD_H_

void app_set_log_level(enum st_log_level level);
enum st_log_level app_get_log_level(void);

/* log define */
#ifdef DEBUG
#define dbg(...)                                                        \
  do {                                                                  \
    if (app_get_log_level() <= ST_LOG_LEVEL_DEBUG) printf(__VA_ARGS__); \
  } while (0)
#else
#define dbg(...) \
  do {           \
  } while (0)
#endif
#define info(...)                                                      \
  do {                                                                 \
    if (app_get_log_level() <= ST_LOG_LEVEL_INFO) printf(__VA_ARGS__); \
  } while (0)
#define warn(...)                                                         \
  do {                                                                    \
    if (app_get_log_level() <= ST_LOG_LEVEL_WARNING) printf(__VA_ARGS__); \
  } while (0)
#define err(...)                                                        \
  do {                                                                  \
    if (app_get_log_level() <= ST_LOG_LEVEL_ERROR) printf(__VA_ARGS__); \
  } while (0)
#define critical(...)    \
  do {                   \
    printf(__VA_ARGS__); \
  } while (0)

#endif
