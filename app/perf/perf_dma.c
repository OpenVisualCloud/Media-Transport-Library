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
#include <st_dpdk_api.h>
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

static inline void rand_data(uint8_t* p, size_t sz, uint8_t base) {
  for (size_t i = 0; i < sz; i++) {
    p[i] = rand();
  }
}

static int dma_copy_perf(st_handle st, int w, int h, int frames, int pkt_size) {
  st_udma_handle dma;
  int ret;
  uint16_t nb_desc = 1024;
  size_t fb_size = w * h * 5 / 2;            /* rfc4175_422be10 */
  fb_size = (fb_size / pkt_size) * pkt_size; /* align to pkt_size */
  float fb_size_m = (float)fb_size / 1024 / 1024;
  int fb_dst_iova_off = 0, fb_src_iova_off = 0;

  /* create user dma dev */
  dma = st_udma_create(st, nb_desc, ST_PORT_P);
  if (!dma) {
    printf("dma create fail\n");
    return -EIO;
  }

  void *fb_dst = NULL, *fb_src = NULL;
  st_iova_t fb_dst_iova, fb_src_iova;

  /* allocate fb dst and src(with random data) */
  fb_dst = st_hp_malloc(st, fb_size, ST_PORT_P);
  if (!fb_dst) {
    printf("fb dst create fail\n");
    st_udma_free(dma);
    return -ENOMEM;
  }
  fb_dst_iova = st_hp_virt2iova(st, fb_dst);
  fb_src = st_hp_malloc(st, fb_size, ST_PORT_P);
  if (!fb_dst) {
    printf("fb src create fail\n");
    st_hp_free(st, fb_dst);
    st_udma_free(dma);
    return -ENOMEM;
  }
  fb_src_iova = st_hp_virt2iova(st, fb_src);
  rand_data((uint8_t*)fb_src, fb_size, 0);

  clock_t start, end;
  float duration_cpu, duration_simd, duration_dma;

  start = clock();
  for (int idx = 0; idx < frames; idx++) {
    size_t copied_size = 0;
    while (copied_size < fb_size) {
      memcpy(fb_src + pkt_size, fb_dst + pkt_size, pkt_size);
      copied_size += pkt_size;
    }
  }
  end = clock();
  duration_cpu = (float)(end - start) / CLOCKS_PER_SEC;
  printf("cpu, time: %f secs with %d frames(%dx%d,%fm), pkt_size %d\n", duration_cpu,
         frames, w, h, fb_size_m, pkt_size);

  start = clock();
  for (int idx = 0; idx < frames; idx++) {
    size_t copied_size = 0;
    while (copied_size < fb_size) {
      st_memcpy(fb_src + pkt_size, fb_dst + pkt_size, pkt_size);
      copied_size += pkt_size;
    }
  }
  end = clock();
  duration_simd = (float)(end - start) / CLOCKS_PER_SEC;
  printf("simd, time: %f secs with %d frames(%dx%d,%fm), pkt_size %d\n", duration_simd,
         frames, w, h, fb_size_m, pkt_size);
  printf("simd, %fx performance to cpu\n", duration_cpu / duration_simd);

  start = clock();
  for (int idx = 0; idx < frames; idx++) {
    while (fb_dst_iova_off < fb_size) {
      /* try to copy */
      while (fb_src_iova_off < fb_size) {
        ret = st_udma_copy(dma, fb_dst_iova + fb_src_iova_off,
                           fb_src_iova + fb_src_iova_off, pkt_size);
        if (ret < 0) break;
        fb_src_iova_off += pkt_size;
      }
      /* submit */
      st_udma_submit(dma);

      /* check complete */
      uint16_t nb_dq = st_udma_completed(dma, 32);
      fb_dst_iova_off += pkt_size * nb_dq;
    }
  }
  end = clock();
  duration_dma = (float)(end - start) / CLOCKS_PER_SEC;
  printf("dma, time: %f secs with %d frames(%dx%d,%fm), pkt_size %d\n", duration_dma,
         frames, w, h, fb_size_m, pkt_size);
  printf("dma, %fx performance to cpu\n", duration_cpu / duration_dma);
  printf("\n");

  st_hp_free(st, fb_dst);
  st_hp_free(st, fb_src);

  ret = st_udma_free(dma);
  return 0;
}

int main() {
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

  dma_copy_perf(dev_handle, 1920, 1080, 60, 128);
  dma_copy_perf(dev_handle, 1920 * 2, 1080 * 2, 60, 128);
  dma_copy_perf(dev_handle, 1920 * 4, 1080 * 4, 60, 128);
  printf("\n");

  dma_copy_perf(dev_handle, 1920, 1080, 60, 1200);
  dma_copy_perf(dev_handle, 1920 * 2, 1080 * 2, 60, 1200);
  dma_copy_perf(dev_handle, 1920 * 4, 1080 * 4, 60, 1200);
  printf("\n");

  dma_copy_perf(dev_handle, 1920, 1080, 60, 4096);
  dma_copy_perf(dev_handle, 1920 * 2, 1080 * 2, 60, 4096);
  dma_copy_perf(dev_handle, 1920 * 4, 1080 * 4, 60, 4096);
  printf("\n");

  // destroy device
  st_uninit(dev_handle);
  return 0;
}
