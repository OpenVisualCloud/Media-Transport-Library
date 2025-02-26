/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef _ST_LIB_AVX2_H_
#define _ST_LIB_AVX2_H_

#include "st_main.h"

int st20_rfc4175_422be10_to_422le10_avx2(struct st20_rfc4175_422_10_pg2_be *pg_be,
                                         struct st20_rfc4175_422_10_pg2_le *pg_le,
                                         uint32_t w, uint32_t h);

int st20_rfc4175_422le10_to_422be10_avx2(struct st20_rfc4175_422_10_pg2_le *pg_le,
                                         struct st20_rfc4175_422_10_pg2_be *pg_be,
                                         uint32_t w, uint32_t h);

#endif