/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include <errno.h>
#include <openssl/sha.h>
#include <pthread.h>
#include <st_dpdk_api.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NIC_PORT_BDF "0000:af:00.0"
#define DMA_PORT_BDF "0000:80:04.0"

/* local ip address for current bdf port */
static uint8_t g_local_ip[ST_IP_ADDR_LEN] = {192, 168, 0, 1};

static inline void rand_data(uint8_t* p, size_t sz, uint8_t base) {
  for (size_t i = 0; i < sz; i++) {
    p[i] = rand();
  }
}

static int dma_copy_sample(st_handle st) {
  st_udma_handle dma;
  int ret;
  uint16_t nb_desc = 1024;
  int nb_elements = nb_desc * 8, element_size = 1260;
  size_t fb_size = element_size * nb_elements;
  int fb_dst_iova_off = 0, fb_src_iova_off = 0;

  /* create user dma dev */
  dma = st_udma_create(st, nb_desc, ST_PORT_P);
  if (!dma) {
    printf("dma create fail\n");
    return -EIO;
  }

  void *fb_dst = NULL, *fb_src = NULL;
  st_iova_t fb_dst_iova, fb_src_iova;
  unsigned char fb_dst_shas[SHA256_DIGEST_LENGTH];
  unsigned char fb_src_shas[SHA256_DIGEST_LENGTH];

  /* allocate fb dst and src(with random data) */
  fb_dst = st_hp_malloc(st, fb_size, ST_PORT_P);
  if (!fb_dst) {
    printf("fb dst create fail\n");
    st_udma_free(dma);
    return -ENOMEM;
  }
  fb_dst_iova = st_hp_virt2iova(st, fb_dst);
  fb_src = st_hp_malloc(st, fb_size, ST_PORT_P);
  if (!fb_src) {
    printf("fb src create fail\n");
    st_hp_free(st, fb_dst);
    st_udma_free(dma);
    return -ENOMEM;
  }
  fb_src_iova = st_hp_virt2iova(st, fb_src);
  rand_data((uint8_t*)fb_src, fb_size, 0);
  SHA256((unsigned char*)fb_src, fb_size, fb_src_shas);

  uint64_t start_ns = st_ptp_read_time(st);
  while (fb_dst_iova_off < fb_size) {
    /* try to copy */
    while (fb_src_iova_off < fb_size) {
      ret = st_udma_copy(dma, fb_dst_iova + fb_src_iova_off,
                         fb_src_iova + fb_src_iova_off, element_size);
      if (ret < 0) break;
      fb_src_iova_off += element_size;
    }
    /* submit */
    st_udma_submit(dma);

    /* do any other job*/

    /* check complete */
    uint16_t nb_dq = st_udma_completed(dma, 32);
    fb_dst_iova_off += element_size * nb_dq;
  }
  uint64_t end_ns = st_ptp_read_time(st);

  /* all copy completed, check sha */
  SHA256((unsigned char*)fb_dst, fb_size, fb_dst_shas);
  ret = memcmp(fb_dst_shas, fb_src_shas, SHA256_DIGEST_LENGTH);
  if (ret != 0) {
    printf("sha check fail\n");
  } else {
    printf("dma copy %" PRIu64 "k with time %dus\n", fb_size / 1024,
           (int)(end_ns - start_ns) / 1000);
  }

  st_hp_free(st, fb_dst);
  st_hp_free(st, fb_src);

  ret = st_udma_free(dma);
  return 0;
}

