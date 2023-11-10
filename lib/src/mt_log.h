/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include <rte_log.h>

#ifndef _MT_LIB_LOG_HEAD_H_
#define _MT_LIB_LOG_HEAD_H_

extern void (*g_mt_log_prefix_format)(char* buf, size_t sz);

/* log define */
#define RTE_LOGTYPE_MT (RTE_LOGTYPE_USER1)

#define MT_LOG(l, t, format, ...)                        \
  do {                                                   \
    char __prefix[64];                                   \
    g_mt_log_prefix_format(__prefix, sizeof(__prefix));  \
    RTE_LOG(l, t, "%s" format, __prefix, ##__VA_ARGS__); \
  } while (0)

/* Debug-level messages */
#ifdef DEBUG
#define dbg(...)                    \
  do {                              \
    MT_LOG(DEBUG, MT, __VA_ARGS__); \
  } while (0)
#else
#define dbg(...) \
  do {           \
  } while (0)
#endif

/* Informational */
#define info(...)                  \
  do {                             \
    MT_LOG(INFO, MT, __VA_ARGS__); \
  } while (0)
#define info_once(...)               \
  do {                               \
    static bool once = true;         \
    if (once) {                      \
      MT_LOG(INFO, MT, __VA_ARGS__); \
      once = false;                  \
    }                                \
  } while (0)

/* Normal but significant condition. */
#define notice(...)                  \
  do {                               \
    MT_LOG(NOTICE, MT, __VA_ARGS__); \
  } while (0)
#define notice_once(...)               \
  do {                                 \
    static bool once = true;           \
    if (once) {                        \
      MT_LOG(NOTICE, MT, __VA_ARGS__); \
      once = false;                    \
    }                                  \
  } while (0)

/* Warning conditions. */
#define warn(...)                              \
  do {                                         \
    MT_LOG(WARNING, MT, "Warn: " __VA_ARGS__); \
  } while (0)
#define warn_once(...)                           \
  do {                                           \
    static bool once = true;                     \
    if (once) {                                  \
      MT_LOG(WARNING, MT, "Warn: " __VA_ARGS__); \
      once = false;                              \
    }                                            \
  } while (0)

/* Error conditions. */
#define err(...)                            \
  do {                                      \
    MT_LOG(ERR, MT, "Error: " __VA_ARGS__); \
  } while (0)
#define err_once(...)                         \
  do {                                        \
    static bool once = true;                  \
    if (once) {                               \
      MT_LOG(ERR, MT, "Error: " __VA_ARGS__); \
      once = false;                           \
    }                                         \
  } while (0)

/* Critical conditions		*/
#define critical(...)              \
  do {                             \
    MT_LOG(CRIT, MT, __VA_ARGS__); \
  } while (0)
#define critical_once(...)           \
  do {                               \
    static bool once = true;         \
    if (once) {                      \
      MT_LOG(CRIT, MT, __VA_ARGS__); \
      once = false;                  \
    }                                \
  } while (0)

#endif
