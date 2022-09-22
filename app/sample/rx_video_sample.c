/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <st20_dpdk_api.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/app_platform.h"

#if 0
#define RX_VIDEO_PMD ST_PMD_DPDK_AF_XDP
#define RX_VIDEO_PORT_BDF "ens801f0"
#endif

#if 1
#define RX_VIDEO_PMD ST_PMD_DPDK_USER
#define RX_VIDEO_PORT_BDF "0000:af:00.0"
#endif

#define RX_VIDEO_UDP_PORT (20000)
#define RX_VIDEO_PAYLOAD_TYPE (112)

#define EXT_FRAME (1)

/* local ip address for current bdf port */
static uint8_t g_rx_video_local_ip[ST_IP_ADDR_LEN] = {192, 168, 0, 1};
/* source ip address for rx video session */
static uint8_t g_rx_video_source_ip[ST_IP_ADDR_LEN] = {239, 168, 85, 20};

static bool g_update_src_test = false;
#define RX_VIDEO_UDP_PORT_2 (RX_VIDEO_UDP_PORT + 1)
static uint8_t g_rx_video_source_ip2[ST_IP_ADDR_LEN] = {239, 168, 0, 2};

static bool g_video_active = false;
static st_handle g_st_handle;

struct app_context {
  int idx;
  int fb_rec;
  void* handle;

  bool stop;
  pthread_t app_thread;
  pthread_cond_t wake_cond;
  pthread_mutex_t wake_mutex;

  uint16_t framebuff_cnt;
  uint16_t framebuff_producer_idx;
  uint16_t framebuff_consumer_idx;
  struct st_rx_frame* framebuffs;

