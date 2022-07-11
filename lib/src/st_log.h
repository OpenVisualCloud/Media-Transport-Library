/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
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
