/* SPDX-License-Identifier: BSD-3-Clause
 * Shared by the examples (doc/unified-api/examples.md). They only have to compile:
 * application hooks are declared, never defined.
 */
#ifndef EX_COMMON_H
#define EX_COMMON_H

#include <mtl/experimental/mtl.h>
#include <stdio.h>

extern volatile int g_running; /* cleared by the application's shutdown path */

/* Prints a failure with its reason and the field at fault, and returns ret. */
static inline int ex_fail(const char* what, int ret) {
  struct mtl_error_info e;
  mtl_last_error(&e, sizeof(e));
  fprintf(stderr, "%s: %s %s %s %s\n", what, mtl_error_name(ret),
          mtl_reason_name(e.reason), e.field, e.detail);
  return ret;
}

#endif
