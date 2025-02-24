/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "fmt.h"

#include "log.h"

int user_get_pgroup(enum user_pg_fmt fmt, struct user_pgroup *pg) {
  int i;

  for (i = 0; i < ARRAY_SIZE(user_pgroups); i++) {
    if (fmt == user_pgroups[i].fmt) {
      *pg = user_pgroups[i];
      return 0;
    }
  }

  err("%s, invalid fmt: %d\n", __func__, fmt);
  return -1;
}

void convert_uyvy10b_to_uyvy8b(uint8_t *yuv_8b, uint8_t const *yuv_10b,
                               int pg_count) {
  struct st20_rfc4175_422_8_pg2_le *pg_8 =
      (struct st20_rfc4175_422_8_pg2_le *)yuv_8b;
  struct st20_rfc4175_422_10_pg2_be *pg_10 =
      (struct st20_rfc4175_422_10_pg2_be *)yuv_10b;

  st20_rfc4175_422be10_to_422le8(pg_10, pg_8, pg_count, 2);
}
