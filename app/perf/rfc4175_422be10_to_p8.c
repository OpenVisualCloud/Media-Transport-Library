/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#include "../sample/sample_util.h"

static int perf_cvt_422_10_pg2_be_to_p8(mtl_handle st, int w, int h, int frames,
                                        int fb_cnt) {
  size_t fb_pg10_size = (size_t)w * h * 5 / 2;
  struct st20_rfc4175_422_10_pg2_be *pg_10 =
      (struct st20_rfc4175_422_10_pg2_be *)mtl_hp_malloc(
          st, fb_pg10_size * fb_cnt, MTL_PORT_P);
  size_t fb_pg8_size = (size_t)w * h * 2;
  float fb_pg8_size_m = (float)fb_pg8_size / 1024 / 1024;
  uint8_t *pg_8 = (uint8_t *)malloc(fb_pg8_size * fb_cnt);
  enum mtl_simd_level cpu_level = mtl_get_simd_level();

  struct st20_rfc4175_422_10_pg2_be *pg_10_in;
  uint8_t *pg_8_out;

  for (int i = 0; i < fb_cnt; i++) {
    pg_10_in = pg_10 + (i % fb_cnt) * (fb_pg10_size / sizeof(*pg_10));
    fill_rfc4175_422_10_pg2_data(pg_10_in, w, h);
  }

  clock_t start, end;
  float duration;

  start = clock();
  for (int i = 0; i < frames; i++) {
    pg_10_in = pg_10 + (i % fb_cnt) * (fb_pg10_size / sizeof(*pg_10));
    pg_8_out = pg_8 + (i % fb_cnt) * (fb_pg8_size / sizeof(*pg_8));
    st20_rfc4175_422be10_to_yuv422p8_simd(pg_10_in, pg_8_out, pg_8_out + w * h,
                                          pg_8_out + w * h * 3 / 2, w, h,
                                          MTL_SIMD_LEVEL_NONE);
  }
  end = clock();
  duration = (float)(end - start) / CLOCKS_PER_SEC;
  info("scalar, time: %f secs with %d frames(%dx%d,%fm@%d buffers)\n", duration,
       frames, w, h, fb_pg8_size_m, fb_cnt);

  if (cpu_level >= MTL_SIMD_LEVEL_AVX512) {
    start = clock();
    for (int i = 0; i < frames; i++) {
      pg_10_in = pg_10 + (i % fb_cnt) * (fb_pg10_size / sizeof(*pg_10));
      pg_8_out = pg_8 + (i % fb_cnt) * (fb_pg8_size / sizeof(*pg_8));
      st20_rfc4175_422be10_to_yuv422p8_simd(
          pg_10_in, pg_8_out, pg_8_out + w * h, pg_8_out + w * h * 3 / 2, w, h,
          MTL_SIMD_LEVEL_AVX512);
    }
    end = clock();
    float duration_simd = (float)(end - start) / CLOCKS_PER_SEC;
    info("avx512, time: %f secs with %d frames(%dx%d@%d buffers)\n",
         duration_simd, frames, w, h, fb_cnt);
    info("avx512, %fx performance to scalar\n", duration / duration_simd);
  }

  mtl_hp_free(st, pg_10);
  free(pg_8);

  return 0;
}

static void *perf_thread(void *arg) {
  struct st_sample_context *ctx = arg;
  mtl_handle dev_handle = ctx->st;
  int frames = ctx->perf_frames;
  int fb_cnt = ctx->perf_fb_cnt;

  unsigned int lcore = 0;
  int ret = mtl_get_lcore(dev_handle, &lcore);
  if (ret < 0) {
    return NULL;
  }
  mtl_bind_to_lcore(dev_handle, pthread_self(), lcore);
  info("%s, run in lcore %u\n", __func__, lcore);

  perf_cvt_422_10_pg2_be_to_p8(dev_handle, 640, 480, frames, fb_cnt);
  perf_cvt_422_10_pg2_be_to_p8(dev_handle, 1280, 720, frames, fb_cnt);
  perf_cvt_422_10_pg2_be_to_p8(dev_handle, 1920, 1080, frames, fb_cnt);
  perf_cvt_422_10_pg2_be_to_p8(dev_handle, 1920 * 2, 1080 * 2, frames, fb_cnt);
  perf_cvt_422_10_pg2_be_to_p8(dev_handle, 1920 * 4, 1080 * 4, frames, fb_cnt);

  mtl_put_lcore(dev_handle, lcore);

  return NULL;
}

int main(int argc, char **argv) {
  struct st_sample_context ctx;
  int ret;

  memset(&ctx, 0, sizeof(ctx));
  ret = tx_sample_parse_args(&ctx, argc, argv);
  if (ret < 0)
    return ret;

  ctx.st = mtl_init(&ctx.param);
  if (!ctx.st) {
    err("%s: mtl_init fail\n", __func__);
    return -EIO;
  }

  pthread_t thread;
  ret = pthread_create(&thread, NULL, perf_thread, &ctx);
  if (ret)
    goto exit;
  pthread_join(thread, NULL);

exit:
  /* release sample(st) dev */
  if (ctx.st) {
    mtl_uninit(ctx.st);
    ctx.st = NULL;
  }
  return ret;
}