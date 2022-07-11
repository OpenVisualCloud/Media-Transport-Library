/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include <errno.h>
#include <pthread.h>
#include <st_convert_api.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if 0 /* spr */
#define NIC_PORT_BDF "0000:94:00.0"
#define DMA_PORT_BDF "0000:e7:01.0"
#endif

#if 0 /* icelake */
#define NIC_PORT_BDF "0000:4b:00.0"
#define DMA_PORT_BDF "0000:00:01.0"
#endif

#if 1 /* clx */
#define NIC_PORT_BDF "0000:af:00.0"
#define DMA_PORT_BDF "0000:80:04.0"
#endif

/* local ip address for current bdf port */
static uint8_t g_local_ip[ST_IP_ADDR_LEN] = {192, 168, 0, 1};

static void fill_422_planar_le(uint16_t* y, uint16_t* b, uint16_t* r, int w, int h) {
  int pg_size = w * h / 2;

  for (int pg = 0; pg < pg_size; pg++) {
    *b++ = (0 + pg * 4) & 0x3FF;
    *y++ = (1 + pg * 4) & 0x3FF;
    *r++ = (2 + pg * 4) & 0x3FF;
    *y++ = (3 + pg * 4) & 0x3FF;
  }
}

static int perf_cvt_planar_le_to_422_10_pg2(st_handle st, int w, int h, int frames,
                                            int fb_cnt) {
  size_t fb_pg2_size = w * h * 5 / 2;
  st_udma_handle dma = st_udma_create(st, 128, ST_PORT_P);
  struct st20_rfc4175_422_10_pg2_be* pg_be =
      (struct st20_rfc4175_422_10_pg2_be*)malloc(fb_pg2_size * fb_cnt);
  size_t planar_size = w * h * 2 * sizeof(uint16_t);
  float planar_size_m = (float)planar_size / 1024 / 1024;
  uint16_t* p10_u16 = (uint16_t*)st_hp_malloc(st, planar_size * fb_cnt, ST_PORT_P);
  uint16_t* p10_u16_b = p10_u16 + w * h;
  uint16_t* p10_u16_r = p10_u16 + w * h * 3 / 2;
  st_iova_t p10_u16_y_iova = st_hp_virt2iova(st, p10_u16);
  st_iova_t p10_u16_b_iova = p10_u16_y_iova + planar_size / 2;
  st_iova_t p10_u16_r_iova = p10_u16_b_iova + planar_size / 4;
  st_iova_t p10_u16_y_in_iova, p10_u16_b_in_iova, p10_u16_r_in_iova;
  enum st_simd_level cpu_level = st_get_simd_level();

  struct st20_rfc4175_422_10_pg2_be* pg_be_out;
  uint16_t* p10_u16_in;
  uint16_t* p10_u16_b_in;
  uint16_t* p10_u16_r_in;

  for (int i = 0; i < fb_cnt; i++) {
    p10_u16_in = p10_u16 + (i % fb_cnt) * (planar_size / sizeof(*p10_u16));
    p10_u16_b_in = p10_u16_b + (i % fb_cnt) * (planar_size / sizeof(*p10_u16_b));
    p10_u16_r_in = p10_u16_r + (i % fb_cnt) * (planar_size / sizeof(*p10_u16_r));
    fill_422_planar_le(p10_u16_in, p10_u16_b_in, p10_u16_r_in, w, h);
  }

  clock_t start, end;
  float duration;

  start = clock();
  for (int i = 0; i < frames; i++) {
    pg_be_out = pg_be + (i % fb_cnt) * (fb_pg2_size / sizeof(*pg_be));
    p10_u16_in = p10_u16 + (i % fb_cnt) * (planar_size / sizeof(*p10_u16));
    p10_u16_b_in = p10_u16_b + (i % fb_cnt) * (planar_size / sizeof(*p10_u16_b));
    p10_u16_r_in = p10_u16_r + (i % fb_cnt) * (planar_size / sizeof(*p10_u16_r));
    st20_yuv422p10le_to_rfc4175_422be10_simd(p10_u16_in, p10_u16_b_in, p10_u16_r_in,
                                             pg_be_out, w, h, ST_SIMD_LEVEL_NONE);
  }
  end = clock();
  duration = (float)(end - start) / CLOCKS_PER_SEC;
  printf("scalar, time: %f secs with %d frames(%dx%d,%fm@%d buffers)\n", duration, frames,
         w, h, planar_size_m, fb_cnt);

  if (cpu_level >= ST_SIMD_LEVEL_AVX512) {
    start = clock();
    for (int i = 0; i < frames; i++) {
      pg_be_out = pg_be + (i % fb_cnt) * (fb_pg2_size / sizeof(*pg_be));
      p10_u16_in = p10_u16 + (i % fb_cnt) * (planar_size / sizeof(*p10_u16));
      p10_u16_b_in = p10_u16_b + (i % fb_cnt) * (planar_size / sizeof(*p10_u16_b));
      p10_u16_r_in = p10_u16_r + (i % fb_cnt) * (planar_size / sizeof(*p10_u16_r));
      st20_yuv422p10le_to_rfc4175_422be10_simd(p10_u16_in, p10_u16_b_in, p10_u16_r_in,
                                               pg_be_out, w, h, ST_SIMD_LEVEL_AVX512);
    }
    end = clock();
    float duration_simd = (float)(end - start) / CLOCKS_PER_SEC;
    printf("avx512, time: %f secs with %d frames(%dx%d@%d buffers)\n", duration_simd,
           frames, w, h, fb_cnt);
    printf("avx512, %fx performance to scalar\n", duration / duration_simd);
    if (dma) {
      start = clock();
      for (int i = 0; i < frames; i++) {
        pg_be_out = pg_be + (i % fb_cnt) * (fb_pg2_size / sizeof(*pg_be));
        p10_u16_in = p10_u16 + (i % fb_cnt) * (planar_size / sizeof(*p10_u16));
        p10_u16_b_in = p10_u16_b + (i % fb_cnt) * (planar_size / sizeof(*p10_u16_b));
        p10_u16_r_in = p10_u16_r + (i % fb_cnt) * (planar_size / sizeof(*p10_u16_r));
        p10_u16_y_in_iova = p10_u16_y_iova + (i % fb_cnt) * (planar_size);
        p10_u16_b_in_iova = p10_u16_b_iova + (i % fb_cnt) * (planar_size);
        p10_u16_r_in_iova = p10_u16_r_iova + (i % fb_cnt) * (planar_size);
        st20_yuv422p10le_to_rfc4175_422be10_simd_dma(
            dma, p10_u16_in, p10_u16_y_in_iova, p10_u16_b_in, p10_u16_b_in_iova,
            p10_u16_r_in, p10_u16_r_in_iova, pg_be_out, w, h, ST_SIMD_LEVEL_AVX512);
      }
      end = clock();
      float duration_simd = (float)(end - start) / CLOCKS_PER_SEC;
      printf("avx512+dma, time: %f secs with %d frames(%dx%d@%d buffers)\n",
             duration_simd, frames, w, h, fb_cnt);
      printf("avx512+dma, %fx performance to scalar\n", duration / duration_simd);
    }
  }

  if (cpu_level >= ST_SIMD_LEVEL_AVX512_VBMI2) {
    start = clock();
    for (int i = 0; i < frames; i++) {
      pg_be_out = pg_be + (i % fb_cnt) * (fb_pg2_size / sizeof(*pg_be));
      p10_u16_in = p10_u16 + (i % fb_cnt) * (planar_size / sizeof(*p10_u16));
      p10_u16_b_in = p10_u16_b + (i % fb_cnt) * (planar_size / sizeof(*p10_u16_b));
      p10_u16_r_in = p10_u16_r + (i % fb_cnt) * (planar_size / sizeof(*p10_u16_r));
      st20_yuv422p10le_to_rfc4175_422be10_simd(p10_u16_in, p10_u16_b_in, p10_u16_r_in,
                                               pg_be_out, w, h,
                                               ST_SIMD_LEVEL_AVX512_VBMI2);
    }
    end = clock();
    float duration_vbmi = (float)(end - start) / CLOCKS_PER_SEC;
    printf("avx512_vbmi, time: %f secs with %d frames(%dx%d@%d buffers)\n", duration_vbmi,
           frames, w, h, fb_cnt);
    printf("avx512_vbmi, %fx performance to scalar\n", duration / duration_vbmi);
    if (dma) {
      start = clock();
      for (int i = 0; i < frames; i++) {
        pg_be_out = pg_be + (i % fb_cnt) * (fb_pg2_size / sizeof(*pg_be));
        p10_u16_in = p10_u16 + (i % fb_cnt) * (planar_size / sizeof(*p10_u16));
        p10_u16_b_in = p10_u16_b + (i % fb_cnt) * (planar_size / sizeof(*p10_u16_b));
        p10_u16_r_in = p10_u16_r + (i % fb_cnt) * (planar_size / sizeof(*p10_u16_r));
        p10_u16_y_in_iova = p10_u16_y_iova + (i % fb_cnt) * (planar_size);
        p10_u16_b_in_iova = p10_u16_b_iova + (i % fb_cnt) * (planar_size);
        p10_u16_r_in_iova = p10_u16_r_iova + (i % fb_cnt) * (planar_size);
        st20_yuv422p10le_to_rfc4175_422be10_simd_dma(
            dma, p10_u16_in, p10_u16_y_in_iova, p10_u16_b_in, p10_u16_b_in_iova,
            p10_u16_r_in, p10_u16_r_in_iova, pg_be_out, w, h, ST_SIMD_LEVEL_AVX512_VBMI2);
      }
      end = clock();
      float duration_simd = (float)(end - start) / CLOCKS_PER_SEC;
      printf("avx512_vbmi+dma, time: %f secs with %d frames(%dx%d@%d buffers)\n",
             duration_simd, frames, w, h, fb_cnt);
      printf("avx512_vbmi+dma, %fx performance to scalar\n", duration / duration_simd);
    }
  }

  free(pg_be);
  st_hp_free(st, p10_u16);
  if (dma) st_udma_free(dma);

  return 0;
}

