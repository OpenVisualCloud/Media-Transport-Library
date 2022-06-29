/*
 * Copyright (C) 2022 Intel Corporation.
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
#include <fcntl.h>
#include <pthread.h>
#include <st_pipeline_api.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../src/app_platform.h"

#define FWD_PORT_BDF "0000:af:00.1"
/* local ip address for current bdf port */
static uint8_t g_fwd_local_ip[ST_IP_ADDR_LEN] = {192, 168, 84, 2};

#define RX_ST20_UDP_PORT (20000)
#define RX_ST20_PAYLOAD_TYPE (112)
/* source ip address for rx video session, 239.168.84.1 */
static uint8_t g_rx_video_source_ip[ST_IP_ADDR_LEN] = {239, 168, 85, 20};

#define TX_ST20_UDP_PORT (20000)
#define TX_ST20_PAYLOAD_TYPE (112)
/* dst ip address for tx video session, 239.168.0.1 */
static uint8_t g_tx_st20_dst_ip[ST_IP_ADDR_LEN] = {239, 168, 85, 21};

#define ST20_TX_LOGO_FMT (ST_FRAME_FMT_YUV422RFC4175PG2BE10)
#define ST20_TX_LOGO_FILE ("logo_rfc4175.yuv")
#define ST20_TX_LOGO_WIDTH (200)
#define ST20_TX_LOGO_HEIGHT (200)

struct app_context {
  st_handle st;
  int idx;
  st20_rx_handle rx_handle;
  st20_tx_handle tx_handle;

  bool stop;
  bool ready;
  pthread_t fwd_thread;

  int fb_fwd;
  pthread_cond_t wake_cond;
  pthread_mutex_t wake_mutex;

  size_t framebuff_size;
  uint16_t framebuff_cnt;
  uint16_t framebuff_producer_idx;
  uint16_t framebuff_consumer_idx;
  struct st_rx_frame* framebuffs;

  uint16_t tx_framebuff_producer_idx;
  uint16_t tx_framebuff_consumer_idx;
  struct st_tx_frame* tx_framebuffs;

  /* logo */
  void* logo_buf;
  struct st_frame_meta logo_meta;
};

static int st20_fwd_open_logo(struct app_context* s, char* file) {
  FILE* fp_logo = st_fopen(file, "rb");
  if (!fp_logo) {
    printf("%s, open %s fail\n", __func__, file);
    return -EIO;
  }

  size_t logo_size =
      st_frame_size(ST20_TX_LOGO_FMT, ST20_TX_LOGO_WIDTH, ST20_TX_LOGO_HEIGHT);
  s->logo_buf = st_hp_malloc(s->st, logo_size, ST_PORT_P);
  if (!s->logo_buf) {
    printf("%s, logo buf malloc fail\n", __func__);
    fclose(fp_logo);
    return -EIO;
  }

  size_t read = fread(s->logo_buf, 1, logo_size, fp_logo);
  if (read != logo_size) {
    printf("%s, logo buf read fail\n", __func__);
    st_hp_free(s->st, s->logo_buf);
    s->logo_buf = NULL;
    fclose(fp_logo);
    return -EIO;
  }

  s->logo_meta.addr = s->logo_buf;
  s->logo_meta.fmt = ST20_TX_LOGO_FMT;
  s->logo_meta.width = ST20_TX_LOGO_WIDTH;
  s->logo_meta.height = ST20_TX_LOGO_HEIGHT;

  fclose(fp_logo);
  return 0;
}

