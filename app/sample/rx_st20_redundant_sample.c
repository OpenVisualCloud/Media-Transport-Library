/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <st20_redundant_api.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/app_platform.h"

#define RX_VIDEO_UDP_PORT_P (20000)
#define RX_VIDEO_PAYLOAD_TYPE (112)
#define RX_VIDEO_UDP_PORT_R (20000)

/* p local ip address for current bdf port */
static uint8_t g_rx_video_local_ip_p[ST_IP_ADDR_LEN] = {192, 168, 0, 1};
/* p source ip address for rx video session */
static uint8_t g_rx_video_source_ip_p[ST_IP_ADDR_LEN] = {239, 168, 85, 20};
/* r local ip address for current bdf port */
static uint8_t g_rx_video_local_ip_r[ST_IP_ADDR_LEN] = {192, 168, 0, 2};
/* r source ip address for rx video session */
static uint8_t g_rx_video_source_ip_r[ST_IP_ADDR_LEN] = {239, 168, 86, 20};

static bool g_video_active = false;
static st_handle g_st_handle;

struct app_context {
  int idx;
  int fb_rec;
  st20r_rx_handle handle;

  bool stop;
  pthread_t app_thread;
  pthread_cond_t wake_cond;
  pthread_mutex_t wake_mutex;

  uint16_t framebuff_cnt;
  uint16_t framebuff_producer_idx;
  uint16_t framebuff_consumer_idx;
  struct st_rx_frame* framebuffs;
};

static int rx_video_enqueue_frame(struct app_context* s, void* frame, size_t size) {
  uint16_t producer_idx = s->framebuff_producer_idx;
  struct st_rx_frame* framebuff = &s->framebuffs[producer_idx];

  if (framebuff->frame) {
    return -EBUSY;
  }

  // printf("%s(%d), frame idx %d\n", __func__, s->idx, producer_idx);
  framebuff->frame = frame;
  framebuff->size = size;
  /* point to next */
  producer_idx++;
  if (producer_idx >= s->framebuff_cnt) producer_idx = 0;
  s->framebuff_producer_idx = producer_idx;
  return 0;
}

static int rx_video_frame_ready(void* priv, void* frame,
                                struct st20_rx_frame_meta* meta) {
  struct app_context* s = (struct app_context*)priv;

  if (!s->handle) return -EIO;

  /* incomplete frame */
  if (!st_is_frame_complete(meta->status)) {
    st20r_rx_put_frame(s->handle, frame);
    return 0;
  }

  st_pthread_mutex_lock(&s->wake_mutex);
  int ret = rx_video_enqueue_frame(s, frame, meta->frame_total_size);
  if (ret < 0) {
    printf("%s(%d), frame %p dropped\n", __func__, s->idx, frame);
    /* free the queue */
    st20r_rx_put_frame(s->handle, frame);
    st_pthread_mutex_unlock(&s->wake_mutex);
    return ret;
  }
  st_pthread_cond_signal(&s->wake_cond);
  st_pthread_mutex_unlock(&s->wake_mutex);

  return 0;
}

static void rx_video_consume_frame(struct app_context* s, void* frame,
                                   size_t frame_size) {
  // printf("%s(%d), frame %p\n", __func__, s->idx, frame);

  /* call the real consumer here, sample just sleep */
  st_usleep(10 * 1000);
  s->fb_rec++;
}

static void* rx_video_frame_thread(void* arg) {
  struct app_context* s = arg;
  int idx = s->idx;
  int consumer_idx;
  struct st_rx_frame* framebuff;

  printf("%s(%d), start\n", __func__, idx);
  while (!s->stop) {
    st_pthread_mutex_lock(&s->wake_mutex);
    consumer_idx = s->framebuff_consumer_idx;
    framebuff = &s->framebuffs[consumer_idx];
    if (!framebuff->frame) {
      /* no ready frame */
      if (!s->stop) st_pthread_cond_wait(&s->wake_cond, &s->wake_mutex);
      st_pthread_mutex_unlock(&s->wake_mutex);
      continue;
    }
    st_pthread_mutex_unlock(&s->wake_mutex);

    // printf("%s(%d), frame idx %d\n", __func__, idx, consumer_idx);
    rx_video_consume_frame(s, framebuff->frame, framebuff->size);
    st20r_rx_put_frame(s->handle, framebuff->frame);
    /* point to next */
    st_pthread_mutex_lock(&s->wake_mutex);
    framebuff->frame = NULL;
    consumer_idx++;
    if (consumer_idx >= s->framebuff_cnt) consumer_idx = 0;
    s->framebuff_consumer_idx = consumer_idx;
    st_pthread_mutex_unlock(&s->wake_mutex);
  }
  printf("%s(%d), stop\n", __func__, idx);

  return NULL;
}

static void app_sig_handler(int signo) {
  printf("%s, signal %d\n", __func__, signo);
  switch (signo) {
    case SIGINT: /* Interrupt from keyboard */
      g_video_active = false;
      st_request_exit(g_st_handle);
      break;
  }

  return;
}

