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

#include <pthread.h>
#include <st_dpdk_api.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define RX_VIDEO_PORT_BDF "0000:af:00.0"
#define RX_VIDEO_UDP_PORT (10000)

/* local ip address for current bdf port */
static uint8_t g_rx_video_local_ip[ST_IP_ADDR_LEN] = {192, 168, 0, 1};
/* source ip address for rx video session */
static uint8_t g_rx_video_source_ip[ST_IP_ADDR_LEN] = {239, 168, 0, 1};

struct app_context {
  int idx;
  int fb_rec;
  void* handle;
  bool stop;
  pthread_t app_thread;
  pthread_cond_t wake_cond;
  pthread_mutex_t wake_mutex;
};

static int rx_rtp_ready(void* priv) {
  struct app_context* s = (struct app_context*)priv;
  // wake up the app thread who is waiting for the rtp buf;
  pthread_mutex_lock(&s->wake_mutex);
  pthread_cond_signal(&s->wake_cond);
  pthread_mutex_unlock(&s->wake_mutex);
  return 0;
}

static void* app_rx_video_rtp_thread(void* arg) {
  struct app_context* s = arg;
  void* usrptr;
  uint16_t len;
  void* mbuf;
  struct st_rfc3550_rtp_hdr* hdr;

  while (!s->stop) {
    mbuf = st22_rx_get_mbuf(s->handle, &usrptr, &len);
    if (!mbuf) {
      /* no buffer */
      pthread_mutex_lock(&s->wake_mutex);
      if (!s->stop) pthread_cond_wait(&s->wake_cond, &s->wake_mutex);
      pthread_mutex_unlock(&s->wake_mutex);
      continue;
    }

    /* get one packet */
    hdr = (struct st_rfc3550_rtp_hdr*)usrptr;
    /* handle the rtp packet, should not handle the heavy work, if the st22_rx_get_mbuf is
     * not called timely, the rtp queue in the lib will be full and rtp will be enqueued
     * fail in the lib, packet will be dropped*/
    if (hdr->marker) s->fb_rec++;
    /* free to lib */
    st22_rx_put_mbuf(s->handle, mbuf);
  }

  return NULL;
}

int main() {
  struct st_init_params param;
  int session_num = 1;

  memset(&param, 0, sizeof(param));
  param.num_ports = 1;
  strncpy(param.port[ST_PORT_P], RX_VIDEO_PORT_BDF, ST_PORT_MAX_LEN);
  memcpy(param.sip_addr[ST_PORT_P], g_rx_video_local_ip, ST_IP_ADDR_LEN);
  param.flags = ST_FLAG_BIND_NUMA;      // default bind to numa
  param.log_level = ST_LOG_LEVEL_INFO;  // log level. ERROR, INFO, WARNING
  param.priv = NULL;                    // usr ctx pointer
  param.ptp_get_time_fn =
      NULL;  // user regist ptp func, if not regist, the internal pt p will be used
  param.tx_sessions_cnt_max = 0;
  param.rx_sessions_cnt_max = session_num;
  param.lcores = NULL;
  /* create device */
  st_handle dev_handle = st_init(&param);
  if (!dev_handle) {
    printf("st_init fail\n");
    return -1;
  }

  st22_rx_handle rx_handle[session_num];
  struct app_context* app[session_num];
  int ret;
  // create and register rx session
  for (int i = 0; i < session_num; i++) {
    app[i] = (struct app_context*)malloc(sizeof(struct app_context));
    if (!app[i]) {
      printf(" app struct is not correctly malloc");
      return -1;
    }
    memset(app[i], 0, sizeof(struct app_context));
    app[i]->idx = i;
    struct st22_rx_ops ops_rx;
    memset(&ops_rx, 0, sizeof(ops_rx));
    ops_rx.name = "st22_test";
    ops_rx.priv = app[i];  // app handle register to lib
    ops_rx.num_port = 1;
    memcpy(ops_rx.sip_addr[ST_PORT_P], g_rx_video_source_ip, ST_IP_ADDR_LEN);
    strncpy(ops_rx.port[ST_PORT_P], RX_VIDEO_PORT_BDF, ST_PORT_MAX_LEN);
    // user could config the udp port in this interface.
    ops_rx.udp_port[ST_PORT_P] = RX_VIDEO_UDP_PORT + i;
    ops_rx.width = 1920;
    ops_rx.height = 1080;
    ops_rx.fps = ST_FPS_P59_94;
    ops_rx.fmt = ST20_FMT_YUV_422_10BIT;
    ops_rx.rtp_ring_size = 1024;
    ops_rx.notify_rtp_ready = rx_rtp_ready;
    rx_handle[i] = st22_rx_create(dev_handle, &ops_rx);
    if (!rx_handle[i]) {
      printf(" rx_session is not correctly created");
      free(app[i]);
      return -1;
    }
    app[i]->handle = rx_handle[i];
    pthread_mutex_init(&app[i]->wake_mutex, NULL);
    pthread_cond_init(&app[i]->wake_cond, NULL);
    ret = pthread_create(&app[i]->app_thread, NULL, app_rx_video_rtp_thread, app[i]);
    if (ret < 0) {
      printf("%s(%d), app_thread create fail %d\n", __func__, ret, i);
      ret = st22_rx_free(rx_handle[i]);
      if (ret) {
        printf("session free failed\n");
      }
      free(app[i]);
      return -1;
    }
  }

  // start rx
  ret = st_start(dev_handle);

  // rx run 120s
  sleep(120);

  // stop app thread
  for (int i = 0; i < session_num; i++) {
    app[i]->stop = true;
    pthread_mutex_lock(&app[i]->wake_mutex);
    pthread_cond_signal(&app[i]->wake_cond);
    pthread_mutex_unlock(&app[i]->wake_mutex);
    pthread_join(app[i]->app_thread, NULL);
  }

  // stop rx
  ret = st_stop(dev_handle);

  // release session
  for (int i = 0; i < session_num; i++) {
    ret = st22_rx_free(rx_handle[i]);
    if (ret) {
      printf("session free failed\n");
    }
    pthread_mutex_destroy(&app[i]->wake_mutex);
    pthread_cond_destroy(&app[i]->wake_cond);
    printf("session(%d) received frames %d\n", i, app[i]->fb_rec);
    free(app[i]);
  }

  // destroy device
  st_uninit(dev_handle);
  return 0;
}
