/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#ifndef TESTS_UNIT_FFMPEG_MTL_COMMON_HARNESS_H
#define TESTS_UNIT_FFMPEG_MTL_COMMON_HARNESS_H

#include <mtl/mtl_api.h>

#ifdef __cplusplus
extern "C" {
#endif

struct StDevArgs;

void ut_ffmpeg_reset(void);
mtl_handle ut_ffmpeg_get(const struct StDevArgs* args, int* idx);
int ut_ffmpeg_put(mtl_handle handle);
const struct mtl_init_params* ut_ffmpeg_last_init_params(void);
int ut_ffmpeg_init_calls(void);
int ut_ffmpeg_uninit_calls(void);

#ifdef __cplusplus
}
#endif

#endif