/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include "st22_avcodec_harness.h"

#include <pthread.h>

/* No worker thread: it would get frames from the pipeline, and the test calls
 * avcodec_encode_frame() itself. */
static int ut_no_thread(pthread_t* thread, const pthread_attr_t* attr,
                        void* (*start)(void*), void* arg) {
  return 0;
}

#define pthread_create ut_no_thread
#include "../st22_avcodec_plugin.c"
#undef pthread_create

/* 128 wide, so each plane fills the AVFrame plane the plugin copies it into at any
 * FFmpeg stride alignment, 32 or 64 bytes */
#define UT_WIDTH 128
#define UT_HEIGHT 64
#define UT_LUMA_SIZE ((size_t)UT_WIDTH * UT_HEIGHT)

int ut_avcodec_encode_frame(uint8_t* dst, size_t buffer_size, size_t* data_size) {
  static uint8_t planes[UT_LUMA_SIZE * 3 / 2];
  struct st22_encoder_create_req req = {
      .codestream_size = UT_LUMA_SIZE,
      .width = UT_WIDTH,
      .height = UT_HEIGHT,
      .fps = ST_FPS_P50,
      .input_fmt = ST_FRAME_FMT_YUV420PLANAR8,
      .output_fmt = ST_FRAME_FMT_H264_CODESTREAM,
  };
  enum st_frame_fmt fmt = ST_FRAME_FMT_YUV420PLANAR8;
  struct st_frame src = {
      .addr = {planes, planes + UT_LUMA_SIZE, planes + UT_LUMA_SIZE * 5 / 4},
      .linesize = {st_frame_least_linesize(fmt, UT_WIDTH, 0),
                   st_frame_least_linesize(fmt, UT_WIDTH, 1),
                   st_frame_least_linesize(fmt, UT_WIDTH, 2)},
      .fmt = fmt,
      .width = UT_WIDTH,
      .height = UT_HEIGHT,
  };
  /* data_size starts at buffer_size, so a rejected packet must clear it */
  struct st_frame codestream = {
      .addr = {dst},
      .fmt = ST_FRAME_FMT_H264_CODESTREAM,
      .buffer_size = buffer_size,
      .data_size = buffer_size,
  };
  struct st22_encode_frame_meta frame = {.src = &src, .dst = &codestream};
  struct st22_avcodec_encoder_session session = {0};
  int ret;

  if (avcodec_encoder_init_session(&session, &req) < 0) return 1;
  ret = avcodec_encode_frame(&session, &frame);
  avcodec_encoder_uinit_session(&session);
  *data_size = codestream.data_size;
  return ret;
}
