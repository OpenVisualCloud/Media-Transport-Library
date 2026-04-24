/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Test-only shim for lib/src/mt_log.h.
 *
 * The production header pulls in DPDK <rte_log.h> + USDT machinery and
 * declares external printer functions. For unit tests of pure-math code
 * paths inside lib/src/st2110 (.c files) we don't want logging at all.
 *
 * Activated by injecting -include of this file into the wrapper TU AND
 * defining _MT_LIB_LOG_HEAD_H_ on the command line so that any later
 * "#include "../mt_log.h"" in the production .c becomes a no-op.
 */

#ifndef _MT_LIB_LOG_HEAD_H_
#define _MT_LIB_LOG_HEAD_H_

/* USDT macros are needed by st_*_session.c bodies even when log macros
 * are stubbed out. mt_usdt.h provides empty do{}while(0) bodies for
 * every MT_USDT_* macro when MTL_HAS_USDT is undefined. The build may
 * have set MTL_HAS_USDT globally; force it off for unit tests so we
 * don't need the dtrace-generated provider header on the include path. */
#undef MTL_HAS_USDT
#include "mt_usdt.h"

/* No DPDK / printer plumbing — just absorb every log macro. */

#define dbg(...) \
  do {           \
  } while (0)
#define info(...) \
  do {            \
  } while (0)
#define info_once(...) \
  do {                 \
  } while (0)
#define notice(...) \
  do {              \
  } while (0)
#define notice_once(...) \
  do {                   \
  } while (0)
#define warn(...) \
  do {            \
  } while (0)
#define warn_once(...) \
  do {                 \
  } while (0)
#define err(...) \
  do {           \
  } while (0)
#define err_once(...) \
  do {                \
  } while (0)
#define critical(...) \
  do {                \
  } while (0)
#define critical_once(...) \
  do {                     \
  } while (0)

#endif /* _MT_LIB_LOG_HEAD_H_ */
