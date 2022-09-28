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

#define RX_ST20_PORT_BDF "0000:af:00.0"
#define RX_ST20_UDP_PORT (20000)
#define RX_ST20_PAYLOAD_TYPE (112)

/* local ip address for current bdf port */
static uint8_t g_rx_st20_local_ip[ST_IP_ADDR_LEN] = {192, 168, 96, 2};
/* dst ip address for rx video session */
static uint8_t g_rx_st20_src_ip[ST_IP_ADDR_LEN] = {239, 168, 85, 20};

//#define ST20_RX_SAMPLE_FMT_YUV422P10LE
#define ST20_RX_SAMPLE_FMT_YUV422RFC4175PG2BE
//#define ST20_RX_SAMPLE_FMT_YUV422PACKED8
//#define ST20_RX_SAMPLE_FMT_INVALID

#ifdef ST20_RX_SAMPLE_FMT_YUV422P10LE
#define ST20_RX_SAMPLE_TRAN_FMT (ST20_FMT_YUV_422_10BIT)
#define ST20_RX_SAMPLE_OUT_FMT (ST_FRAME_FMT_YUV422PLANAR10LE)
#define ST20_RX_SAMPLE_FILE ("out_le.yuv")
#endif

#ifdef ST20_RX_SAMPLE_FMT_YUV422RFC4175PG2BE
#define ST20_RX_SAMPLE_TRAN_FMT (ST20_FMT_YUV_422_10BIT)
#define ST20_RX_SAMPLE_OUT_FMT (ST_FRAME_FMT_YUV422RFC4175PG2BE10)
#define ST20_RX_SAMPLE_FILE ("out_rfc4175.yuv")
#endif

#ifdef ST20_RX_SAMPLE_FMT_YUV422PACKED8
#define ST20_RX_SAMPLE_TRAN_FMT (ST20_FMT_YUV_422_10BIT)
#define ST20_RX_SAMPLE_OUT_FMT (ST_FRAME_FMT_YUV422PACKED8)
#define ST20_RX_SAMPLE_FILE ("out_packed8.yuv")
#endif

#ifdef ST20_RX_SAMPLE_FMT_INVALID
#define ST20_RX_SAMPLE_TRAN_FMT (ST20_FMT_YUV_422_8BIT)
#define ST20_RX_SAMPLE_OUT_FMT (ST_FRAME_FMT_YUV422RFC4175PG2BE10)
#define ST20_RX_SAMPLE_FILE ("out_rfc4175.yuv")
#endif

struct app_context {
  int idx;
  st20p_rx_handle handle;

  bool stop;
  pthread_t frame_thread;

  int fb_recv;
  pthread_cond_t wake_cond;
  pthread_mutex_t wake_mutex;

  size_t frame_size;
  int dst_fd;
  uint8_t* dst_begin;
  uint8_t* dst_end;
  uint8_t* dst_cursor;

