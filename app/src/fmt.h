/*
 * Copyright (C) 2022 Intel Corporation.
 *
 * This software and the related documents are Intel copyrighted materials,
 * and your use of them is governed by the express license under which they
 * were provided to you ("License").
 * Unless the License provides otherwise, you may not use, modify, copy,
 * publish, distribute, disclose or transmit this software or the related
 * documents without Intel's prior written permission.
 *
 * This software and the related documents are provided as is, with no
 * express or implied warranties, other than those that are expressly stated
 * in the License.
 *
 */

#include <st_convert_api.h>

#include "app_platform.h"

#ifndef _FMT_HEAD_H_
#define _FMT_HEAD_H_

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof(arr[0]))

enum user_pg_fmt {
  USER_FMT_YUV_422_8BIT,
  USER_FMT_MAX,
};

struct user_pgroup {
  /** video format of current pixel group */
  enum user_pg_fmt fmt;
  /** pixel group size(octets), e.g. 5 for YUV422 10 bit */
  unsigned int size;
  /** pixel group coverage(pixels), e.g. 2 for YUV422 10 bit */
  unsigned int coverage;
};

static const struct user_pgroup user_pgroups[] = {
    {
        /* USER_FMT_YUV_422_8BIT */
        .fmt = USER_FMT_YUV_422_8BIT,
        .size = 4,
        .coverage = 2,
    },
};

int user_get_pgroup(enum user_pg_fmt fmt, struct user_pgroup* pg);

void convert_uyvy10b_to_uyvy8b(uint8_t* yuv_8b, uint8_t const* yuv_10b, int pg_count);

#endif