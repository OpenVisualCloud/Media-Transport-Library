/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <st20_dpdk_api.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../src/app_platform.h"

#define TX_VIDEO_PMD ST_PMD_DPDK_USER
#define TX_VIDEO_PORT_BDF "0000:4b:00.0"
#define TX_VIDEO_YUV_FILE "../assets/test_4k.yuv"

#define TX_VIDEO_UDP_PORT (20000)
#define TX_VIDEO_PAYLOAD_TYPE (112)

/* local ip address for current bdf port */
static uint8_t g_tx_video_local_ip[ST_IP_ADDR_LEN] = {192, 168, 96, 1};
/* dst ip address for tx video session */
static uint8_t g_tx_video_dst_ip[ST_IP_ADDR_LEN] = {239, 168, 85, 20};

static bool g_video_active = false;
static st_handle g_st_handle;

struct app_context {
  int idx;
  int fb_send;
  int nfi; /* next_frame_idx */
  st20_tx_handle handle;
  struct st20_tx_ops ops;

  size_t frame_size; /* 1080p */
  size_t fb_size;    /* 4k */
  int fb_idx;
  int fb_total;
  size_t fb_offset;

  st_dma_mem_handle dma_mem;
};

static int tx_video_next_frame(void* priv, uint16_t* next_frame_idx,
                               struct st20_tx_frame_meta* meta) {
  struct app_context* s = priv;
  int ret = 0;

  if (!s->handle) return -EIO; /* not ready */

  struct st20_ext_frame ext_frame;
  ext_frame.buf_addr =
      st_dma_mem_addr(s->dma_mem) + s->fb_idx * s->fb_size + s->fb_offset;
  ext_frame.buf_iova =
      st_dma_mem_iova(s->dma_mem) + s->fb_idx * s->fb_size + s->fb_offset;
  ext_frame.buf_len = s->frame_size * 2;
  st20_tx_set_ext_frame(s->handle, s->nfi, &ext_frame);

  *next_frame_idx = s->nfi;
  s->nfi++;
  if (s->nfi >= 3) s->nfi = 0;

  s->fb_idx++;
  if (s->fb_idx >= s->fb_total) s->fb_idx = 0;

  return ret;
}

