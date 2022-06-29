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

#define TX_ST22_PORT_BDF "0000:af:00.1"
#define TX_ST22_UDP_PORT (50000)
#define TX_ST22_PAYLOAD_TYPE (114)

/* local ip address for current bdf port */
static uint8_t g_tx_st22_local_ip[ST_IP_ADDR_LEN] = {192, 168, 1, 22};
/* dst ip address for tx video session */
static uint8_t g_tx_st22_dst_ip[ST_IP_ADDR_LEN] = {239, 168, 85, 22};

//#define ST22_TX_SAMPLE_FMT_BGRA
//#define ST22_TX_SAMPLE_FMT_YUV422P10LE
#define ST22_TX_SAMPLE_FMT_YUV422RFC4175PG2BE
//#define ST22_TX_SAMPLE_FMT_YUV422PLANAR8
//#define ST22_TX_SAMPLE_FMT_YUV422PACKED8

#ifdef ST22_TX_SAMPLE_FMT_BGRA
#define ST22_TX_SAMPLE_FMT (ST_FRAME_FMT_BGRA)
#define ST22_TX_SAMPLE_FILE ("test.bgra")
#endif

#ifdef ST22_TX_SAMPLE_FMT_YUV422P10LE
#define ST22_TX_SAMPLE_FMT (ST_FRAME_FMT_YUV422PLANAR10LE)
#define ST22_TX_SAMPLE_FILE ("test_le.yuv")
#endif

#ifdef ST22_TX_SAMPLE_FMT_YUV422RFC4175PG2BE
#define ST22_TX_SAMPLE_FMT (ST_FRAME_FMT_YUV422RFC4175PG2BE10)
#define ST22_TX_SAMPLE_FILE ("test_rfc4175.yuv")
#define ST22_TX_LOGO_FILE ("logo_rfc4175.yuv")
#define ST22_TX_LOGO_WIDTH (200)
#define ST22_TX_LOGO_HEIGHT (200)
#endif

#ifdef ST22_TX_SAMPLE_FMT_YUV422PLANAR8
#define ST22_TX_SAMPLE_FMT (ST_FRAME_FMT_YUV422PLANAR8)
#define ST22_TX_SAMPLE_FILE ("test_planar8.yuv")
#endif

#ifdef ST22_TX_SAMPLE_FMT_YUV422PACKED8
#define ST22_TX_SAMPLE_FMT (ST_FRAME_FMT_YUV422PACKED8)
#define ST22_TX_SAMPLE_FILE ("test_packed8.yuv")
#endif

struct app_context {
  st_handle st;
  int idx;
  st22p_tx_handle handle;

  bool stop;
  pthread_t frame_thread;

  int fb_send;
  pthread_cond_t wake_cond;
  pthread_mutex_t wake_mutex;

  size_t frame_size;
  uint8_t* source_begin;
  uint8_t* source_end;
  uint8_t* frame_cursor;

  /* logo */
  void* logo_buf;
  struct st_frame_meta logo_meta;
};

static int tx_st22p_close_source(struct app_context* s) {
  if (s->source_begin) {
    st_hp_free(s->st, s->source_begin);
    s->source_begin = NULL;
  }

  if (s->logo_buf) {
    st_hp_free(s->st, s->logo_buf);
    s->logo_buf = NULL;
  }

  return 0;
}

