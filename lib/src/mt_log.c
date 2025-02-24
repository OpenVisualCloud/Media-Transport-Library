/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#include "mt_log.h"

#include "mt_main.h"

static void log_default_prefix(char *buf, size_t sz) {
  time_t now;
  struct tm tm;

  time(&now);
  localtime_r(&now, &tm);
  strftime(buf, sz, "%Y-%m-%d %H:%M:%S, ", &tm);
}

/* default log prefix format */
static mtl_log_prefix_formatter_t g_mt_log_prefix_format = log_default_prefix;

int mtl_set_log_prefix_formatter(mtl_log_prefix_formatter_t f) {
  if (f) {
    info("%s, new formatter %p\n", __func__, f);
    g_mt_log_prefix_format = f;
  } else {
    info("%s, switch to default as user prefix is null\n", __func__);
    g_mt_log_prefix_format = log_default_prefix;
  }
  return 0;
}

mtl_log_prefix_formatter_t mt_get_log_prefix_formatter(void) {
  return g_mt_log_prefix_format;
}

static mtl_log_printer_t g_mt_log_printer;

int mtl_set_log_printer(mtl_log_printer_t f) {
  info("%s, new printer %p\n", __func__, f);
  g_mt_log_printer = f;
  return 0;
}

mtl_log_printer_t mt_get_log_printer(void) { return g_mt_log_printer; }

static void log_usdt_printer(enum mtl_log_level level, const char *format,
                             ...) {
  char msg[256];
  va_list args;
  MTL_MAY_UNUSED(level);

  va_start(args, format);
  vsnprintf(msg, sizeof(msg), format, args);
  va_end(args);

  MT_USDT_SYS_LOG_MSG(level, msg);
}

mtl_log_printer_t mt_get_usdt_log_printer(void) { return log_usdt_printer; }

static enum mtl_log_level g_mt_log_level = MTL_LOG_LEVEL_INFO;

int mt_set_log_global_level(enum mtl_log_level level) {
  g_mt_log_level = level;
  return 0;
}

enum mtl_log_level mt_get_log_global_level(void) { return g_mt_log_level; }

int mtl_set_log_level(mtl_handle mt, enum mtl_log_level level) {
  struct mtl_main_impl *impl = mt;
  uint32_t rte_level;

  if (impl->type != MT_HANDLE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return -EIO;
  }

  dbg("%s, set log level %d\n", __func__, level);
  if (level == mtl_get_log_level(mt))
    return 0;

  switch (level) {
  case MTL_LOG_LEVEL_DEBUG:
    rte_level = RTE_LOG_DEBUG;
    break;
  case MTL_LOG_LEVEL_INFO:
    rte_level = RTE_LOG_INFO;
    break;
  case MTL_LOG_LEVEL_NOTICE:
    rte_level = RTE_LOG_NOTICE;
    break;
  case MTL_LOG_LEVEL_WARNING:
    rte_level = RTE_LOG_WARNING;
    break;
  case MTL_LOG_LEVEL_ERR:
    rte_level = RTE_LOG_ERR;
    break;
  case MTL_LOG_LEVEL_CRIT:
    rte_level = RTE_LOG_CRIT;
    break;
  default:
    err("%s, invalid level %d\n", __func__, level);
    return -EINVAL;
  }

  rte_log_set_global_level(rte_level);

  info("%s, set log level %d succ\n", __func__, level);
  mt_get_user_params(impl)->log_level = level;
  mt_set_log_global_level(level);
  return 0;
}

enum mtl_log_level mtl_get_log_level(mtl_handle mt) {
  struct mtl_main_impl *impl = mt;

  if (impl->type != MT_HANDLE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return -EIO;
  }

  return mt_get_user_params(impl)->log_level;
}

int mtl_openlog_stream(FILE *f) { return rte_openlog_stream(f); }