static void* perf_thread(void* arg) {
  st_handle dev_handle = arg;
  int frames = 60;
  int fb_cnt = 3;

  unsigned int lcore = 0;
  int ret = st_get_lcore(dev_handle, &lcore);
  if (ret < 0) {
    return NULL;
  }
  st_bind_to_lcore(dev_handle, pthread_self(), lcore);
  printf("%s, run in lcore %u\n", __func__, lcore);

  perf_cvt_planar_le_to_422_10_pg2(dev_handle, 640, 480, frames, fb_cnt);
  perf_cvt_planar_le_to_422_10_pg2(dev_handle, 1280, 720, frames, fb_cnt);
  perf_cvt_planar_le_to_422_10_pg2(dev_handle, 1920, 1080, frames, fb_cnt);
  perf_cvt_planar_le_to_422_10_pg2(dev_handle, 1920 * 2, 1080 * 2, frames, fb_cnt);
  perf_cvt_planar_le_to_422_10_pg2(dev_handle, 1920 * 4, 1080 * 4, frames, fb_cnt);

  st_put_lcore(dev_handle, lcore);

  return NULL;
}

int main(int argc, char** argv) {
  struct st_init_params param;
  int session_num = 1;

  memset(&param, 0, sizeof(param));
  param.num_ports = 1;
  strncpy(param.port[ST_PORT_P], NIC_PORT_BDF, ST_PORT_MAX_LEN);
  memcpy(param.sip_addr[ST_PORT_P], g_local_ip, ST_IP_ADDR_LEN);
  param.flags = ST_FLAG_BIND_NUMA;       // default bind to numa
  param.log_level = ST_LOG_LEVEL_ERROR;  // log level. ERROR, INFO, WARNING
  param.priv = NULL;                     // usr ctx pointer
  param.ptp_get_time_fn = NULL;
  param.tx_sessions_cnt_max = session_num;
  param.rx_sessions_cnt_max = 0;
  param.lcores = NULL;
  // dma port
  strncpy(param.dma_dev_port[0], DMA_PORT_BDF, ST_PORT_MAX_LEN);
  param.num_dma_dev_port = 1;

  // create device
  st_handle dev_handle = st_init(&param);
  if (!dev_handle) {
    printf("st_init fail\n");
    return -EIO;
  }

  pthread_t thread;
  pthread_create(&thread, NULL, perf_thread, dev_handle);
  pthread_join(thread, NULL);

  // destroy device
  if (dev_handle) st_uninit(dev_handle);

  return 0;
}
