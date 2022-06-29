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

#include "../src/app_platform.h"

#define TX_ST22_PORT_BDF "0000:af:00.1"
#define TX_ST22_UDP_PORT (10000)
#define TX_ST22_PAYLOAD_TYPE (114)

/* local ip address for current bdf port */
static uint8_t g_tx_st22_local_ip[ST_IP_ADDR_LEN] = {192, 168, 0, 2};
/* dst ip address for tx video session */
static uint8_t g_tx_st22_dst_ip[ST_IP_ADDR_LEN] = {239, 168, 0, 1};

struct app_context {
  int idx;
  void* handle;
  size_t bytes_per_frame;

  uint16_t framebuff_cnt;
  uint16_t framebuff_producer_idx;
  uint16_t framebuff_consumer_idx;
  struct st_tx_frame* framebuffs;

  bool stop;
  pthread_t encode_thread;

  int fb_send;
  pthread_cond_t wake_cond;
  pthread_mutex_t wake_mutex;
};

static int tx_st22_next_frame(void* priv, uint16_t* next_frame_idx, size_t* frame_size) {
  struct app_context* s = priv;
  int ret;
  uint16_t consumer_idx = s->framebuff_consumer_idx;
  struct st_tx_frame* framebuff = &s->framebuffs[consumer_idx];

  st_pthread_mutex_lock(&s->wake_mutex);
  if (ST_TX_FRAME_READY == framebuff->stat) {
    // printf("%s(%d), next frame idx %u\n", __func__, s->idx, consumer_idx);
    ret = 0;
    framebuff->stat = ST_TX_FRAME_IN_TRANSMITTING;
    *next_frame_idx = consumer_idx;
    *frame_size = framebuff->size;
    /* point to next */
    consumer_idx++;
    if (consumer_idx >= s->framebuff_cnt) consumer_idx = 0;
    s->framebuff_consumer_idx = consumer_idx;
  } else {
    /* not ready */
    ret = -EIO;
  }
  st_pthread_cond_signal(&s->wake_cond);
  st_pthread_mutex_unlock(&s->wake_mutex);

  return ret;
}

static int tx_st22_frame_done(void* priv, uint16_t frame_idx) {
  struct app_context* s = priv;
  int ret;
  struct st_tx_frame* framebuff = &s->framebuffs[frame_idx];

  st_pthread_mutex_lock(&s->wake_mutex);
  if (ST_TX_FRAME_IN_TRANSMITTING == framebuff->stat) {
    ret = 0;
    framebuff->stat = ST_TX_FRAME_FREE;
    // printf("%s(%d), done_idx %u\n", __func__, s->idx, frame_idx);
    s->fb_send++;
  } else {
    ret = -EIO;
    printf("%s(%d), err status %d for frame %u\n", __func__, s->idx, framebuff->stat,
           frame_idx);
  }
  st_pthread_cond_signal(&s->wake_cond);
  st_pthread_mutex_unlock(&s->wake_mutex);

  return ret;
}

static void st22_encode_frame(struct app_context* s, void* codestream_addr,
                              size_t max_codestream_size, size_t* codestream_size) {
  /* call the real encoding here, sample just sleep */
  usleep(10 * 1000);
  *codestream_size = s->bytes_per_frame;
}

static void* st22_encode_thread(void* arg) {
  struct app_context* s = arg;
  uint16_t producer_idx;
  struct st_tx_frame* framebuff;

  printf("%s(%d), start\n", __func__, s->idx);
  while (!s->stop) {
    st_pthread_mutex_lock(&s->wake_mutex);
    producer_idx = s->framebuff_producer_idx;
    framebuff = &s->framebuffs[producer_idx];
    if (ST_TX_FRAME_FREE != framebuff->stat) {
      /* not in free */
      if (!s->stop) st_pthread_cond_wait(&s->wake_cond, &s->wake_mutex);
      st_pthread_mutex_unlock(&s->wake_mutex);
      continue;
    }
    st_pthread_mutex_unlock(&s->wake_mutex);

    void* frame_addr = st22_tx_get_fb_addr(s->handle, producer_idx);
    size_t max_framesize = s->bytes_per_frame;
    size_t codestream_size = s->bytes_per_frame;
    st22_encode_frame(s, frame_addr, max_framesize, &codestream_size);

    st_pthread_mutex_lock(&s->wake_mutex);
    framebuff->size = codestream_size;
    framebuff->stat = ST_TX_FRAME_READY;
    /* point to next */
    producer_idx++;
    if (producer_idx >= s->framebuff_cnt) producer_idx = 0;
    s->framebuff_producer_idx = producer_idx;
    st_pthread_mutex_unlock(&s->wake_mutex);
  }
  printf("%s(%d), stop\n", __func__, s->idx);

  return NULL;
}

