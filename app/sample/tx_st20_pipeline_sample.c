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

#define TX_EXT_FRAME

#define TX_ST20_PORT_BDF "0000:af:00.1"
#define TX_ST20_UDP_PORT (20000)
#define TX_ST20_PAYLOAD_TYPE (112)

/* local ip address for current bdf port */
static uint8_t g_tx_st20_local_ip[ST_IP_ADDR_LEN] = {192, 168, 96, 1};
/* dst ip address for tx video session */
static uint8_t g_tx_st20_dst_ip[ST_IP_ADDR_LEN] = {239, 168, 85, 20};

//#define ST20_TX_SAMPLE_FMT_BGRA
//#define ST20_TX_SAMPLE_FMT_YUV422P10LE
#define ST20_TX_SAMPLE_FMT_YUV422RFC4175PG2BE
//#define ST20_TX_SAMPLE_FMT_YUV422PLANAR8
//#define ST20_TX_SAMPLE_FMT_YUV422PACKED8

#ifdef ST20_TX_SAMPLE_FMT_YUV422P10LE
#define ST20_TX_SAMPLE_FMT (ST_FRAME_FMT_YUV422PLANAR10LE)
#define ST20_TX_TRANSPORT_FMT (ST20_FMT_YUV_422_10BIT)
#define ST20_TX_SAMPLE_FILE ("test_le.yuv")
#endif

#ifdef ST20_TX_SAMPLE_FMT_YUV422RFC4175PG2BE
#define ST20_TX_SAMPLE_FMT (ST_FRAME_FMT_YUV422RFC4175PG2BE10)
#define ST20_TX_TRANSPORT_FMT (ST20_FMT_YUV_422_10BIT)
#define ST20_TX_SAMPLE_FILE ("test_rfc4175.yuv")
#endif

#ifdef ST20_TX_SAMPLE_FMT_YUV422PLANAR8
#define ST20_TX_SAMPLE_FMT (ST_FRAME_FMT_YUV422PLANAR8)
#define ST20_TX_TRANSPORT_FMT (ST20_FMT_YUV_422_10BIT)
#define ST20_TX_SAMPLE_FILE ("test_planar8.yuv")
#endif

#ifdef ST20_TX_SAMPLE_FMT_YUV422PACKED8
#define ST20_TX_SAMPLE_FMT (ST_FRAME_FMT_YUV422PACKED8)
#define ST20_TX_TRANSPORT_FMT (ST20_FMT_YUV_422_10BIT)
#define ST20_TX_SAMPLE_FILE ("test_packed8.yuv")
#endif

struct app_context {
  st_handle st;
  int idx;
  st20p_tx_handle handle;

  bool stop;
  pthread_t frame_thread;

  int fb_send;
  pthread_cond_t wake_cond;
  pthread_mutex_t wake_mutex;

  size_t frame_size;
  uint8_t* source_begin;
  st_iova_t source_begin_iova;
  uint8_t* source_end;
  uint8_t* frame_cursor;