  st_dma_mem_handle dma_mem;
  struct st20_ext_frame* ext_frames;
  int ext_idx;
  int fb_cnt;
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

static int rx_st20p_frame_available(void* priv) {
  struct app_context* s = priv;

  st_pthread_mutex_lock(&s->wake_mutex);
  st_pthread_cond_signal(&s->wake_cond);
  st_pthread_mutex_unlock(&s->wake_mutex);

  return 0;
}

static int rx_st20p_query_ext_frame(void* priv, struct st20_ext_frame* ext_frame) {
  struct app_context* s = priv;
  int i = s->ext_idx;

  ext_frame->buf_addr = s->ext_frames[i].buf_addr;
  ext_frame->buf_iova = s->ext_frames[i].buf_iova;
  ext_frame->buf_len = s->ext_frames[i].buf_len;

  /* save your private data here get it from st_frame.opaque */
  /* ext_frame->opaque = ?; */

  if (++s->ext_idx >= s->fb_cnt) s->ext_idx = 0;

  return 0;
}

static int rx_st20p_close_source(struct app_context* s) {
  if (s->dst_fd >= 0) {
    close(s->dst_fd);
    s->dst_fd = 0;
  }

  return 0;
}

static int rx_st20p_open_source(struct app_context* s, const char* file) {
  int fd, ret, idx = s->idx;
  off_t f_size;
  int fb_cnt = 3;

  fd = st_open_mode(file, O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
  if (fd < 0) {
    printf("%s(%d), open %s fail\n", __func__, idx, file);
    return -EIO;
  }

  f_size = fb_cnt * s->frame_size;
  ret = ftruncate(fd, f_size);
  if (ret < 0) {
    printf("%s(%d), ftruncate %s fail\n", __func__, idx, file);
    close(fd);
    return -EIO;
  }

  uint8_t* m = mmap(NULL, f_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (MAP_FAILED == m) {
    printf("%s(%d), mmap %s fail\n", __func__, idx, file);
    close(fd);
    return -EIO;
  }

  s->dst_begin = m;
  s->dst_cursor = m;
  s->dst_end = m + f_size;
  s->dst_fd = fd;
  printf("%s(%d), save %d framebuffers to file %s(%p,%ld)\n", __func__, idx, fb_cnt, file,
         m, f_size);

  return 0;
}

static void rx_st20p_consume_frame(struct app_context* s, struct st_frame* frame) {
  if (s->dst_cursor + s->frame_size > s->dst_end) s->dst_cursor = s->dst_begin;
  st_memcpy(s->dst_cursor, frame->addr, s->frame_size);
  s->dst_cursor += s->frame_size;
  /* parse private data for dynamic ext frame
    if (frame->opaque) {
      do_something(frame->opaque);
    }
  */
  s->fb_recv++;
}

static void* rx_st20p_frame_thread(void* arg) {
  struct app_context* s = arg;
  st20p_rx_handle handle = s->handle;
  struct st_frame* frame;

  printf("%s(%d), start\n", __func__, s->idx);
  while (!s->stop) {
    frame = st20p_rx_get_frame(handle);
    if (!frame) { /* no frame */
      st_pthread_mutex_lock(&s->wake_mutex);
      if (!s->stop) st_pthread_cond_wait(&s->wake_cond, &s->wake_mutex);
      st_pthread_mutex_unlock(&s->wake_mutex);
      continue;
    }
    rx_st20p_consume_frame(s, frame);
    st20p_rx_put_frame(handle, frame);
  }
  printf("%s(%d), stop\n", __func__, s->idx);

  return NULL;
}

int main() {
  struct st_init_params param;
  int session_num = 1;
  int fb_cnt = 3;
  int ret = -EIO;
  struct app_context* app[session_num];
  st_handle dev_handle;
  char* port = getenv("ST_PORT_P");
  if (!port) port = RX_ST20_PORT_BDF;

  bool equal =
      st_frame_fmt_equal_transport(ST20_RX_SAMPLE_OUT_FMT, ST20_RX_SAMPLE_TRAN_FMT);

  for (int i = 0; i < session_num; i++) {
    app[i] = NULL;
  }

  memset(&param, 0, sizeof(param));
  param.num_ports = 1;
  strncpy(param.port[ST_PORT_P], port, ST_PORT_MAX_LEN);
  memcpy(param.sip_addr[ST_PORT_P], g_rx_st20_local_ip, ST_IP_ADDR_LEN);
  param.flags = ST_FLAG_BIND_NUMA | ST_FLAG_DEV_AUTO_START_STOP;
  param.log_level = ST_LOG_LEVEL_INFO;  // log level. ERROR, INFO, WARNING
  param.priv = NULL;                    // usr crx pointer
  param.ptp_get_time_fn = NULL;
  param.rx_sessions_cnt_max = session_num;
  param.tx_sessions_cnt_max = 0;
  param.lcores = NULL;
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

  // create and register rx session
  for (int i = 0; i < session_num; i++) {
    app[i] = malloc(sizeof(struct app_context));
    if (!app[i]) {
      printf("%s, app struct malloc fail\n", __func__);
      ret = -ENOMEM;
      goto err;
    }
    memset(app[i], 0, sizeof(struct app_context));
    app[i]->idx = i;
    app[i]->stop = false;
    st_pthread_mutex_init(&app[i]->wake_mutex, NULL);
    st_pthread_cond_init(&app[i]->wake_cond, NULL);
    app[i]->dst_fd = -1;
    app[i]->fb_cnt = fb_cnt;

    struct st20p_rx_ops ops_rx;
    memset(&ops_rx, 0, sizeof(ops_rx));
    ops_rx.name = "st20p_test";
    ops_rx.priv = app[i];  // app handle register to lib
    ops_rx.port.num_port = 1;
    // rx src ip like 239.0.0.1
    memcpy(ops_rx.port.sip_addr[ST_PORT_P], g_rx_st20_src_ip, ST_IP_ADDR_LEN);
    // send port interface like 0000:af:00.0
    strncpy(ops_rx.port.port[ST_PORT_P], port, ST_PORT_MAX_LEN);
    ops_rx.port.udp_port[ST_PORT_P] = RX_ST20_UDP_PORT + i;
    ops_rx.port.payload_type = RX_ST20_PAYLOAD_TYPE;
    ops_rx.width = 1920;
    ops_rx.height = 1080;
    ops_rx.fps = ST_FPS_P59_94;
    ops_rx.transport_fmt = ST20_RX_SAMPLE_TRAN_FMT;
    ops_rx.output_fmt = ST20_RX_SAMPLE_OUT_FMT;
    ops_rx.device = ST_PLUGIN_DEVICE_AUTO;
    ops_rx.framebuff_cnt = fb_cnt;
    ops_rx.notify_frame_available = rx_st20p_frame_available;

    if (equal) {
      /* no convert, use ext frame for example */
      app[i]->ext_frames =
          (struct st20_ext_frame*)malloc(sizeof(*app[i]->ext_frames) * fb_cnt);
      size_t framebuff_size =
          st20_frame_size(ops_rx.transport_fmt, ops_rx.width, ops_rx.height);
      size_t fb_size = framebuff_size * fb_cnt;
      /* alloc enough memory to hold framebuffers and map to iova */
      st_dma_mem_handle dma_mem = st_dma_mem_alloc(dev_handle, fb_size);
      if (!dma_mem) {
        printf("%s(%d), dma mem alloc/map fail\n", __func__, i);
        ret = -EIO;
        goto err;
      }
      app[i]->dma_mem = dma_mem;

      for (int j = 0; j < fb_cnt; ++j) {
        app[i]->ext_frames[j].buf_addr = st_dma_mem_addr(dma_mem) + j * framebuff_size;
        app[i]->ext_frames[j].buf_iova = st_dma_mem_iova(dma_mem) + j * framebuff_size;
        app[i]->ext_frames[j].buf_len = framebuff_size;
      }
      app[i]->ext_idx = 0;
      // ops_rx.ext_frames = app[i]->ext_frames;
      /* use dynamic external frames */
      ops_rx.query_ext_frame = rx_st20p_query_ext_frame;
      ops_rx.flags |= ST20P_RX_FLAG_RECEIVE_INCOMPLETE_FRAME;
    }

    st20p_rx_handle rx_handle = st20p_rx_create(dev_handle, &ops_rx);
    if (!rx_handle) {
      printf("%s, st20p_rx_create fail\n", __func__);
      ret = -EIO;
      goto err;
    }
    app[i]->handle = rx_handle;

    app[i]->frame_size = st20p_rx_frame_size(rx_handle);
    ret = rx_st20p_open_source(app[i], ST20_RX_SAMPLE_FILE);
    if (ret < 0) {
      goto err;
    }

    ret = pthread_create(&app[i]->frame_thread, NULL, rx_st20p_frame_thread, app[i]);
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

    rx_st20p_close_source(app[i]);
  }

  // release session
  for (int i = 0; i < session_num; i++) {
    printf("%s, fb_recv %d\n", __func__, app[i]->fb_recv);
    ret = st20p_rx_free(app[i]->handle);
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
      if (app[i]->handle) st20p_rx_free(app[i]->handle);
      if (dev_handle && app[i]->dma_mem) st_dma_mem_free(dev_handle, app[i]->dma_mem);
      if (app[i]->ext_frames) free(app[i]->ext_frames);
      free(app[i]);
    }
  }
  if (dev_handle) st_uninit(dev_handle);
  return ret;
}
