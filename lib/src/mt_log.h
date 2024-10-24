/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef _MT_LIB_LOG_HEAD_H_
#define _MT_LIB_LOG_HEAD_H_

#include <rte_log.h>

#include "mt_usdt.h"
#include "mtl_api.h"

/* log define */
#define RTE_LOGTYPE_MTL (RTE_LOGTYPE_USER1)

int mt_set_log_global_level(enum mtl_log_level level);
enum mtl_log_level mt_get_log_global_level(void);

mtl_log_prefix_formatter_t mt_get_log_prefix_formatter(void);
mtl_log_printer_t mt_get_log_printer(void);

mtl_log_printer_t mt_get_usdt_log_printer(void);

#define MT_LOG(l, t, format, ...)                                              \
  do {                                                                         \
    if (MTL_LOG_LEVEL_##l >= mt_get_log_global_level()) {                      \
      char __prefix[64];                                                       \
      mtl_log_prefix_formatter_t formatter = mt_get_log_prefix_formatter();    \
      mtl_log_printer_t printer = mt_get_log_printer();                        \
      formatter(__prefix, sizeof(__prefix));                                   \
      if (printer)                                                             \
        printer(MTL_LOG_LEVEL_##l, "MTL: %s" format, __prefix, ##__VA_ARGS__); \
      else                                                                     \
        RTE_LOG(l, t, "%s" format, __prefix, ##__VA_ARGS__);                   \
    }                                                                          \
    if (MT_USDT_SYS_LOG_MSG_ENABLED()) {                                       \
      mtl_log_printer_t usdt_printer = mt_get_usdt_log_printer();              \
      usdt_printer(MTL_LOG_LEVEL_##l, format, ##__VA_ARGS__);                  \
    }                                                                          \
  } while (0)

/* Debug-level messages */
#ifdef DEBUG
#define dbg(...)                    \
  do {                              \
    MT_LOG(DEBUG, MTL, __VA_ARGS__); \
  } while (0)
#else
#define dbg(...) \
  do {           \
  } while (0)
#endif

/* Informational */
#define info(...)                   \
  do {                              \
    MT_LOG(INFO, MTL, __VA_ARGS__); \
  } while (0)
#define info_once(...)                \
  do {                                \
    static bool once = true;          \
    if (once) {                       \
      MT_LOG(INFO, MTL, __VA_ARGS__); \
      once = false;                   \
    }                                 \
  } while (0)

/* Normal but significant condition. */
#define notice(...)                   \
  do {                                \
    MT_LOG(NOTICE, MTL, __VA_ARGS__); \
  } while (0)
#define notice_once(...)                \
  do {                                  \
    static bool once = true;            \
    if (once) {                         \
      MT_LOG(NOTICE, MTL, __VA_ARGS__); \
      once = false;                     \
    }                                   \
  } while (0)

/* Warning conditions. */
#define warn(...)                               \
  do {                                          \
    MT_LOG(WARNING, MTL, "Warn: " __VA_ARGS__); \
  } while (0)
#define warn_once(...)                            \
  do {                                            \
    static bool once = true;                      \
    if (once) {                                   \
      MT_LOG(WARNING, MTL, "Warn: " __VA_ARGS__); \
      once = false;                               \
    }                                             \
  } while (0)

/* Error conditions. */
#define err(...)                             \
  do {                                       \
    MT_LOG(ERR, MTL, "Error: " __VA_ARGS__); \
  } while (0)
#define err_once(...)                          \
  do {                                         \
    static bool once = true;                   \
    if (once) {                                \
      MT_LOG(ERR, MTL, "Error: " __VA_ARGS__); \
      once = false;                            \
    }                                          \
  } while (0)

/* Critical conditions		*/
#define critical(...)               \
  do {                              \
    MT_LOG(CRIT, MTL, __VA_ARGS__); \
  } while (0)
#define critical_once(...)            \
  do {                                \
    static bool once = true;          \
    if (once) {                       \
      MT_LOG(CRIT, MTL, __VA_ARGS__); \
      once = false;                   \
    }                                 \
  } while (0)

#endif
