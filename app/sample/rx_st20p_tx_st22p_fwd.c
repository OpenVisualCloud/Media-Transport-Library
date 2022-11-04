/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
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

#define FWD_PORT_BDF "0000:af:00.0"
/* local ip address for current bdf port */
static uint8_t g_fwd_local_ip[ST_IP_ADDR_LEN] = {192, 168, 84, 2};

#define RX_ST20_UDP_PORT (20000)
#define RX_ST20_PAYLOAD_TYPE (112)
/* source ip address for rx video session, 239.168.84.1 */
static uint8_t g_rx_video_source_ip[ST_IP_ADDR_LEN] = {239, 168, 85, 20};

#define TX_ST22_UDP_PORT (50000)
#define TX_ST22_PAYLOAD_TYPE (114)
/* dst ip address for tx video session, 239.168.0.1 */
static uint8_t g_tx_st22_dst_ip[ST_IP_ADDR_LEN] = {239, 168, 85, 22};

#define ST22_TX_SAMPLE_FMT (ST_FRAME_FMT_YUV422RFC4175PG2BE10)
#define ST22_TX_LOGO_FILE ("logo_rfc4175.yuv")
#define ST22_TX_LOGO_WIDTH (200)
#define ST22_TX_LOGO_HEIGHT (200)

struct app_context {
  st_handle st;
  int idx;
  st20p_rx_handle rx_handle;
  st22p_tx_handle tx_handle;

  bool stop;
  bool ready;
  pthread_t fwd_thread;

  int fb_fwd;
  pthread_cond_t wake_cond;
  pthread_mutex_t wake_mutex;

  size_t framebuff_size;

  /* logo */
  void* logo_buf;
  struct st_frame logo_meta;
};

static bool g_video_active = false;
static st_handle g_st_handle;

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

static int st22_fwd_open_logo(struct app_context* s, char* file) {
  FILE* fp_logo = st_fopen(file, "rb");
  if (!fp_logo) {
    printf("%s, open %s fail\n", __func__, file);
    return -EIO;
  }

  size_t logo_size =
      st_frame_size(ST22_TX_SAMPLE_FMT, ST22_TX_LOGO_WIDTH, ST22_TX_LOGO_HEIGHT);
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
  s->logo_meta.fmt = ST22_TX_SAMPLE_FMT;
  s->logo_meta.width = ST22_TX_LOGO_WIDTH;
  s->logo_meta.height = ST22_TX_LOGO_HEIGHT;

  fclose(fp_logo);
  return 0;
}

static int tx_st22p_frame_available(void* priv) {
  struct app_context* s = priv;

  if (!s->ready) return -EIO;

  st_pthread_mutex_lock(&s->wake_mutex);
  st_pthread_cond_signal(&s->wake_cond);
  st_pthread_mutex_unlock(&s->wake_mutex);

  return 0;
}

static int rx_st20p_frame_available(void* priv) {
  struct app_context* s = (struct app_context*)priv;

  if (!s->ready) return -EIO;

  st_pthread_mutex_lock(&s->wake_mutex);
  st_pthread_cond_signal(&s->wake_cond);
  st_pthread_mutex_unlock(&s->wake_mutex);

  return 0;
}

static void fwd_st22_consume_frame(struct app_context* s, struct st_frame* frame) {
  st22p_tx_handle tx_handle = s->tx_handle;
  struct st_frame* tx_frame;

  if (frame->data_size != s->framebuff_size) {
    printf("%s(%d), mismatch frame size %ld %ld\n", __func__, s->idx, frame->data_size,
           s->framebuff_size);
    return;
  }

  while (!s->stop) {
    tx_frame = st22p_tx_get_frame(tx_handle);
    if (!tx_frame) { /* no frame */
      st_pthread_mutex_lock(&s->wake_mutex);
      if (!s->stop) st_pthread_cond_wait(&s->wake_cond, &s->wake_mutex);
      st_pthread_mutex_unlock(&s->wake_mutex);
      continue;
    }
    st_memcpy(tx_frame->addr, frame->addr, s->framebuff_size);
    if (s->logo_buf) {
      st_draw_logo(tx_frame, &s->logo_meta, 16, 16);
    }
    st22p_tx_put_frame(tx_handle, tx_frame);
    s->fb_fwd++;
    return;
  }
}

static void* st20_fwd_st22_thread(void* arg) {
  struct app_context* s = arg;
  st20p_rx_handle rx_handle = s->rx_handle;
  struct st_frame* frame;

  printf("%s(%d), start\n", __func__, s->idx);
  while (!s->stop) {
    frame = st20p_rx_get_frame(rx_handle);
    if (!frame) { /* no frame */
      st_pthread_mutex_lock(&s->wake_mutex);
      if (!s->stop) st_pthread_cond_wait(&s->wake_cond, &s->wake_mutex);
      st_pthread_mutex_unlock(&s->wake_mutex);
      continue;
    }

    fwd_st22_consume_frame(s, frame);
    st20p_rx_put_frame(rx_handle, frame);
  }
  printf("%s(%d), stop\n", __func__, s->idx);

  return NULL;
}