int main() {
  struct st_init_params param;
  int session_num = 1;
  int bpp = 3; /* 3bit per pixel */
  int fb_cnt = 3;

  memset(&param, 0, sizeof(param));
  param.num_ports = 1;
  strncpy(param.port[ST_PORT_P], TX_ST22_PORT_BDF, ST_PORT_MAX_LEN);
  memcpy(param.sip_addr[ST_PORT_P], g_tx_st22_local_ip, ST_IP_ADDR_LEN);
  param.flags = ST_FLAG_BIND_NUMA;      // default bind to numa
  param.log_level = ST_LOG_LEVEL_INFO;  // log level. ERROR, INFO, WARNING
  param.priv = NULL;                    // usr ctx pointer
  param.ptp_get_time_fn = NULL;
  param.tx_sessions_cnt_max = session_num;
  param.rx_sessions_cnt_max = 0;
  param.lcores = NULL;
  param.nb_tx_desc = 128;
  // create device
  st_handle dev_handle = st_init(&param);
  if (!dev_handle) {
    printf("%s, st_init fail\n", __func__);
    return -1;
  }

  st22_tx_handle tx_handle[session_num];
  struct app_context* app[session_num];
  int ret;
  // create and register tx session
  for (int i = 0; i < session_num; i++) {
    app[i] = (struct app_context*)malloc(sizeof(struct app_context));
    if (!app[i]) {
      printf("%s, app struct malloc fail\n", __func__);
      return -1;
    }
    memset(app[i], 0, sizeof(struct app_context));
    app[i]->idx = i;

    app[i]->framebuff_cnt = fb_cnt;
    app[i]->framebuffs =
        (struct st_tx_frame*)malloc(sizeof(*app[i]->framebuffs) * app[i]->framebuff_cnt);
    if (!app[i]->framebuffs) {
      printf("%s, framebuffs malloc fail\n", __func__);
      free(app[i]);
      return -1;
    }
    for (uint16_t j = 0; j < app[i]->framebuff_cnt; j++) {
      app[i]->framebuffs[j].stat = ST_TX_FRAME_FREE;
    }

    struct st22_tx_ops ops_tx;
    memset(&ops_tx, 0, sizeof(ops_tx));
    ops_tx.name = "st22_test";
    ops_tx.priv = app[i];  // app handle register to lib
    ops_tx.num_port = 1;
    // tx src ip like 239.0.0.1
    memcpy(ops_tx.dip_addr[ST_PORT_P], g_tx_st22_dst_ip, ST_IP_ADDR_LEN);
    // send port interface like 0000:af:00.0
    strncpy(ops_tx.port[ST_PORT_P], TX_ST22_PORT_BDF, ST_PORT_MAX_LEN);
    ops_tx.udp_port[ST_PORT_P] =
        TX_ST22_UDP_PORT + i;  // user could config the udp port in this interface.
    ops_tx.pacing = ST21_PACING_NARROW;
    ops_tx.width = 1920;
    ops_tx.height = 1080;
    ops_tx.fps = ST_FPS_P59_94;
    ops_tx.payload_type = TX_ST22_PAYLOAD_TYPE;
    ops_tx.type = ST22_TYPE_FRAME_LEVEL;
    ops_tx.pack_type = ST22_PACK_CODESTREAM;

    app[i]->bytes_per_frame = ops_tx.width * ops_tx.height * bpp / 8;
    ops_tx.framebuff_cnt = fb_cnt;
    /* set to the max size per frame if not CBR model */
    ops_tx.framebuff_max_size = app[i]->bytes_per_frame;
    ops_tx.get_next_frame = tx_st22_next_frame;
    ops_tx.notify_frame_done = tx_st22_frame_done;

    tx_handle[i] = st22_tx_create(dev_handle, &ops_tx);
    if (!tx_handle[i]) {
      printf("%s, tx_session create fail", __func__);
      free(app[i]->framebuffs);
      free(app[i]);
      return -1;
    }
    app[i]->handle = tx_handle[i];

    app[i]->stop = false;
    st_pthread_mutex_init(&app[i]->wake_mutex, NULL);
    st_pthread_cond_init(&app[i]->wake_cond, NULL);
    ret = pthread_create(&app[i]->encode_thread, NULL, st22_encode_thread, app[i]);
    if (ret < 0) {
      printf("%s(%d), encode_thread create fail %d\n", __func__, ret, i);
      ret = st22_tx_free(tx_handle[i]);
      if (ret < 0) {
        printf("%s, session free failed\n", __func__);
      }
      free(app[i]->framebuffs);
      free(app[i]);
      return -1;
    }
  }

  // start tx
  ret = st_start(dev_handle);

  // tx 120s
  sleep(120);

  // stop app thread
  for (int i = 0; i < session_num; i++) {
    app[i]->stop = true;
    st_pthread_mutex_lock(&app[i]->wake_mutex);
    st_pthread_cond_signal(&app[i]->wake_cond);
    st_pthread_mutex_unlock(&app[i]->wake_mutex);
    pthread_join(app[i]->encode_thread, NULL);
  }

  // stop tx
  ret = st_stop(dev_handle);

  // release session
  for (int i = 0; i < session_num; i++) {
    printf("%s, fb_send %d\n", __func__, app[i]->fb_send);
    ret = st22_tx_free(tx_handle[i]);
    if (ret < 0) {
      printf("%s, session free failed\n", __func__);
    }
    st_pthread_mutex_destroy(&app[i]->wake_mutex);
    st_pthread_cond_destroy(&app[i]->wake_cond);

    free(app[i]->framebuffs);
    free(app[i]);
  }

  // destroy device
  st_uninit(dev_handle);
  return 0;
}
