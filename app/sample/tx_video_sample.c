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

#define TX_EXT_FRAME

#if 0
#define TX_VIDEO_PMD ST_PMD_DPDK_AF_XDP
#define TX_VIDEO_PORT_BDF "ens801f1"
//#define TX_VIDEO_DST_MAC "b4:96:91:aa:bb:08"
#endif

#if 1
#define TX_VIDEO_PMD ST_PMD_DPDK_USER
#define TX_VIDEO_PORT_BDF "0000:af:00.1"
#endif

#define TX_VIDEO_UDP_PORT (20000)
#define TX_VIDEO_PAYLOAD_TYPE (112)

/* local ip address for current bdf port */
static uint8_t g_tx_video_local_ip[ST_IP_ADDR_LEN] = {192, 168, 0, 2};
/* dst ip address for tx video session */
static uint8_t g_tx_video_dst_ip[ST_IP_ADDR_LEN] = {239, 168, 85, 20};

static bool g_video_active = false;
static st_handle g_st_handle;

struct app_context {
  int idx;
  int fb_send;
  st20_tx_handle handle;
  struct st20_tx_ops ops;

  bool stop;
  pthread_t app_thread;
  pthread_cond_t wake_cond;
  pthread_mutex_t wake_mutex;

  int framebuff_size;
  uint16_t framebuff_cnt;
  uint16_t framebuff_producer_idx;
  uint16_t framebuff_consumer_idx;
  struct st_tx_frame* framebuffs;

  st_dma_mem_handle dma_mem;
};