static int dma_map_copy_sample(st_handle st) {
  st_udma_handle dma = NULL;
  int ret = -EIO;
  uint16_t nb_desc = 1024;
  int nb_elements = nb_desc * 8, element_size = 1260;
  size_t fb_size = element_size * nb_elements;
  size_t pg_sz = st_page_size(st);
  /* 2 more pages to hold the head and tail */
  size_t fb_size_malloc = fb_size + 2 * pg_sz;
  int fb_dst_iova_off = 0, fb_src_iova_off = 0;

  /* create user dma dev */
  dma = st_udma_create(st, nb_desc, ST_PORT_P);
  if (!dma) {
    printf("%s: dma create fail\n", __func__);
    return -EIO;
  }

  void *fb_dst_malloc = NULL, *fb_src_malloc = NULL;
  void *fb_dst = NULL, *fb_src = NULL;
  st_iova_t fb_dst_iova = ST_BAD_IOVA, fb_src_iova = ST_BAD_IOVA;
  unsigned char fb_dst_shas[SHA256_DIGEST_LENGTH];
  unsigned char fb_src_shas[SHA256_DIGEST_LENGTH];

  /* allocate fb dst and src(with random data) */
  fb_dst_malloc = malloc(fb_size_malloc);
  if (!fb_dst_malloc) {
    printf("%s: fb dst malloc fail\n", __func__);
    ret = -ENOMEM;
    goto out;
  }
  fb_dst = (void*)ST_ALIGN((uint64_t)fb_dst_malloc, pg_sz);
  fb_dst_iova = st_dma_map(st, fb_dst, fb_size);
  if (fb_dst_iova == ST_BAD_IOVA) {
    printf("%s: fb dst mmap fail\n", __func__);
    ret = -EIO;
    goto out;
  }

  fb_src_malloc = malloc(fb_size_malloc);
  if (!fb_src_malloc) {
    printf("%s: fb src malloc fail\n", __func__);
    ret = -ENOMEM;
    goto out;
  }
  fb_src = (void*)ST_ALIGN((uint64_t)fb_src_malloc, pg_sz);
  fb_src_iova = st_dma_map(st, fb_src, fb_size);
  if (fb_src_iova == ST_BAD_IOVA) {
    printf("%s: fb src mmap fail\n", __func__);
    ret = -EIO;
    goto out;
  }

  rand_data((uint8_t*)fb_src, fb_size, 0);
  SHA256((unsigned char*)fb_src, fb_size, fb_src_shas);

  uint64_t start_ns = st_ptp_read_time(st);
  while (fb_dst_iova_off < fb_size) {
    /* try to copy */
    while (fb_src_iova_off < fb_size) {
      ret = st_udma_copy(dma, fb_dst_iova + fb_src_iova_off,
                         fb_src_iova + fb_src_iova_off, element_size);
      if (ret < 0) break;
      fb_src_iova_off += element_size;
    }
    /* submit */
    st_udma_submit(dma);

    /* do any other job*/

    /* check complete */
    uint16_t nb_dq = st_udma_completed(dma, 32);
    fb_dst_iova_off += element_size * nb_dq;
  }
  uint64_t end_ns = st_ptp_read_time(st);

  /* all copy completed, check sha */
  SHA256((unsigned char*)fb_dst, fb_size, fb_dst_shas);
  ret = memcmp(fb_dst_shas, fb_src_shas, SHA256_DIGEST_LENGTH);
  if (ret != 0) {
    printf("%s: sha check fail\n", __func__);
  } else {
    printf("%s: dma map copy %" PRIu64 "k with time %dus\n", __func__, fb_size / 1024,
           (int)(end_ns - start_ns) / 1000);
  }

out:
  if (fb_src_malloc) {
    if (fb_src_iova != ST_BAD_IOVA) st_dma_unmap(st, fb_src, fb_src_iova, fb_size);
    free(fb_src_malloc);
  }
  if (fb_dst_malloc) {
    if (fb_dst_iova != ST_BAD_IOVA) st_dma_unmap(st, fb_dst, fb_dst_iova, fb_size);
    free(fb_dst_malloc);
  }
  if (dma) st_udma_free(dma);
  return ret;
}

int main() {
  struct st_init_params param;
  int session_num = 1;

  memset(&param, 0, sizeof(param));
  param.num_ports = 1;
  strncpy(param.port[ST_PORT_P], NIC_PORT_BDF, ST_PORT_MAX_LEN);
  memcpy(param.sip_addr[ST_PORT_P], g_local_ip, ST_IP_ADDR_LEN);
  param.flags = ST_FLAG_BIND_NUMA;      // default bind to numa
  param.log_level = ST_LOG_LEVEL_INFO;  // log level. ERROR, INFO, WARNING
  param.priv = NULL;                    // usr ctx pointer
  // if not registed, the internal ptp source will be used
  param.ptp_get_time_fn = NULL;
  param.tx_sessions_cnt_max = session_num;
  param.rx_sessions_cnt_max = 0;
  // let lib decide to core or user could define it.
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

  /* dma copy with st_hp_*** memory */
  dma_copy_sample(dev_handle);
  /* dma copy with malloc/free memory, use map before passing to DMA */
  dma_map_copy_sample(dev_handle);

  // destroy device
  st_uninit(dev_handle);
  return 0;
}
