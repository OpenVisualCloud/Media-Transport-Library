/* SPDX-License-Identifier: BSD-3-Clause
 * Shared by the examples (doc/unified-api/examples.md). They only have to compile:
 * application hooks are declared, never defined.
 */
#ifndef EX_COMMON_H
#define EX_COMMON_H

#include <mtl/experimental/mtl.h>
#include <stdio.h>
#include <string.h>

extern volatile int g_running; /* cleared by the application's shutdown path */

/* Prints a failure and returns ret. The reason and the field at fault are printed only
   when the last error is this failure: a helper that detects a failure itself
   (mtl_util.h) returns its code without setting mtl_last_error(). */
static inline int ex_fail(const char* what, int ret) {
  struct mtl_error_info e;
  memset(&e, 0, sizeof(e));
  if (mtl_last_error(&e, sizeof(e)) >= 0 && e.code == ret)
    fprintf(stderr, "%s: %s %s %s %s\n", what, mtl_error_name(ret),
            mtl_reason_name(e.reason), e.field, e.detail);
  else
    fprintf(stderr, "%s: %s\n", what, mtl_error_name(ret));
  return ret;
}

#endif
