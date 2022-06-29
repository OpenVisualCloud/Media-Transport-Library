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

#include <rte_log.h>

#ifndef _ST_LIB_LOG_HEAD_H_
#define _ST_LIB_LOG_HEAD_H_

/* log define */
#define RTE_LOGTYPE_ST (RTE_LOGTYPE_USER1)
#ifdef DEBUG
#define dbg(...)                     \
  do {                               \
    RTE_LOG(DEBUG, ST, __VA_ARGS__); \
  } while (0)
#else
#define dbg(...) \
  do {           \
  } while (0)
#endif
#define info(...)                   \
  do {                              \
    RTE_LOG(INFO, ST, __VA_ARGS__); \
  } while (0)
#define info_once(...)                \
  do {                                \
    static bool once = true;          \
    if (once) {                       \
      RTE_LOG(INFO, ST, __VA_ARGS__); \
      once = false;                   \
    }                                 \
  } while (0)
#define warn(...)                      \
  do {                                 \
    RTE_LOG(WARNING, ST, __VA_ARGS__); \
  } while (0)
#define warn_once(...)                   \
  do {                                   \
    static bool once = true;             \
    if (once) {                          \
      RTE_LOG(WARNING, ST, __VA_ARGS__); \
      once = false;                      \
    }                                    \
  } while (0)
#define err(...)                   \
  do {                             \
    RTE_LOG(ERR, ST, __VA_ARGS__); \
  } while (0)
#define err_once(...)                \
  do {                               \
    static bool once = true;         \
    if (once) {                      \
      RTE_LOG(ERR, ST, __VA_ARGS__); \
      once = false;                  \
    }                                \
  } while (0)
#define critical(...)               \
  do {                              \
    RTE_LOG(CRIT, ST, __VA_ARGS__); \
  } while (0)
#define critical_once(...)            \
  do {                                \
    static bool once = true;          \
    if (once) {                       \
      RTE_LOG(CRIT, ST, __VA_ARGS__); \
      once = false;                   \
    }                                 \
  } while (0)

#endif