static int free_app(struct app_context* app) {
  if (app->tx_handle) {
    st22p_tx_free(app->tx_handle);
    app->tx_handle = NULL;
  }
  if (app->rx_handle) {
    st20p_rx_free(app->rx_handle);
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

  return 0;
}

int main() {
  struct st_init_params param;
  int fb_cnt = 4;
  int bpp = 3;
  int ret = -EIO;
  struct app_context app;
  st_handle st;
  char* port = getenv("ST_PORT_P");
  if (!port) port = FWD_PORT_BDF;

  memset(&app, 0, sizeof(app));
  app.idx = 0;
  app.stop = false;
  st_pthread_mutex_init(&app.wake_mutex, NULL);
  st_pthread_cond_init(&app.wake_cond, NULL);

  memset(&param, 0, sizeof(param));
  param.num_ports = 1;
  strncpy(param.port[ST_PORT_P], port, ST_PORT_MAX_LEN);
  memcpy(param.sip_addr[ST_PORT_P], g_fwd_local_ip, ST_IP_ADDR_LEN);
  param.flags = ST_FLAG_BIND_NUMA | ST_FLAG_DEV_AUTO_START_STOP;
  param.log_level = ST_LOG_LEVEL_NOTICE;  // log level. ERROR, INFO, WARNING
  param.priv = NULL;                      // usr ctx pointer
  param.ptp_get_time_fn = NULL;
  param.tx_sessions_cnt_max = 1;
  param.rx_sessions_cnt_max = 1;
  param.lcores = NULL;
  param.nb_tx_desc = 128;
  // create device
  st = st_init(&param);
  if (!st) {
    printf("%s, st_init fail\n", __func__);
    free_app(&app);
    return -EIO;
  }
  app.st = st;

  g_st_handle = st;
  signal(SIGINT, app_sig_handler);

  struct st20p_rx_ops ops_rx;
  memset(&ops_rx, 0, sizeof(ops_rx));
  ops_rx.name = "st20p_test";
  ops_rx.priv = &app;  // app handle register to lib
  ops_rx.port.num_port = 1;
  // rx src ip like 239.0.0.1
  memcpy(ops_rx.port.sip_addr[ST_PORT_P], g_rx_video_source_ip, ST_IP_ADDR_LEN);
  // send port interface like 0000:af:00.0
  strncpy(ops_rx.port.port[ST_PORT_P], port, ST_PORT_MAX_LEN);
  ops_rx.port.udp_port[ST_PORT_P] = RX_ST20_UDP_PORT;
  ops_rx.port.payload_type = RX_ST20_PAYLOAD_TYPE;
  ops_rx.width = 1920;
  ops_rx.height = 1080;
  ops_rx.fps = ST_FPS_P59_94;
  ops_rx.transport_fmt = ST20_FMT_YUV_422_10BIT;
  ops_rx.output_fmt = ST_FRAME_FMT_YUV422RFC4175PG2BE10;
  ops_rx.device = ST_PLUGIN_DEVICE_AUTO;
  ops_rx.framebuff_cnt = fb_cnt;
  ops_rx.notify_frame_available = rx_st20p_frame_available;

  st20p_rx_handle rx_handle = st20p_rx_create(st, &ops_rx);
  if (!rx_handle) {
    printf("%s, st20p_rx_create fail\n", __func__);
    free_app(&app);
    return -EIO;
  }
  app.rx_handle = rx_handle;

  struct st22p_tx_ops ops_tx;
  memset(&ops_tx, 0, sizeof(ops_tx));
  ops_tx.name = "st22_fwd";
  ops_tx.priv = &app;  // app handle register to lib
  ops_tx.port.num_port = 1;
  memcpy(ops_tx.port.dip_addr[ST_PORT_P], g_tx_st22_dst_ip, ST_IP_ADDR_LEN);
  strncpy(ops_tx.port.port[ST_PORT_P], port, ST_PORT_MAX_LEN);
  ops_tx.port.udp_port[ST_PORT_P] = TX_ST22_UDP_PORT;
  ops_tx.port.payload_type = TX_ST22_PAYLOAD_TYPE;
  ops_tx.width = 1920;
  ops_tx.height = 1080;
  ops_tx.fps = ST_FPS_P59_94;
  ops_tx.input_fmt = ST_FRAME_FMT_YUV422RFC4175PG2BE10;
  ops_tx.pack_type = ST22_PACK_CODESTREAM;
  ops_tx.codec = ST22_CODEC_JPEGXS;
  ops_tx.device = ST_PLUGIN_DEVICE_AUTO;
  ops_tx.quality = ST22_QUALITY_MODE_QUALITY;
  ops_tx.codec_thread_cnt = 2;
  ops_tx.codestream_size = ops_tx.width * ops_tx.height * bpp / 8;
  ops_tx.framebuff_cnt = fb_cnt;
  ops_tx.notify_frame_available = tx_st22p_frame_available;
  st22p_tx_handle tx_handle = st22p_tx_create(st, &ops_tx);
  if (!tx_handle) {
    printf("%s, st22p_tx_create fail\n", __func__);
    free_app(&app);
    return -EIO;
  }
  app.tx_handle = tx_handle;
  app.framebuff_size = st22p_tx_frame_size(tx_handle);

  st22_fwd_open_logo(&app, ST22_TX_LOGO_FILE);

  ret = pthread_create(&app.fwd_thread, NULL, st20_fwd_st22_thread, &app);
  if (ret < 0) {
    printf("%s(%d), thread create fail\n", __func__, ret);
    free_app(&app);
    return -EIO;
  }

  app.ready = true;

  g_video_active = true;
  while (g_video_active) {
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
