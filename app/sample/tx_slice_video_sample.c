/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
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

#define TX_VIDEO_PORT_BDF "0000:af:00.1"
#define TX_VIDEO_UDP_PORT (10000)
#define TX_VIDEO_PAYLOAD_TYPE (112)

/* local ip address for current bdf port */
static uint8_t g_tx_video_local_ip[ST_IP_ADDR_LEN] = {192, 168, 0, 2};
/* dst ip address for tx video session */
static uint8_t g_tx_video_dst_ip[ST_IP_ADDR_LEN] = {239, 168, 0, 1};

struct app_context {
  int idx;
  int fb_send;
  void* handle;

  bool stop;
  pthread_t app_thread;
  pthread_cond_t wake_cond;
  pthread_mutex_t wake_mutex;

  int framebuff_size;
  uint16_t framebuff_cnt;
  uint16_t framebuff_producer_idx;
  uint16_t framebuff_consumer_idx;
  struct st_tx_frame* framebuffs;

  int lines_per_slice;
  int height;
};

static int tx_video_next_frame(void* priv, uint16_t* next_frame_idx, bool* second_field) {
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
    *second_field = false;
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

static int tx_video_frame_done(void* priv, uint16_t frame_idx) {
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

static int tx_video_frame_lines_ready(void* priv, uint16_t frame_idx,
                                      uint16_t* lines_ready) {
  struct app_context* s = priv;
  struct st_tx_frame* framebuff = &s->framebuffs[frame_idx];

  st_pthread_mutex_lock(&s->wake_mutex);
  framebuff->slice_trigger = true;
  *lines_ready = framebuff->lines_ready;
  // printf("%s(%d), frame %u ready %d lines\n", __func__, s->idx, frame_idx,
  // framebuff->lines_ready);
  st_pthread_mutex_unlock(&s->wake_mutex);

  return 0;
}

static void tx_video_build_slice(struct app_context* s, struct st_tx_frame* framebuff,
                                 void* frame_addr) {
  int lines_build = 0;
  int slices = (s->height / s->lines_per_slice) + 1;

  /* simulate the timing */
  while (!framebuff->slice_trigger) {
    usleep(1);
  }
  lines_build += s->lines_per_slice;
  st_pthread_mutex_lock(&s->wake_mutex);
  framebuff->lines_ready = lines_build;
  st_pthread_mutex_unlock(&s->wake_mutex);

  while (lines_build < s->height) {
    /* call the real build here, sample just sleep */
    usleep(10 * 1000 / slices);

    st_pthread_mutex_lock(&s->wake_mutex);
    lines_build += s->lines_per_slice;
    if (lines_build > s->height) lines_build = s->height;
    framebuff->lines_ready = lines_build;
    st_pthread_mutex_unlock(&s->wake_mutex);
  }
}

static void* tx_video_slice_thread(void* arg) {
  struct app_context* s = arg;
  uint16_t producer_idx;
  uint16_t consumer_idx;
  struct st_tx_frame* framebuff;

  printf("%s(%d), start\n", __func__, s->idx);
  while (!s->stop) {
    st_pthread_mutex_lock(&s->wake_mutex);
    producer_idx = s->framebuff_producer_idx;
    consumer_idx = s->framebuff_consumer_idx;
    framebuff = &s->framebuffs[producer_idx];
    /* limit the producer to simulate the slice timing */
    if ((producer_idx != consumer_idx) || (ST_TX_FRAME_FREE != framebuff->stat)) {
      /* not in free */
      if (!s->stop) st_pthread_cond_wait(&s->wake_cond, &s->wake_mutex);
      st_pthread_mutex_unlock(&s->wake_mutex);
      continue;
    }

    // printf("%s(%d), producer_idx %d consumer_idx %d\n", __func__, s->idx, producer_idx,
    // consumer_idx);
    void* frame_addr = st20_tx_get_framebuffer(s->handle, producer_idx);

    framebuff->size = s->framebuff_size;
    framebuff->lines_ready = 0;
    framebuff->slice_trigger = false;
    framebuff->stat = ST_TX_FRAME_READY;
    /* point to next */
    producer_idx++;
    if (producer_idx >= s->framebuff_cnt) producer_idx = 0;
    s->framebuff_producer_idx = producer_idx;
    st_pthread_mutex_unlock(&s->wake_mutex);

    tx_video_build_slice(s, framebuff, frame_addr);
  }
  printf("%s(%d), stop\n", __func__, s->idx);

  return NULL;
}

int main() {
  struct st_init_params param;
  int session_num = 1;
  int fb_cnt = 3;

  memset(&param, 0, sizeof(param));
  param.num_ports = 1;
  strncpy(param.port[ST_PORT_P], TX_VIDEO_PORT_BDF, ST_PORT_MAX_LEN);
  memcpy(param.sip_addr[ST_PORT_P], g_tx_video_local_ip, ST_IP_ADDR_LEN);
  param.flags = ST_FLAG_BIND_NUMA;      // default bind to numa
  param.log_level = ST_LOG_LEVEL_INFO;  // log level. ERROR, INFO, WARNING
  param.priv = NULL;                    // usr ctx pointer
  // if not registed, the internal ptp source will be used
  param.ptp_get_time_fn = NULL;
  param.tx_sessions_cnt_max = session_num;
  param.rx_sessions_cnt_max = 0;
  // let lib decide to core or user could define it.
  param.lcores = NULL;

  // create device
  st_handle dev_handle = st_init(&param);
  if (!dev_handle) {
    printf("st_init fail\n");
    return -EIO;
  }

  st20_tx_handle tx_handle[session_num];
  struct app_context* app[session_num];
  int ret;
  // create and register tx session
  for (int i = 0; i < session_num; i++) {
    app[i] = (struct app_context*)malloc(sizeof(struct app_context));
    if (!app[i]) {
      printf(" app struct is not correctly malloc");
      return -ENOMEM;
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
      app[i]->framebuffs[j].lines_ready = 0;
    }

    struct st20_tx_ops ops_tx;
    memset(&ops_tx, 0, sizeof(ops_tx));
    ops_tx.name = "st20_tx";
    ops_tx.priv = app[i];  // app handle register to lib
    ops_tx.num_port = 1;
    // tx src ip like 239.0.0.1
    memcpy(ops_tx.dip_addr[ST_PORT_P], g_tx_video_dst_ip, ST_IP_ADDR_LEN);
    // send port interface like 0000:af:00.0
    strncpy(ops_tx.port[ST_PORT_P], TX_VIDEO_PORT_BDF, ST_PORT_MAX_LEN);
    ops_tx.udp_port[ST_PORT_P] = TX_VIDEO_UDP_PORT + i;  // udp port
    ops_tx.pacing = ST21_PACING_NARROW;
    ops_tx.type = ST20_TYPE_SLICE_LEVEL;
    ops_tx.width = 1920;
    ops_tx.height = 1080;
    ops_tx.fps = ST_FPS_P59_94;
    ops_tx.fmt = ST20_FMT_YUV_422_10BIT;
    ops_tx.payload_type = TX_VIDEO_PAYLOAD_TYPE;
    ops_tx.framebuff_cnt = fb_cnt;
    // app regist non-block func, app could get a frame to send to lib
    ops_tx.get_next_frame = tx_video_next_frame;
    // app regist non-block func, app could get the frame tx done
    ops_tx.notify_frame_done = tx_video_frame_done;
    ops_tx.query_frame_lines_ready = tx_video_frame_lines_ready;
    tx_handle[i] = st20_tx_create(dev_handle, &ops_tx);
    if (!tx_handle[i]) {
      printf("tx_session is not correctly created\n");
      free(app[i]->framebuffs);
      free(app[i]);
      return -EIO;
    }
    app[i]->handle = tx_handle[i];
    app[i]->stop = false;

    app[i]->framebuff_size = st20_tx_get_framebuffer_size(tx_handle[i]);
    app[i]->height = ops_tx.height;
    app[i]->lines_per_slice = app[i]->height / 30;
    st_pthread_mutex_init(&app[i]->wake_mutex, NULL);
    st_pthread_cond_init(&app[i]->wake_cond, NULL);
    ret = pthread_create(&app[i]->app_thread, NULL, tx_video_slice_thread, app[i]);
    if (ret < 0) {
      printf("%s(%d), app_thread create fail %d\n", __func__, ret, i);
      ret = st20_tx_free(tx_handle[i]);
      if (ret) {
        printf("session free failed\n");
      }
      free(app[i]->framebuffs);
      free(app[i]);
      return -EIO;
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
    pthread_join(app[i]->app_thread, NULL);
  }

  // stop tx
  ret = st_stop(dev_handle);

  // release session
  for (int i = 0; i < session_num; i++) {
    ret = st20_tx_free(tx_handle[i]);
    if (ret) {
      printf("session free failed\n");
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