static int tx_st22p_open_logo(struct app_context* s, char* file) {
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

static int tx_st22p_open_source(struct app_context* s, char* file) {
  int fd;
  struct stat i;

  fd = st_open(file, O_RDONLY);
  if (fd < 0) {
    printf("%s, open %s fail\n", __func__, file);
    return -EIO;
  }

  fstat(fd, &i);
  if (i.st_size < s->frame_size) {
    printf("%s, %s file size small then a frame %ld\n", __func__, file, s->frame_size);
    close(fd);
    return -EIO;
  }

  uint8_t* m = mmap(NULL, i.st_size, PROT_READ, MAP_SHARED, fd, 0);
  if (MAP_FAILED == m) {
    printf("%s, mmap %s fail\n", __func__, file);
    close(fd);
    return -EIO;
  }

  s->source_begin = st_hp_malloc(s->st, i.st_size, ST_PORT_P);
  if (!s->source_begin) {
    printf("%s, source malloc on hugepage fail\n", __func__);
    close(fd);
    return -EIO;
  }

  s->frame_cursor = s->source_begin;
  st_memcpy(s->source_begin, m, i.st_size);
  s->source_end = s->source_begin + i.st_size;
  close(fd);

  tx_st22p_open_logo(s, ST22_TX_LOGO_FILE);

  return 0;
}

static int tx_st22p_frame_available(void* priv) {
  struct app_context* s = priv;

  st_pthread_mutex_lock(&s->wake_mutex);
  st_pthread_cond_signal(&s->wake_cond);
  st_pthread_mutex_unlock(&s->wake_mutex);

  return 0;
}

static void tx_st22p_build_frame(struct app_context* s, struct st_frame_meta* frame) {
  if (s->frame_cursor + s->frame_size > s->source_end) {
    s->frame_cursor = s->source_begin;
  }
  uint8_t* src = s->frame_cursor;

  st_memcpy(frame->addr, src, s->frame_size);
  if (s->logo_buf) {
    st_draw_logo(frame, &s->logo_meta, 16, 16);
  }

  /* point to next frame */
  s->frame_cursor += s->frame_size;

  s->fb_send++;
}

static void* tx_st22p_frame_thread(void* arg) {
  struct app_context* s = arg;
  st22p_tx_handle handle = s->handle;
  struct st_frame_meta* frame;

  printf("%s(%d), start\n", __func__, s->idx);
  while (!s->stop) {
    frame = st22p_tx_get_frame(handle);
    if (!frame) { /* no frame */
      st_pthread_mutex_lock(&s->wake_mutex);
      if (!s->stop) st_pthread_cond_wait(&s->wake_cond, &s->wake_mutex);
      st_pthread_mutex_unlock(&s->wake_mutex);
      continue;
    }
    tx_st22p_build_frame(s, frame);
    st22p_tx_put_frame(handle, frame);
  }
  printf("%s(%d), stop\n", __func__, s->idx);

  return NULL;
}

int main() {
  struct st_init_params param;
  int session_num = 1;
  int fb_cnt = 4;
  int bpp = 3;
  int ret = -EIO;
  struct app_context* app[session_num];
  st_handle dev_handle;

  for (int i = 0; i < session_num; i++) {
    app[i] = NULL;
  }

  memset(&param, 0, sizeof(param));
  param.num_ports = 1;
  strncpy(param.port[ST_PORT_P], TX_ST22_PORT_BDF, ST_PORT_MAX_LEN);
  memcpy(param.sip_addr[ST_PORT_P], g_tx_st22_local_ip, ST_IP_ADDR_LEN);
  param.flags = ST_FLAG_BIND_NUMA | ST_FLAG_DEV_AUTO_START_STOP;
  param.log_level = ST_LOG_LEVEL_INFO;  // log level. ERROR, INFO, WARNING
  param.priv = NULL;                    // usr ctx pointer
  param.ptp_get_time_fn = NULL;
  param.tx_sessions_cnt_max = session_num;
  param.rx_sessions_cnt_max = 0;
  param.lcores = NULL;
  param.nb_tx_desc = 128;
  // create device
  dev_handle = st_init(&param);
  if (!dev_handle) {
    printf("%s, st_init fail\n", __func__);
    ret = -EIO;
    goto err;
  }

  // create and register tx session
  for (int i = 0; i < session_num; i++) {
    app[i] = malloc(sizeof(struct app_context));
    if (!app[i]) {
      printf("%s, app struct malloc fail\n", __func__);
      ret = -ENOMEM;
      goto err;
    }
    memset(app[i], 0, sizeof(struct app_context));
    app[i]->st = dev_handle;
    app[i]->idx = i;
    app[i]->stop = false;
    st_pthread_mutex_init(&app[i]->wake_mutex, NULL);
    st_pthread_cond_init(&app[i]->wake_cond, NULL);

    struct st22p_tx_ops ops_tx;
    memset(&ops_tx, 0, sizeof(ops_tx));
    ops_tx.name = "st22p_test";
    ops_tx.priv = app[i];  // app handle register to lib
    ops_tx.port.num_port = 1;
    // tx src ip like 239.0.0.1
    memcpy(ops_tx.port.dip_addr[ST_PORT_P], g_tx_st22_dst_ip, ST_IP_ADDR_LEN);
    // send port interface like 0000:af:00.0
    strncpy(ops_tx.port.port[ST_PORT_P], TX_ST22_PORT_BDF, ST_PORT_MAX_LEN);
    ops_tx.port.udp_port[ST_PORT_P] = TX_ST22_UDP_PORT + i;
    ops_tx.port.payload_type = TX_ST22_PAYLOAD_TYPE;
    ops_tx.width = 1920;
    ops_tx.height = 1080;
    ops_tx.fps = ST_FPS_P59_94;
    ops_tx.input_fmt = ST22_TX_SAMPLE_FMT;
    ops_tx.pack_type = ST22_PACK_CODESTREAM;
    ops_tx.codec = ST22_CODEC_JPEGXS;
    ops_tx.device = ST_PLUGIN_DEVICE_AUTO;
    ops_tx.quality = ST22_QUALITY_MODE_QUALITY;
    ops_tx.codec_thread_cnt = 2;
    ops_tx.codestream_size = ops_tx.width * ops_tx.height * bpp / 8;
    ops_tx.framebuff_cnt = fb_cnt;
    ops_tx.notify_frame_available = tx_st22p_frame_available;

    st22p_tx_handle tx_handle = st22p_tx_create(dev_handle, &ops_tx);
    if (!tx_handle) {
      printf("%s, st22p_tx_createcreate fail\n", __func__);
      ret = -EIO;
      goto err;
    }
    app[i]->handle = tx_handle;

    app[i]->frame_size = st22p_tx_frame_size(tx_handle);
    ret = tx_st22p_open_source(app[i], ST22_TX_SAMPLE_FILE);
    if (ret < 0) {
      goto err;
    }

    ret = pthread_create(&app[i]->frame_thread, NULL, tx_st22p_frame_thread, app[i]);
    if (ret < 0) {
      printf("%s(%d), thread create fail %d\n", __func__, ret, i);
      ret = -EIO;
      goto err;
    }
  }

  st_pause();

  // stop app thread
  for (int i = 0; i < session_num; i++) {
    app[i]->stop = true;
    st_pthread_mutex_lock(&app[i]->wake_mutex);
    st_pthread_cond_signal(&app[i]->wake_cond);
    st_pthread_mutex_unlock(&app[i]->wake_mutex);
    pthread_join(app[i]->frame_thread, NULL);

    tx_st22p_close_source(app[i]);
  }

  // release session
  for (int i = 0; i < session_num; i++) {
    printf("%s, fb_send %d\n", __func__, app[i]->fb_send);
    ret = st22p_tx_free(app[i]->handle);
    if (ret < 0) {
      printf("%s, session free failed\n", __func__);
    }
    st_pthread_mutex_destroy(&app[i]->wake_mutex);
    st_pthread_cond_destroy(&app[i]->wake_cond);

    free(app[i]);
  }

  // destroy device
  st_uninit(dev_handle);
  return 0;

err:
  for (int i = 0; i < session_num; i++) {
    if (app[i]) {
      if (app[i]->handle) st22p_tx_free(app[i]->handle);
      free(app[i]);
    }
  }
  if (dev_handle) st_uninit(dev_handle);
  return ret;
}