  st_dma_mem_handle dma_mem;
  struct st20_ext_frame* ext_frames;
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
    st20_rx_put_framebuff(s->handle, frame);
    return 0;
  }

  st_pthread_mutex_lock(&s->wake_mutex);
  int ret = rx_video_enqueue_frame(s, frame, meta->frame_total_size);
  if (ret < 0) {
    printf("%s(%d), frame %p dropped\n", __func__, s->idx, frame);
    /* free the queue */
    st20_rx_put_framebuff(s->handle, frame);
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
    st20_rx_put_framebuff(s->handle, framebuff->frame);
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
  char* port = getenv("ST_PORT_P");
  if (!port) port = RX_VIDEO_PORT_BDF;

  memset(&param, 0, sizeof(param));
  param.num_ports = 1;
  strncpy(param.port[ST_PORT_P], port, ST_PORT_MAX_LEN);
  memcpy(param.sip_addr[ST_PORT_P], g_rx_video_local_ip, ST_IP_ADDR_LEN);
  param.pmd[ST_PORT_P] = RX_VIDEO_PMD;
  param.xdp_info[ST_PORT_P].queue_count = session_num;
  param.xdp_info[ST_PORT_P].start_queue = 16;
  param.flags = ST_FLAG_BIND_NUMA;      // default bind to numa
  param.log_level = ST_LOG_LEVEL_INFO;  // log level. ERROR, INFO, WARNING
  param.priv = NULL;                    // usr ctx pointer
  // user regist ptp func, if not regist, the internal pt p will be used
  param.ptp_get_time_fn = NULL;
  param.tx_sessions_cnt_max = 0;
  param.rx_sessions_cnt_max = session_num;
  param.lcores = NULL;
  // create device
  st_handle dev_handle = st_init(&param);
  if (!dev_handle) {
    printf("st_init fail\n");
    return -EIO;
  }

  g_st_handle = dev_handle;
  signal(SIGINT, app_sig_handler);

  st20_rx_handle rx_handle[session_num];
  struct app_context* app[session_num];
  int ret;
  // create and register rx session
  for (int i = 0; i < session_num; i++) {
    app[i] = (struct app_context*)malloc(sizeof(struct app_context));
    if (!app[i]) {
      printf("app struct is malloc failed");
      ret = -ENOMEM;
      goto error;
    }
    memset(app[i], 0, sizeof(struct app_context));
    app[i]->idx = i;
    app[i]->framebuff_cnt = fb_cnt;
    app[i]->framebuffs =
        (struct st_rx_frame*)malloc(sizeof(*app[i]->framebuffs) * app[i]->framebuff_cnt);
    if (!app[i]->framebuffs) {
      printf("%s, framebuffs malloc fail\n", __func__);
      ret = -ENOMEM;
      goto error;
    }
    for (uint16_t j = 0; j < app[i]->framebuff_cnt; j++)
      app[i]->framebuffs[j].frame = NULL;
    app[i]->framebuff_producer_idx = 0;
    app[i]->framebuff_consumer_idx = 0;

    struct st20_rx_ops ops_rx;
    memset(&ops_rx, 0, sizeof(ops_rx));
    ops_rx.name = "st20_test";
    ops_rx.priv = app[i];  // app handle register to lib
    ops_rx.num_port = 1;
    memcpy(ops_rx.sip_addr[ST_PORT_P], g_rx_video_source_ip, ST_IP_ADDR_LEN);
    strncpy(ops_rx.port[ST_PORT_P], port, ST_PORT_MAX_LEN);
    ops_rx.udp_port[ST_PORT_P] = RX_VIDEO_UDP_PORT + i;  // user config the udp port.
    ops_rx.pacing = ST21_PACING_NARROW;
    ops_rx.type = ST20_TYPE_FRAME_LEVEL;
    ops_rx.width = 1920;
    ops_rx.height = 1080;
    ops_rx.fps = ST_FPS_P59_94;
    ops_rx.fmt = ST20_FMT_YUV_422_10BIT;
    ops_rx.framebuff_cnt = fb_cnt;
    ops_rx.payload_type = RX_VIDEO_PAYLOAD_TYPE;
    // app regist non-block func, app get a frame ready notification info by this cb
    ops_rx.notify_frame_ready = rx_video_frame_ready;

    if (EXT_FRAME) {
      app[i]->ext_frames = (struct st20_ext_frame*)malloc(sizeof(*app[i]->ext_frames) *
                                                          app[i]->framebuff_cnt);
      size_t framebuff_size = st20_frame_size(ops_rx.fmt, ops_rx.width, ops_rx.height);
      size_t fb_size = framebuff_size * fb_cnt;
      /* alloc enough memory to hold framebuffers and map to iova */
      st_dma_mem_handle dma_mem = st_dma_mem_alloc(dev_handle, fb_size);
      if (!dma_mem) {
        printf("%s(%d), dma mem alloc/map fail\n", __func__, i);
        ret = -EIO;
        goto error;
      }
      app[i]->dma_mem = dma_mem;

      for (int j = 0; j < fb_cnt; ++j) {
        app[i]->ext_frames[j].buf_addr = st_dma_mem_addr(dma_mem) + j * framebuff_size;
        app[i]->ext_frames[j].buf_iova = st_dma_mem_iova(dma_mem) + j * framebuff_size;
        app[i]->ext_frames[j].buf_len = framebuff_size;
      }
      ops_rx.ext_frames = app[i]->ext_frames;
    }

    rx_handle[i] = st20_rx_create(dev_handle, &ops_rx);
    if (!rx_handle[i]) {
      printf("rx_session is not correctly created for %d", i);
      ret = -EIO;
      goto error;
    }
    app[i]->handle = rx_handle[i];
    struct st_queue_meta queue_meta;
    st20_rx_get_queue_meta(rx_handle[i], &queue_meta);
    printf("queue_id %u, start_queue %u\n", queue_meta.queue_id[ST_PORT_P],
           queue_meta.start_queue[ST_PORT_P]);
    app[i]->stop = false;
    st_pthread_mutex_init(&app[i]->wake_mutex, NULL);
    st_pthread_cond_init(&app[i]->wake_cond, NULL);
    ret = pthread_create(&app[i]->app_thread, NULL, rx_video_frame_thread, app[i]);
    if (ret < 0) {
      printf("%s(%d), app_thread create fail %d\n", __func__, ret, i);
      goto error;
    }
  }

  // start rx
  ret = st_start(dev_handle);

  g_video_active = true;

  // update ip and port api calling
  if (g_update_src_test) {
    sleep(10);
    struct st_rx_source_info src;
    /* switch to mcast port p(tx_session:1) */
    memset(&src, 0, sizeof(src));
    src.udp_port[ST_PORT_P] = 20000;
    memcpy(src.sip_addr[ST_PORT_P], g_rx_video_source_ip2, ST_IP_ADDR_LEN);
    ret = st20_rx_update_source(rx_handle[0], &src);

    sleep(10);
    /* switch to mcast port p(tx_session:1) */
    memset(&src, 0, sizeof(src));
    src.udp_port[ST_PORT_P] = RX_VIDEO_UDP_PORT;
    memcpy(src.sip_addr[ST_PORT_P], g_rx_video_source_ip, ST_IP_ADDR_LEN);
    ret = st20_rx_update_source(rx_handle[0], &src);
  }

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
    if (rx_handle[i]) st20_rx_free(rx_handle[i]);
    if (!app[i]) continue;
    st_pthread_mutex_destroy(&app[i]->wake_mutex);
    st_pthread_cond_destroy(&app[i]->wake_cond);
    printf("session(%d) received frames %d\n", i, app[i]->fb_rec);
    if (app[i]->dma_mem) st_dma_mem_free(dev_handle, app[i]->dma_mem);
    if (app[i]->framebuffs) free(app[i]->framebuffs);
    if (app[i]->ext_frames) free(app[i]->ext_frames);
    free(app[i]);
  }

  // destroy device
  st_uninit(dev_handle);
  return 0;
}