  bool ext;
  st_dma_mem_handle dma_mem;
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

static int tx_st20p_close_source(struct app_context* s) {
  if (s->ext) {
    if (s->dma_mem) st_dma_mem_free(s->st, s->dma_mem);
  } else if (s->source_begin) {
    st_hp_free(s->st, s->source_begin);
    s->source_begin = NULL;
  }

  return 0;
}

static int tx_st20p_open_source(struct app_context* s, char* file) {
  int fd = -EIO;
  struct stat i;
  int frame_cnt = 2;
  uint8_t* m = NULL;
  size_t fbs_size = s->frame_size * frame_cnt;

  fd = st_open(file, O_RDONLY);
  if (fd < 0) {
    printf("%s, open %s fail\n", __func__, file);
    goto init_fb;
  }

  fstat(fd, &i);
  if (i.st_size < s->frame_size) {
    printf("%s, %s file size small then a frame %ld\n", __func__, file, s->frame_size);
    close(fd);
    return -EIO;
  }
  if (i.st_size % s->frame_size) {
    printf("%s, %s file size should be multiple of frame size %ld\n", __func__, file,
           s->frame_size);
    close(fd);
    return -EIO;
  }
  m = mmap(NULL, i.st_size, PROT_READ, MAP_SHARED, fd, 0);
  if (MAP_FAILED == m) {
    printf("%s, mmap %s fail\n", __func__, file);
    close(fd);
    return -EIO;
  }
  frame_cnt = i.st_size / s->frame_size;
  fbs_size = i.st_size;

init_fb:
  if (s->ext) { /* ext frame enabled */
    if (frame_cnt < 2) {
      /* notice that user should prepare more buffer than fb_cnt *frame_size */
      printf("%s, only 1 frame, will duplicate to 2\n", __func__);
      fbs_size *= 2;
    }

    /* alloc enough memory to hold framebuffers and map to iova */
    st_dma_mem_handle dma_mem = st_dma_mem_alloc(s->st, fbs_size);
    if (!dma_mem) {
      printf("%s(%d), dma mem alloc/map fail\n", __func__, s->idx);
      close(fd);
      return -EIO;
    }
    s->dma_mem = dma_mem;

    s->source_begin = st_dma_mem_addr(dma_mem);
    s->source_begin_iova = st_dma_mem_iova(dma_mem);
    s->frame_cursor = s->source_begin;
    if (m) {
      if (frame_cnt < 2) {
        st_memcpy(s->source_begin, m, s->frame_size);
        st_memcpy(s->source_begin + s->frame_size, m, s->frame_size);
      } else {
        st_memcpy(s->source_begin, m, i.st_size);
      }
    }
    s->source_end = s->source_begin + fbs_size;
    printf("%s, source begin at %p, end at %p\n", __func__, s->source_begin,
           s->source_end);
  } else {
    s->source_begin = st_hp_zmalloc(s->st, fbs_size, ST_PORT_P);
    if (!s->source_begin) {
      printf("%s, source malloc on hugepage fail\n", __func__);
      close(fd);
      return -EIO;
    }
    s->frame_cursor = s->source_begin;
    if (m) st_memcpy(s->source_begin, m, fbs_size);
    s->source_end = s->source_begin + fbs_size;
  }

  if (fd >= 0) close(fd);

  return 0;
}

static int tx_st20p_frame_available(void* priv) {
  struct app_context* s = priv;

  st_pthread_mutex_lock(&s->wake_mutex);
  st_pthread_cond_signal(&s->wake_cond);
  st_pthread_mutex_unlock(&s->wake_mutex);

  return 0;
}

static int tx_st20p_frame_done(void* priv, struct st_frame* frame) {
  struct app_context* s = priv;

  if (s->ext) {
    /* free or return the ext memory here if necessary */
    /* then clear the frame buffer */
  }

  return 0;
}

static void tx_st20p_build_frame(struct app_context* s, struct st_frame* frame) {
  uint8_t* src = s->frame_cursor;

  st_memcpy(frame->addr, src, s->frame_size);
}

static void* tx_st20p_frame_thread(void* arg) {
  struct app_context* s = arg;
  st20p_tx_handle handle = s->handle;
  struct st_frame* frame;

  printf("%s(%d), start\n", __func__, s->idx);
  while (!s->stop) {
    frame = st20p_tx_get_frame(handle);
    if (!frame) { /* no frame */
      st_pthread_mutex_lock(&s->wake_mutex);
      if (!s->stop) st_pthread_cond_wait(&s->wake_cond, &s->wake_mutex);
      st_pthread_mutex_unlock(&s->wake_mutex);
      continue;
    }
    if (s->ext) {
      struct st20_ext_frame ext_frame;
      ext_frame.buf_addr = s->frame_cursor;
      ext_frame.buf_iova = s->source_begin_iova + (s->frame_cursor - s->source_begin);
      ext_frame.buf_len = s->frame_size;
      st20p_tx_put_ext_frame(handle, frame, &ext_frame);
    } else {
      if (s->source_begin) tx_st20p_build_frame(s, frame);
      st20p_tx_put_frame(handle, frame);
    }
    /* point to next frame */
    s->frame_cursor += s->frame_size;
    if (s->frame_cursor + s->frame_size > s->source_end) {
      s->frame_cursor = s->source_begin;
    }
    s->fb_send++;
  }
  printf("%s(%d), stop\n", __func__, s->idx);

  return NULL;
}

int main() {
  struct st_init_params param;
  int session_num = 1;
  int fb_cnt = 2;
  int ret = -EIO;
  struct app_context* app[session_num];
  st_handle dev_handle;
  char* port = getenv("ST_PORT_P");
  if (!port) port = TX_ST20_PORT_BDF;

  for (int i = 0; i < session_num; i++) {
    app[i] = NULL;
  }

  memset(&param, 0, sizeof(param));
  param.num_ports = 1;
  strncpy(param.port[ST_PORT_P], port, ST_PORT_MAX_LEN);
  memcpy(param.sip_addr[ST_PORT_P], g_tx_st20_local_ip, ST_IP_ADDR_LEN);
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

  g_st_handle = dev_handle;
  g_video_active = true;
  signal(SIGINT, app_sig_handler);

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
    app[i]->ext = false;
    st_pthread_mutex_init(&app[i]->wake_mutex, NULL);
    st_pthread_cond_init(&app[i]->wake_cond, NULL);

    struct st20p_tx_ops ops_tx;
    memset(&ops_tx, 0, sizeof(ops_tx));
    ops_tx.name = "st20p_test";
    ops_tx.priv = app[i];  // app handle register to lib
    ops_tx.port.num_port = 1;
    // tx src ip like 239.0.0.1
    memcpy(ops_tx.port.dip_addr[ST_PORT_P], g_tx_st20_dst_ip, ST_IP_ADDR_LEN);
    // send port interface like 0000:af:00.0
    strncpy(ops_tx.port.port[ST_PORT_P], port, ST_PORT_MAX_LEN);
    ops_tx.port.udp_port[ST_PORT_P] = TX_ST20_UDP_PORT + i;
    ops_tx.port.payload_type = TX_ST20_PAYLOAD_TYPE;
    ops_tx.width = 1920;
    ops_tx.height = 1080;
    ops_tx.fps = ST_FPS_P59_94;
    ops_tx.input_fmt = ST20_TX_SAMPLE_FMT;
    ops_tx.transport_fmt = ST20_TX_TRANSPORT_FMT;
    ops_tx.device = ST_PLUGIN_DEVICE_AUTO;
    ops_tx.framebuff_cnt = fb_cnt;
    ops_tx.notify_frame_available = tx_st20p_frame_available;
    ops_tx.notify_frame_done = tx_st20p_frame_done;
#ifdef TX_EXT_FRAME
    ops_tx.flags |= ST20P_TX_FLAG_EXT_FRAME;
    app[i]->ext = true;
#endif

    st20p_tx_handle tx_handle = st20p_tx_create(dev_handle, &ops_tx);
    if (!tx_handle) {
      printf("%s, st20p_tx_create fail\n", __func__);
      ret = -EIO;
      goto err;
    }
    app[i]->handle = tx_handle;

    app[i]->frame_size = st20p_tx_frame_size(tx_handle);
    ret = tx_st20p_open_source(app[i], ST20_TX_SAMPLE_FILE);
    if (ret < 0) {
      printf("%s, open source fail %d\n", __func__, ret);
      goto err;
    }

    ret = pthread_create(&app[i]->frame_thread, NULL, tx_st20p_frame_thread, app[i]);
    if (ret < 0) {
      printf("%s(%d), thread create fail %d\n", __func__, ret, i);
      ret = -EIO;
      goto err;
    }
  }

  while (g_video_active) {
    sleep(1);
  }

  // stop app thread
  for (int i = 0; i < session_num; i++) {
    app[i]->stop = true;
    st_pthread_mutex_lock(&app[i]->wake_mutex);
    st_pthread_cond_signal(&app[i]->wake_cond);
    st_pthread_mutex_unlock(&app[i]->wake_mutex);
    pthread_join(app[i]->frame_thread, NULL);

    tx_st20p_close_source(app[i]);
  }

  // release session
  for (int i = 0; i < session_num; i++) {
    printf("%s, fb_send %d\n", __func__, app[i]->fb_send);
    ret = st20p_tx_free(app[i]->handle);
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
      if (app[i]->handle) st20p_tx_free(app[i]->handle);
      free(app[i]);
    }
  }
  if (dev_handle) st_uninit(dev_handle);
  return ret;
}
