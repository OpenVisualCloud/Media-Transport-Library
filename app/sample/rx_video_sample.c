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

struct app_context {
  int idx;
  int16_t ready_frame_idx;
  int16_t consumer_frame_idx;
  int fb_rec;
  void* frame_rec[3];
  void* handle;
  bool stop;
  pthread_t app_thread;
  pthread_cond_t wake_cond;
  pthread_mutex_t wake_mutex;
};

static int rx_frame_ready(void* priv, void* frame) {
  struct app_context* s = (struct app_context*)priv;
  pthread_mutex_lock(&s->wake_mutex);
  // restore the returned frame ptr, since rx_frame_ready callback should be non-blocked.
  // the frame should be handled in app thread
  s->frame_rec[s->ready_frame_idx] = frame;
  s->ready_frame_idx = (s->ready_frame_idx + 1) % 3;
  pthread_cond_signal(&s->wake_cond);
  pthread_mutex_unlock(&s->wake_mutex);
  s->fb_rec++;
  return 0;
}

static void* app_rx_video_frame_thread(void* arg) {
  struct app_context* s = arg;
  int consumer_idx;
  void* frame;

  while (!s->stop) {
    consumer_idx = s->consumer_frame_idx;
    consumer_idx++;
    if (consumer_idx >= 3) consumer_idx = 0;
    if (consumer_idx == s->ready_frame_idx) {
      /* no buffer */
      pthread_mutex_lock(&s->wake_mutex);
      if (!s->stop) pthread_cond_wait(&s->wake_cond, &s->wake_mutex);
      pthread_mutex_unlock(&s->wake_mutex);
      continue;
    }

    frame = s->frame_rec[consumer_idx];
    // put your handle of frame ptr here, it contains pixels data format in st2110-20
    // aligned with the TX transfer pg format
    st20_rx_put_framebuff(s->handle, frame);
    s->consumer_frame_idx = consumer_idx;
  }

  return NULL;
}

int main() {
  struct st_init_params param;
  memset(&param, 0, sizeof(param));
  int session_num = 1;
  param.num_ports = 1;
  strncpy(param.port[ST_PORT_P], "0000:af:00.0", ST_PORT_MAX_LEN);
  uint8_t ip[4] = {192, 168, 0, 1};
  memcpy(param.sip_addr[ST_PORT_P], ip, ST_IP_ADDR_LEN);
  param.flags = ST_FLAG_BIND_NUMA;      // default bind to numa
  param.log_level = ST_LOG_LEVEL_INFO;  // log level. ERROR, INFO, WARNING
  param.priv = NULL;                    // usr ctx pointer
  param.ptp_get_time_fn =
      NULL;  // user regist ptp func, if not regist, the internal pt p will be used
  param.tx_sessions_cnt_max = 0;
  param.rx_sessions_cnt_max = session_num;
  param.lcores = NULL;
  st_handle dev_handle = st_init(&param);
  if (!dev_handle) {
    printf("st_init fail\n");
    return -1;
  }
  st20_rx_handle rx_handle[session_num];
  struct app_context* app[session_num];
  int ret;

  // create and register rx session
  for (int i = 0; i < session_num; i++) {
    app[i] = (struct app_context*)malloc(sizeof(struct app_context));
    if (!app[i]) {
      printf(" app struct is not correctly malloc");
      return -1;
    }
    app[i]->idx = i;
    struct st20_rx_ops ops_rx;
    ops_rx.name = "st20_test";
    ops_rx.priv = app[i];  // app handle register to lib
    ops_rx.num_port = 1;
    uint8_t sip[4] = {239, 168, 0, 1};
    memcpy(ops_rx.sip_addr[ST_PORT_P], sip, ST_IP_ADDR_LEN);  // tx src ip like 239.0.0.1
    strncpy(ops_rx.port[ST_PORT_P], "0000:af:00.0",
            ST_PORT_MAX_LEN);  // send port interface like 0000:af:00.0
    ops_rx.udp_port[ST_PORT_P] =
        10000 + i;  // user could config the udp port in this interface.
    ops_rx.pacing = ST21_PACING_NARROW;
    ops_rx.type = ST20_TYPE_FRAME_LEVEL;
    ops_rx.width = 1920;
    ops_rx.height = 1080;
    ops_rx.fps = ST_FPS_P59_94;
    ops_rx.fmt = ST20_FMT_YUV_422_10BIT;
    ops_rx.framebuff_cnt =
        3;  // aligned with frame_rec[3] in app_context, framebuf pool size is 3 in lib.
    ops_rx.notify_frame_ready =
        rx_frame_ready;  // app regist non-block func, app could get a frame ready
                         // notification info by this callback
    rx_handle[i] = st20_rx_create(dev_handle, &ops_rx);
    if (!rx_handle[i]) {
      printf(" rx_session is not correctly created");
      free(app[i]);
      return -1;
    }
    app[i]->handle = rx_handle[i];
    app[i]->consumer_frame_idx = -1;
    app[i]->ready_frame_idx = 0;
    app[i]->stop = false;
    pthread_mutex_init(&app[i]->wake_mutex, NULL);
    pthread_cond_init(&app[i]->wake_cond, NULL);
    ret = pthread_create(&app[i]->app_thread, NULL, app_rx_video_frame_thread, app[i]);
    if (ret < 0) {
      printf("%s(%d), app_thread create fail %d\n", __func__, ret, i);
      ret = st20_rx_free(rx_handle[i]);
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
    pthread_join(app[i]->app_thread, NULL);
  }

  // stop rx
  ret = st_stop(dev_handle);

  // release session
  for (int i = 0; i < session_num; i++) {
    ret = st20_rx_free(rx_handle[i]);
    if (ret) {
      printf("session free failed\n");
    }
    pthread_mutex_destroy(&app[i]->wake_mutex);
    pthread_cond_destroy(&app[i]->wake_cond);
    free(app[i]);
  }

  // destroy device
  st_uninit(dev_handle);
  return 0;
}