static int rx_st20_enqueue_frame(struct app_context* s, void* frame, size_t size) {
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

static int rx_st20_frame_ready(void* priv, void* frame, struct st20_frame_meta* meta) {
  struct app_context* s = (struct app_context*)priv;

  if (!s->ready) return -EIO;

  /* incomplete frame */
  if (!st20_is_frame_complete(meta->status)) {
    st20_rx_put_framebuff(s->rx_handle, frame);
    return 0;
  }

  st_pthread_mutex_lock(&s->wake_mutex);
  int ret = rx_st20_enqueue_frame(s, frame, meta->frame_total_size);
  if (ret < 0) {
    printf("%s(%d), frame %p dropped\n", __func__, s->idx, frame);
    /* free the queue */
    st20_rx_put_framebuff(s->rx_handle, frame);
    st_pthread_mutex_unlock(&s->wake_mutex);
    return ret;
  }
  st_pthread_cond_signal(&s->wake_cond);
  st_pthread_mutex_unlock(&s->wake_mutex);

  return 0;
}

static int tx_video_next_frame(void* priv, uint16_t* next_frame_idx, bool* second_field) {
  struct app_context* s = priv;
  int ret;
  uint16_t consumer_idx = s->tx_framebuff_consumer_idx;
  struct st_tx_frame* framebuff = &s->tx_framebuffs[consumer_idx];

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
    s->tx_framebuff_consumer_idx = consumer_idx;
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
  struct st_tx_frame* framebuff = &s->tx_framebuffs[frame_idx];

  st_pthread_mutex_lock(&s->wake_mutex);
  if (ST_TX_FRAME_IN_TRANSMITTING == framebuff->stat) {
    ret = 0;
    framebuff->stat = ST_TX_FRAME_FREE;
    // printf("%s(%d), done_idx %u\n", __func__, s->idx, frame_idx);
  } else {
    ret = -EIO;
    printf("%s(%d), err status %d for frame %u\n", __func__, s->idx, framebuff->stat,
           frame_idx);
  }
  st_pthread_cond_signal(&s->wake_cond);
  st_pthread_mutex_unlock(&s->wake_mutex);

  return ret;
}

static void rx_fwd_consume_frame(struct app_context* s, void* frame, size_t frame_size) {
  uint16_t producer_idx;
  struct st_tx_frame* framebuff;
  struct st_frame_meta tx_frame;

  if (frame_size != s->framebuff_size) {
    printf("%s(%d), mismatch frame size %ld %ld\n", __func__, s->idx, frame_size,
           s->framebuff_size);
    return;
  }

  producer_idx = s->tx_framebuff_producer_idx;
  framebuff = &s->tx_framebuffs[producer_idx];
  if (ST_TX_FRAME_FREE != framebuff->stat) {
    /* not in free */
    printf("%s(%d), frame %u err state %d\n", __func__, s->idx, producer_idx,
           framebuff->stat);
    return;
  }

  void* frame_addr = st20_tx_get_framebuffer(s->tx_handle, producer_idx);
  st_memcpy(frame_addr, frame, s->framebuff_size);
  if (s->logo_buf) {
    tx_frame.addr = frame_addr;
    tx_frame.fmt = ST20_TX_LOGO_FMT;
    tx_frame.buffer_size = s->framebuff_size;
    tx_frame.data_size = s->framebuff_size;
    tx_frame.width = 1920;
    tx_frame.height = 1080;
    st_draw_logo(&tx_frame, &s->logo_meta, 16, 16);
  }
  framebuff->size = s->framebuff_size;
  framebuff->stat = ST_TX_FRAME_READY;
  /* point to next */
  producer_idx++;
  if (producer_idx >= s->framebuff_cnt) producer_idx = 0;
  s->tx_framebuff_producer_idx = producer_idx;

  s->fb_fwd++;
}

static void* fwd_thread(void* arg) {
  struct app_context* s = arg;
  st20_rx_handle rx_handle = s->rx_handle;
  struct st_rx_frame* rx_framebuff;
  int consumer_idx;

  printf("%s(%d), start\n", __func__, s->idx);
  while (!s->stop) {
    st_pthread_mutex_lock(&s->wake_mutex);
    consumer_idx = s->framebuff_consumer_idx;
    rx_framebuff = &s->framebuffs[consumer_idx];
    if (!rx_framebuff->frame) {
      /* no ready frame */
      if (!s->stop) st_pthread_cond_wait(&s->wake_cond, &s->wake_mutex);
      st_pthread_mutex_unlock(&s->wake_mutex);
      continue;
    }

    // printf("%s(%d), frame idx %d\n", __func__, idx, consumer_idx);
    rx_fwd_consume_frame(s, rx_framebuff->frame, rx_framebuff->size);
    st20_rx_put_framebuff(rx_handle, rx_framebuff->frame);
    /* point to next */
    rx_framebuff->frame = NULL;
    consumer_idx++;
    if (consumer_idx >= s->framebuff_cnt) consumer_idx = 0;
    s->framebuff_consumer_idx = consumer_idx;
    st_pthread_mutex_unlock(&s->wake_mutex);
  }
  printf("%s(%d), stop\n", __func__, s->idx);

  return NULL;
}

static int free_app(struct app_context* app) {
  if (app->tx_handle) {
    st20_tx_free(app->tx_handle);
    app->tx_handle = NULL;
  }
  if (app->rx_handle) {
    st20_rx_free(app->rx_handle);
    app->rx_handle = NULL;
  }
  if (app->logo_buf) {
    st_hp_free(app->st, app->logo_buf);
    app->logo_buf = NULL;
  }
  if (app->st) {
    st_uninit(app->st);
    app->st = NULL;
  }
  st_pthread_mutex_destroy(&app->wake_mutex);
  st_pthread_cond_destroy(&app->wake_cond);
  if (app->framebuffs) {
    free(app->framebuffs);
    app->framebuffs = NULL;
  }
  if (app->tx_framebuffs) {
    free(app->tx_framebuffs);
    app->tx_framebuffs = NULL;
  }

  return 0;
}

int main() {
  struct st_init_params param;
  int fb_cnt = 4;
  int ret = -EIO;
  struct app_context app;
  st_handle st;

  memset(&app, 0, sizeof(app));
  app.idx = 0;
  app.stop = false;
  st_pthread_mutex_init(&app.wake_mutex, NULL);
  st_pthread_cond_init(&app.wake_cond, NULL);

  app.framebuff_cnt = fb_cnt;
  app.framebuffs =
      (struct st_rx_frame*)malloc(sizeof(*app.framebuffs) * app.framebuff_cnt);
  if (!app.framebuffs) {
    printf("%s, framebuffs malloc fail\n", __func__);
    free_app(&app);
    return -EIO;
  }
  for (uint16_t j = 0; j < app.framebuff_cnt; j++) app.framebuffs[j].frame = NULL;
  app.framebuff_producer_idx = 0;
  app.framebuff_consumer_idx = 0;

  app.tx_framebuffs =
      (struct st_tx_frame*)malloc(sizeof(*app.tx_framebuffs) * app.framebuff_cnt);
  if (!app.tx_framebuffs) {
    printf("%s, tx framebuffs malloc fail\n", __func__);
    free_app(&app);
    return -EIO;
  }
  for (uint16_t j = 0; j < app.framebuff_cnt; j++) {
    app.tx_framebuffs[j].stat = ST_TX_FRAME_FREE;
  }

  memset(&param, 0, sizeof(param));
  param.num_ports = 1;
  strncpy(param.port[ST_PORT_P], FWD_PORT_BDF, ST_PORT_MAX_LEN);
  memcpy(param.sip_addr[ST_PORT_P], g_fwd_local_ip, ST_IP_ADDR_LEN);
  param.flags =
      ST_FLAG_BIND_NUMA | ST_FLAG_DEV_AUTO_START_STOP | ST_FLAG_RX_SEPARATE_VIDEO_LCORE;
  param.log_level = ST_LOG_LEVEL_INFO;  // log level. ERROR, INFO, WARNING
  param.priv = NULL;                    // usr ctx pointer
  param.ptp_get_time_fn = NULL;
  param.tx_sessions_cnt_max = 1;
  param.rx_sessions_cnt_max = 1;
  param.lcores = NULL;
  // create device
  st = st_init(&param);
  if (!st) {
    free_app(&app);
    printf("%s, st_init fail\n", __func__);
    return -EIO;
  }
  app.st = st;

  struct st20_rx_ops ops_rx;
  memset(&ops_rx, 0, sizeof(ops_rx));
  ops_rx.name = "st20_fwd";
  ops_rx.priv = &app;
  ops_rx.num_port = 1;
  memcpy(ops_rx.sip_addr[ST_PORT_P], g_rx_video_source_ip, ST_IP_ADDR_LEN);
  strncpy(ops_rx.port[ST_PORT_P], FWD_PORT_BDF, ST_PORT_MAX_LEN);
  ops_rx.udp_port[ST_PORT_P] = RX_ST20_UDP_PORT;  // user config the udp port.
  ops_rx.pacing = ST21_PACING_NARROW;
  ops_rx.type = ST20_TYPE_FRAME_LEVEL;
  ops_rx.width = 1920;
  ops_rx.height = 1080;
  ops_rx.fps = ST_FPS_P59_94;
  ops_rx.fmt = ST20_FMT_YUV_422_10BIT;
  ops_rx.framebuff_cnt = fb_cnt;
  ops_rx.payload_type = RX_ST20_PAYLOAD_TYPE;
  ops_rx.flags = 0;
  ops_rx.notify_frame_ready = rx_st20_frame_ready;
  st20_rx_handle rx_handle = st20_rx_create(st, &ops_rx);
  if (!rx_handle) {
    free_app(&app);
    printf("%s, st20_rx_create fail\n", __func__);
    return -EIO;
  }
  app.rx_handle = rx_handle;

  struct st20_tx_ops ops_tx;
  memset(&ops_tx, 0, sizeof(ops_tx));
  ops_tx.name = "st20_fwd";
  ops_tx.priv = &app;
  ops_tx.num_port = 1;
  memcpy(ops_tx.dip_addr[ST_PORT_P], g_tx_st20_dst_ip, ST_IP_ADDR_LEN);
  strncpy(ops_tx.port[ST_PORT_P], FWD_PORT_BDF, ST_PORT_MAX_LEN);
  ops_tx.udp_port[ST_PORT_P] = TX_ST20_UDP_PORT;
  ops_tx.pacing = ST21_PACING_NARROW;
  ops_tx.type = ST20_TYPE_FRAME_LEVEL;
  ops_tx.width = 1920;
  ops_tx.height = 1080;
  ops_tx.fps = ST_FPS_P59_94;
  ops_tx.fmt = ST20_FMT_YUV_422_10BIT;
  ops_tx.payload_type = TX_ST20_PAYLOAD_TYPE;
  ops_tx.framebuff_cnt = fb_cnt;
  ops_tx.get_next_frame = tx_video_next_frame;
  ops_tx.notify_frame_done = tx_video_frame_done;
  st20_tx_handle tx_handle = st20_tx_create(st, &ops_tx);
  if (!tx_handle) {
    free_app(&app);
    printf("%s, st20_tx_create fail\n", __func__);
    return -EIO;
  }
  app.tx_handle = tx_handle;
  app.framebuff_size = st20_tx_get_framebuffer_size(tx_handle);

  st20_fwd_open_logo(&app, ST20_TX_LOGO_FILE);

  ret = pthread_create(&app.fwd_thread, NULL, fwd_thread, &app);
  if (ret < 0) {
    printf("%s(%d), thread create fail\n", __func__, ret);
    free_app(&app);
    return -EIO;
  }

  app.ready = true;

  while (1) {
    sleep(1);
  }

  // stop app thread
  app.stop = true;
  st_pthread_mutex_lock(&app.wake_mutex);
  st_pthread_cond_signal(&app.wake_cond);
  st_pthread_mutex_unlock(&app.wake_mutex);
  pthread_join(app.fwd_thread, NULL);

  // release session
  printf("%s, fb_fwd %d\n", __func__, app.fb_fwd);
  free_app(&app);

  return 0;
}
