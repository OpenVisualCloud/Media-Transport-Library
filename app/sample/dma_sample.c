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
#include <openssl/md5.h>
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
  int fb_size = element_size * nb_elements;
  int fb_dst_iova_off = 0, fb_src_iova_off = 0;

  /* create user dma dev */
  dma = st_udma_create(st, nb_desc, ST_PORT_P);
  if (!dma) {
    printf("dma create fail\n");
    return -EIO;
  }

  void *fb_dst = NULL, *fb_src = NULL;
  st_iova_t fb_dst_iova, fb_src_iova;
  unsigned char fb_dst_md5s[MD5_DIGEST_LENGTH];
  unsigned char fb_src_md5s[MD5_DIGEST_LENGTH];

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
  MD5((unsigned char*)fb_src, fb_size, fb_src_md5s);

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

  /* all copy completed, check md5 */
  MD5((unsigned char*)fb_dst, fb_size, fb_dst_md5s);
  ret = memcmp(fb_dst_md5s, fb_src_md5s, MD5_DIGEST_LENGTH);
  if (ret != 0) {
    printf("md5 check fail\n");
  } else {
    printf("dma copy %dk with time %dus\n", fb_size / 1024,
           (int)(end_ns - start_ns) / 1000);
  }

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

  dma_copy_sample(dev_handle);

  // destroy device
  st_uninit(dev_handle);
  return 0;
}