static int tx_video_next_frame(void* priv, uint16_t* next_frame_idx,
                               struct st20_tx_frame_meta* meta) {
  struct app_context* s = priv;
  int ret;
  uint16_t consumer_idx = s->framebuff_consumer_idx;
  struct st_tx_frame* framebuff = &s->framebuffs[consumer_idx];

  if (!s->handle) return -EIO; /* not ready */

  st_pthread_mutex_lock(&s->wake_mutex);
  if (ST_TX_FRAME_READY == framebuff->stat) {
    // printf("%s(%d), next frame idx %u\n", __func__, s->idx, consumer_idx);
    ret = 0;
    framebuff->stat = ST_TX_FRAME_IN_TRANSMITTING;
    *next_frame_idx = consumer_idx;
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

static int tx_video_frame_done(void* priv, uint16_t frame_idx,
                               struct st20_tx_frame_meta* meta) {
  struct app_context* s = priv;
  int ret;
  struct st_tx_frame* framebuff = &s->framebuffs[frame_idx];

  if (!s->handle) return -EIO; /* not ready */

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

static void tx_video_build_frame(struct app_context* s, void* frame, size_t frame_size) {
  /* call the real build here, sample just sleep */
  st_usleep(10 * 1000);
}

static void* tx_video_frame_thread(void* arg) {
  struct app_context* s = arg;
  uint16_t producer_idx;
  struct st_tx_frame* framebuff;

  printf("%s(%d), start\n", __func__, s->idx);
  while (!s->stop) {
    st_pthread_mutex_lock(&s->wake_mutex);
    producer_idx = s->framebuff_producer_idx;
    framebuff = &s->framebuffs[producer_idx];
    if (ST_TX_FRAME_FREE != framebuff->stat) {
      /* not in free */
      if (!s->stop) st_pthread_cond_wait(&s->wake_cond, &s->wake_mutex);
      st_pthread_mutex_unlock(&s->wake_mutex);
      continue;
    }
    st_pthread_mutex_unlock(&s->wake_mutex);
    if (s->ops.flags & ST20_TX_FLAG_EXT_FRAME) { /* ext frame mode */
      struct st20_ext_frame ext_frame;
      ext_frame.buf_addr = st_dma_mem_addr(s->dma_mem) + producer_idx * s->framebuff_size;
      ext_frame.buf_iova = st_dma_mem_iova(s->dma_mem) + producer_idx * s->framebuff_size;
      ext_frame.buf_len = s->framebuff_size;
      st20_tx_set_ext_frame(s->handle, producer_idx, &ext_frame);
    } else {
      void* frame_addr = st20_tx_get_framebuffer(s->handle, producer_idx);
      tx_video_build_frame(s, frame_addr, s->framebuff_size);
    }
    st_pthread_mutex_lock(&s->wake_mutex);
    framebuff->size = s->framebuff_size;
    framebuff->stat = ST_TX_FRAME_READY;
    /* point to next */
    producer_idx++;
    if (producer_idx >= s->framebuff_cnt) producer_idx = 0;
    s->framebuff_producer_idx = producer_idx;
    st_pthread_mutex_unlock(&s->wake_mutex);
  }
  printf("%s(%d), stop\n", __func__, s->idx);

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
  if (!port) port = TX_VIDEO_PORT_BDF;

  memset(&param, 0, sizeof(param));
  param.num_ports = 1;
  param.pmd[ST_PORT_P] = TX_VIDEO_PMD;
  param.xdp_info[ST_PORT_P].queue_count = session_num;
  param.xdp_info[ST_PORT_P].start_queue = 16;
  strncpy(param.port[ST_PORT_P], port, ST_PORT_MAX_LEN);
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

  g_st_handle = dev_handle;
  signal(SIGINT, app_sig_handler);

  st20_tx_handle tx_handle[session_num];
  memset(tx_handle, 0, sizeof(tx_handle));
  struct app_context* app[session_num];
  memset(app, 0, sizeof(app));
  int ret;
  // create and register tx session
  for (int i = 0; i < session_num; i++) {
    app[i] = (struct app_context*)malloc(sizeof(struct app_context));
    if (!app[i]) {
      printf(" app struct is not correctly malloc");
      ret = -ENOMEM;
      goto error;
    }
    memset(app[i], 0, sizeof(struct app_context));
    st_pthread_mutex_init(&app[i]->wake_mutex, NULL);
    st_pthread_cond_init(&app[i]->wake_cond, NULL);
    app[i]->idx = i;
    app[i]->framebuff_cnt = fb_cnt;
    app[i]->framebuffs =
        (struct st_tx_frame*)malloc(sizeof(*app[i]->framebuffs) * app[i]->framebuff_cnt);
    if (!app[i]->framebuffs) {
      printf("%s, framebuffs malloc fail\n", __func__);
      ret = -ENOMEM;
      goto error;
    }
    for (uint16_t j = 0; j < app[i]->framebuff_cnt; j++) {
      app[i]->framebuffs[j].stat = ST_TX_FRAME_FREE;
    }

    struct st20_tx_ops ops_tx;
    memset(&ops_tx, 0, sizeof(ops_tx));
    ops_tx.name = "st20_tx";
    ops_tx.priv = app[i];  // app handle register to lib
    ops_tx.num_port = 1;
    // tx src ip like 239.0.0.1
    memcpy(ops_tx.dip_addr[ST_PORT_P], g_tx_video_dst_ip, ST_IP_ADDR_LEN);
    // send port interface like 0000:af:00.0
    strncpy(ops_tx.port[ST_PORT_P], port, ST_PORT_MAX_LEN);
#ifdef TX_VIDEO_DST_MAC
    uint8_t* mac = ops_tx.tx_dst_mac[ST_PORT_P];
    sscanf(TX_VIDEO_DST_MAC, "%02hhx:%02hhx:%02hhx:%02hhx:%02hhx:%02hhx", &mac[0],
           &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]);
    ops_tx.flags |= ST20_TX_FLAG_USER_P_MAC;
#endif
#ifdef TX_EXT_FRAME
    ops_tx.flags |= ST20_TX_FLAG_EXT_FRAME;
#endif
    ops_tx.udp_port[ST_PORT_P] = TX_VIDEO_UDP_PORT + i;  // udp port
    ops_tx.pacing = ST21_PACING_NARROW;
    ops_tx.type = ST20_TYPE_FRAME_LEVEL;
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
    tx_handle[i] = st20_tx_create(dev_handle, &ops_tx);
    if (!tx_handle[i]) {
      printf("tx_session is not correctly created\n");
      ret = -EIO;
      goto error;
    }
    app[i]->ops = ops_tx;
    app[i]->framebuff_size = st20_tx_get_framebuffer_size(tx_handle[i]);
    if (ops_tx.flags & ST20_TX_FLAG_EXT_FRAME) {
      /* how user allocate framebuffers and map to iova */
      /* the memory malloc layout:
      |____________________|////////// valid framebuffers ////////|____|___|
      |                    |<--------------- size --------------->|    |   |
      |                    |<---------------- iova_size -------------->|   |
      |<---------------------- alloc_size (pgsz multiple)----------------->|
      *alloc_addr          *addr(pg aligned)
      */
      size_t fb_size = app[i]->framebuff_size * fb_cnt;
      /* alloc enough memory to hold framebuffers and map to iova */
      st_dma_mem_handle dma_mem = st_dma_mem_alloc(dev_handle, fb_size);
      if (!dma_mem) {
        printf("%s(%d), dma mem alloc/map fail\n", __func__, i);
        ret = -EIO;
        goto error;
      }
      app[i]->dma_mem = dma_mem;
    }

    app[i]->handle = tx_handle[i];

    app[i]->stop = false;
    ret = pthread_create(&app[i]->app_thread, NULL, tx_video_frame_thread, app[i]);
    if (ret < 0) {
      printf("%s(%d), app_thread create fail %d\n", __func__, ret, i);
      ret = -EIO;
      goto error;
    }
  }

  // start tx
  ret = st_start(dev_handle);
  g_video_active = true;

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

  // stop tx
  ret = st_stop(dev_handle);

error:
  // release session
  for (int i = 0; i < session_num; i++) {
    if (!app[i]) continue;
    if (tx_handle[i]) st20_tx_free(tx_handle[i]);
    st_pthread_mutex_destroy(&app[i]->wake_mutex);
    st_pthread_cond_destroy(&app[i]->wake_cond);
    printf("session(%d) sent frames %d\n", i, app[i]->fb_send);
    if (app[i]->dma_mem) st_dma_mem_free(dev_handle, app[i]->dma_mem);
    if (app[i]->framebuffs) free(app[i]->framebuffs);
    free(app[i]);
  }

  // destroy device
  if (dev_handle) st_uninit(dev_handle);
  return ret;
}
