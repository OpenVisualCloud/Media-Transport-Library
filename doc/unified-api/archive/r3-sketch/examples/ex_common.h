/* SPDX-License-Identifier: BSD-3-Clause
 * Shared by the doc examples (doc/unified-api/10-api-sketch.md). The examples only have
 * to compile: application hooks are declared, never defined.
 */
#ifndef EX_COMMON_H
#define EX_COMMON_H

#include <mtl/experimental/mtl_unified.h>
#include <stdio.h>

extern volatile int g_running; /* cleared by the application's shutdown path */

/* Prints a failure with its reason and detail, and returns ret. */
static inline int ex_fail(const char* what, int ret) {
  struct mtl_error_info e;
  mtl_error_info_init(&e);
  (void)mtl_last_error(&e);
  fprintf(stderr, "%s: %s reason=%s %s\n", what, mtl_error_name(ret),
          mtl_reason_name(e.reason), e.detail);
  return ret;
}

#endif