int main() {
  struct st_init_params param;
  int session_num = 1;
  int fb_cnt = 3;
  char* port_p = getenv("ST_PORT_P");
  if (!port_p) port_p = "0000:af:01.0";
  char* port_r = getenv("ST_PORT_R");
  if (!port_r) port_r = "0000:af:01.1";

  memset(&param, 0, sizeof(param));
  param.num_ports = 2;
  strncpy(param.port[ST_PORT_P], port_p, ST_PORT_MAX_LEN - 1);
  strncpy(param.port[ST_PORT_R], port_r, ST_PORT_MAX_LEN - 1);
  memcpy(param.sip_addr[ST_PORT_P], g_rx_video_local_ip_p, ST_IP_ADDR_LEN);
  memcpy(param.sip_addr[ST_PORT_R], g_rx_video_local_ip_r, ST_IP_ADDR_LEN);
  param.flags = ST_FLAG_BIND_NUMA;      // default bind to numa
  param.log_level = ST_LOG_LEVEL_INFO;  // log level. ERROR, INFO, WARNING
  param.rx_sessions_cnt_max = session_num;

  // create device
  st_handle dev_handle = st_init(&param);
  if (!dev_handle) {
    printf("%s, st_init fail\n", __func__);
    return -EIO;
  }

  g_st_handle = dev_handle;
  signal(SIGINT, app_sig_handler);

  st20r_rx_handle rx_handle[session_num];
  struct app_context* app[session_num];
  int ret;
  // create and register rx session
  for (int i = 0; i < session_num; i++) {
    app[i] = (struct app_context*)malloc(sizeof(struct app_context));
    if (!app[i]) {
      printf("%s, app malloc fail on %d\n", __func__, i);
      ret = -ENOMEM;
      goto error;
    }
    memset(app[i], 0, sizeof(struct app_context));
    app[i]->idx = i;
    app[i]->stop = false;
    st_pthread_mutex_init(&app[i]->wake_mutex, NULL);
    st_pthread_cond_init(&app[i]->wake_cond, NULL);
    app[i]->framebuff_cnt = fb_cnt;
    app[i]->framebuffs =
        (struct st_rx_frame*)malloc(sizeof(*app[i]->framebuffs) * app[i]->framebuff_cnt);
    if (!app[i]->framebuffs) {
      printf("%s, framebuffs malloc fail on %d\n", __func__, i);
      ret = -ENOMEM;
      goto error;
    }
    for (uint16_t j = 0; j < app[i]->framebuff_cnt; j++)
      app[i]->framebuffs[j].frame = NULL;
    app[i]->framebuff_producer_idx = 0;
    app[i]->framebuff_consumer_idx = 0;

    struct st20r_rx_ops ops_rx;
    memset(&ops_rx, 0, sizeof(ops_rx));
    ops_rx.name = "st20r_test";
    ops_rx.priv = app[i];  // app handle register to lib
    ops_rx.num_port = 2;
    memcpy(ops_rx.sip_addr[ST_PORT_P], g_rx_video_source_ip_p, ST_IP_ADDR_LEN);
    memcpy(ops_rx.sip_addr[ST_PORT_R], g_rx_video_source_ip_r, ST_IP_ADDR_LEN);
    strncpy(ops_rx.port[ST_PORT_P], port_p, ST_PORT_MAX_LEN - 1);
    strncpy(ops_rx.port[ST_PORT_R], port_r, ST_PORT_MAX_LEN - 1);
    ops_rx.udp_port[ST_PORT_P] = RX_VIDEO_UDP_PORT_P + i;
    ops_rx.udp_port[ST_PORT_R] = RX_VIDEO_UDP_PORT_R + i;

    ops_rx.width = 1920;
    ops_rx.height = 1080;
    ops_rx.fps = ST_FPS_P59_94;
    ops_rx.fmt = ST20_FMT_YUV_422_10BIT;
    ops_rx.framebuff_cnt = fb_cnt;
    ops_rx.payload_type = RX_VIDEO_PAYLOAD_TYPE;
    ops_rx.notify_frame_ready = rx_video_frame_ready;

    rx_handle[i] = st20r_rx_create(dev_handle, &ops_rx);
    if (!rx_handle[i]) {
      printf("%s, rx create fail on %d\n", __func__, i);
      ret = -EIO;
      goto error;
    }
    app[i]->handle = rx_handle[i];

    ret = pthread_create(&app[i]->app_thread, NULL, rx_video_frame_thread, app[i]);
    if (ret < 0) {
      printf("%s(%d), app_thread create fail %d\n", __func__, ret, i);
      goto error;
    }
  }

  // start rx
  ret = st_start(dev_handle);

  g_video_active = true;

  // rx run
  while (g_video_active) {
    sleep(1);
  }

  // stop app thread
  for (int i = 0; i < session_num; i++) {
    app[i]->stop = true;
    st_pthread_mutex_lock(&app[i]->wake_mutex);
    st_pthread_cond_signal(&app[i]->wake_cond);
    st_pthread_mutex_unlock(&app[i]->wake_mutex);
    pthread_join(app[i]->app_thread, NULL);
  }

  // stop rx
  ret = st_stop(dev_handle);

error:
  // release session
  for (int i = 0; i < session_num; i++) {
    if (rx_handle[i]) st20r_rx_free(rx_handle[i]);
    if (!app[i]) continue;
    st_pthread_mutex_destroy(&app[i]->wake_mutex);
    st_pthread_cond_destroy(&app[i]->wake_cond);
    printf("session(%d) received frames %d\n", i, app[i]->fb_rec);
    if (app[i]->framebuffs) free(app[i]->framebuffs);
    free(app[i]);
  }

  // destroy device
  st_uninit(dev_handle);
  return 0;
}