int tx_video_frame_done(void* priv, uint16_t frame_idx, struct st20_tx_frame_meta* meta) {
  struct app_context* s = priv;
  s->fb_send++;

  /* when using ext frame, the frame lifetime should be considered */
  return 0;
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
  int session_num = 4;
  int fb_cnt = 3;
  char* port = getenv("ST_PORT_P");
  if (!port) port = TX_VIDEO_PORT_BDF;
  char* file = getenv("YUVFILE");
  if (!file) file = TX_VIDEO_YUV_FILE;
  st_dma_mem_handle dma_mem = NULL;
  uint8_t* m = NULL;
  size_t map_size = 0;

  memset(&param, 0, sizeof(param));
  param.num_ports = 1;
  param.pmd[ST_PORT_P] = TX_VIDEO_PMD;
  param.xdp_info[ST_PORT_P].queue_count = session_num;
  param.xdp_info[ST_PORT_P].start_queue = 16;
  strncpy(param.port[ST_PORT_P], port, ST_PORT_MAX_LEN);
  memcpy(param.sip_addr[ST_PORT_P], g_tx_video_local_ip, ST_IP_ADDR_LEN);
  param.flags = ST_FLAG_BIND_NUMA;        // default bind to numa
  param.log_level = ST_LOG_LEVEL_NOTICE;  // log level. ERROR, INFO, WARNING
  param.priv = NULL;                      // usr ctx pointer
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
    app[i]->idx = i;
    app[i]->nfi = 0;
    app[i]->fb_idx = 0;
    app[i]->fb_total = 0;

    struct st20_tx_ops ops_tx;
    memset(&ops_tx, 0, sizeof(ops_tx));
    ops_tx.name = "st20_tx";
    ops_tx.priv = app[i];  // app handle register to lib
    ops_tx.num_port = 1;
    memcpy(ops_tx.dip_addr[ST_PORT_P], g_tx_video_dst_ip, ST_IP_ADDR_LEN);
    strncpy(ops_tx.port[ST_PORT_P], port, ST_PORT_MAX_LEN);

    ops_tx.flags |= ST20_TX_FLAG_EXT_FRAME;
    ops_tx.udp_port[ST_PORT_P] = TX_VIDEO_UDP_PORT + i;
    ops_tx.pacing = ST21_PACING_NARROW;
    ops_tx.packing = ST20_PACKING_GPM_SL;
    ops_tx.type = ST20_TYPE_FRAME_LEVEL;
    ops_tx.width = 1920;
    ops_tx.height = 1080;
    ops_tx.linesize = 9600;
    ops_tx.fps = ST_FPS_P59_94;
    ops_tx.fmt = ST20_FMT_YUV_422_10BIT;
    ops_tx.payload_type = TX_VIDEO_PAYLOAD_TYPE;
    ops_tx.framebuff_cnt = fb_cnt;
    // app regist non-block func, app could get a frame to send to lib
    ops_tx.get_next_frame = tx_video_next_frame;
    ops_tx.notify_frame_done = tx_video_frame_done;
    tx_handle[i] = st20_tx_create(dev_handle, &ops_tx);
    if (!tx_handle[i]) {
      printf("tx_session is not correctly created\n");
      ret = -EIO;
      goto error;
    }
    app[i]->ops = ops_tx;
    app[i]->frame_size = ops_tx.linesize * ops_tx.height / 2;
    app[i]->fb_size = ops_tx.linesize * ops_tx.height * 2;

    if (!dma_mem) {
      /* open yuv file and map to memory */
      int fd = -EIO;
      fd = st_open(file, O_RDONLY);
      if (fd < 0) {
        printf("%s, open %s fail\n", __func__, file);
        ret = -EIO;
        goto error;
      }
      struct stat st;
      fstat(fd, &st);
      if (st.st_size < app[i]->fb_size || st.st_size % app[i]->fb_size) {
        printf("%s, %s file size error %ld\n", __func__, file, st.st_size);
        close(fd);
        ret = -EIO;
        goto error;
      }
      map_size = st.st_size;
      m = mmap(NULL, map_size, PROT_READ, MAP_SHARED, fd, 0);
      if (MAP_FAILED == m) {
        printf("%s, mmap %s fail\n", __func__, file);
        close(fd);
        ret = -EIO;
        goto error;
      }
      close(fd);
      dma_mem = st_dma_mem_alloc(dev_handle, map_size);
      if (!dma_mem) {
        printf("%s(%d), dma mem alloc/map fail\n", __func__, i);
        ret = -EIO;
        goto error;
      }
      void* dst = st_dma_mem_addr(dma_mem);
      st_memcpy(dst, m, map_size);
    }
    app[i]->dma_mem = dma_mem;
    app[i]->fb_total = map_size / app[i]->fb_size;

    app[i]->handle = tx_handle[i];
  }
  /* square division on the 4k frame */
  app[0]->fb_offset = 0;
  app[1]->fb_offset = 4800; /* 1920 * 5 / 2 */
  app[2]->fb_offset = app[0]->frame_size * 2;
  app[3]->fb_offset = app[0]->frame_size * 2 + 4800;

  // start tx
  ret = st_start(dev_handle);
  g_video_active = true;

  while (g_video_active) {
    sleep(1);
  }

  // stop tx
  ret = st_stop(dev_handle);

error:
  // release session
  for (int i = 0; i < session_num; i++) {
    if (!app[i]) continue;
    if (tx_handle[i]) st20_tx_free(tx_handle[i]);
    printf("session(%d) sent frames %d\n", i, app[i]->fb_send);
    free(app[i]);
  }

  // destroy device
  if (dev_handle) {
    if (dma_mem) st_dma_mem_free(dev_handle, dma_mem);
    st_uninit(dev_handle);
  }

  return ret;
}
