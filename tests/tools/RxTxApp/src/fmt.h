/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include <mtl/st_convert_api.h>

#include "app_platform.h"

#ifndef _FMT_HEAD_H_
#define _FMT_HEAD_H_

#define ST_APP_PAYLOAD_TYPE_VIDEO (112)
#define ST_APP_PAYLOAD_TYPE_AUDIO (111)
#define ST_APP_PAYLOAD_TYPE_ANCILLARY (113)
#define ST_APP_PAYLOAD_TYPE_ST22 (114)
#define ST_APP_PAYLOAD_TYPE_FASTMETADATA (115)

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