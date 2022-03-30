/*
 * Copyright (C) 2021 Intel Corporation.
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

#include <errno.h>
#include <pthread.h>
#include <st_convert_api.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void fill_422_10_pd2_data(struct st20_rfc4175_422_10_pg2_be* data, int w, int h) {
  int pg_size = w * h / 2;
  uint16_t cb, y0, cr, y1; /* 10 bit */

  y0 = 0x111;

  cb = 0x222;
  cr = 0x333;
  y1 = y0 + 1;

  for (int pg = 0; pg < pg_size; pg++) {
    data->Cb00 = cb >> 2;
    data->Cb00_ = cb;
    data->Y00 = y0 >> 4;
    data->Y00_ = y0;
    data->Cr00 = cr >> 6;
    data->Cr00_ = cr;
    data->Y01 = y1 >> 8;
    data->Y01_ = y1;
    data++;

    cb++;
    y0 += 2;
    cr++;
    y1 += 2;
  }
}

static int perf_cvt_422_10_pg2_be_to_le(int w, int h, int frames) {
  size_t fb_pg2_size = w * h * 5 / 2;
  struct st20_rfc4175_422_10_pg2_be* pg_be =
      (struct st20_rfc4175_422_10_pg2_be*)malloc(fb_pg2_size);
  struct st20_rfc4175_422_10_pg2_le* pg_le =
      (struct st20_rfc4175_422_10_pg2_le*)malloc(fb_pg2_size);
  size_t planar_size = w * h * 2 * sizeof(uint16_t);
  float planar_size_m = (float)planar_size / 1024 / 1024;
  enum st_simd_level cpu_level = st_get_simd_level();

  fill_422_10_pd2_data(pg_be, w, h);

  clock_t start, end;
  float duration;

  start = clock();
  for (int i = 0; i < frames; i++) {
    st20_rfc4175_422be10_to_422le10_simd(pg_be, pg_le, w, h, ST_SIMD_LEVEL_NONE);
  }
  end = clock();
  duration = (float)(end - start) / CLOCKS_PER_SEC;
  printf("scalar, time: %f secs with %d frames(%dx%d,%fm)\n", duration, frames, w, h,
         planar_size_m);

  if (cpu_level >= ST_SIMD_LEVEL_AVX512) {
    start = clock();
    for (int i = 0; i < frames; i++) {
      st20_rfc4175_422be10_to_422le10_simd(pg_be, pg_le, w, h, ST_SIMD_LEVEL_AVX512);
    }
    end = clock();
    float duration_simd = (float)(end - start) / CLOCKS_PER_SEC;
    printf("avx512, time: %f secs with %d frames(%dx%d)\n", duration_simd, frames, w, h);
    printf("avx512, %fx performance to scalar\n", duration / duration_simd);
  }

  if (cpu_level >= ST_SIMD_LEVEL_AVX512_VBMI2) {
    start = clock();
    for (int i = 0; i < frames; i++) {
      st20_rfc4175_422be10_to_422le10_simd(pg_be, pg_le, w, h,
                                           ST_SIMD_LEVEL_AVX512_VBMI2);
    }
    end = clock();
    float duration_vbmi = (float)(end - start) / CLOCKS_PER_SEC;
    printf("avx512_vbmi, time: %f secs with %d frames(%dx%d)\n", duration_vbmi, frames, w,
           h);
    printf("avx512_vbmi, %fx performance to scalar\n", duration / duration_vbmi);
  }

  free(pg_be);
  free(pg_le);
  return 0;
}

int main(int argc, char** argv) {
  perf_cvt_422_10_pg2_be_to_le(640, 480, 60);
  perf_cvt_422_10_pg2_be_to_le(1280, 720, 60);
  perf_cvt_422_10_pg2_be_to_le(1920, 1080, 60);
  perf_cvt_422_10_pg2_be_to_le(1920 * 2, 1080 * 2, 60);
  perf_cvt_422_10_pg2_be_to_le(1920 * 4, 1080 * 4, 60);
  return 0;
}