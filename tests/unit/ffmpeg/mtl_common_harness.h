/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#ifndef TESTS_UNIT_FFMPEG_MTL_COMMON_HARNESS_H
#define TESTS_UNIT_FFMPEG_MTL_COMMON_HARNESS_H

#include <errno.h>
#include <mtl/mtl_api.h>
#include <stdint.h>

typedef struct AVFormatContext AVFormatContext;
typedef struct AVRational {
  int num;
  int den;
} AVRational;

typedef union AVOptionDefaultVal {
  int64_t i64;
  const char* str;
} AVOptionDefaultVal;

typedef struct AVOption {
  const char* name;
  const char* help;
  int offset;
  int type;
  AVOptionDefaultVal default_val;
  double min;
  double max;
  int flags;
} AVOption;

#define AV_LOG_DEBUG 48
#define AV_LOG_INFO 32
#define AV_LOG_WARNING 24
#define AV_LOG_ERROR 16
#define AVERROR(e) (-(e))
#define AV_OPT_TYPE_STRING 5
#define AV_OPT_TYPE_INT 1
#define AV_OPT_TYPE_BOOL 18

void av_log(void* avcl, int level, const char* fmt, ...);

#ifdef __cplusplus
extern "C" {
#endif

struct StDevArgs;
struct StRxSessionPortArgs;
struct StTxSessionPortArgs;
struct st_rx_port;
struct st_tx_port;

void ut_ffmpeg_reset(void);
mtl_handle ut_ffmpeg_get(const struct StDevArgs* args, int* idx);
int ut_ffmpeg_put(mtl_handle handle);
int ut_ffmpeg_parse_rx_port(const struct StDevArgs* dev_args,
                            const struct StRxSessionPortArgs* args,
                            struct st_rx_port* port);
int ut_ffmpeg_parse_tx_port(const struct StDevArgs* dev_args,
                            const struct StTxSessionPortArgs* args,
                            struct st_tx_port* port);
void ut_ffmpeg_block_first_init(void);
bool ut_ffmpeg_wait_for_init_calls(int count);
bool ut_ffmpeg_wait_for_lifecycle_lock_calls(int count);
void ut_ffmpeg_release_init(void);
const struct mtl_init_params* ut_ffmpeg_last_init_params(void);
int ut_ffmpeg_init_calls(void);
int ut_ffmpeg_uninit_calls(void);

#ifdef __cplusplus
}
#endif

#endif