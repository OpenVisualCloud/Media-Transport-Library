/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include <rte_ring.h>
#include <st20_dpdk_api.h>

#include "../src/app_platform.h"

#define FWD_PORT_BDF "0000:4b:00.1"
/* local ip address for current bdf port */
static uint8_t g_fwd_local_ip[ST_IP_ADDR_LEN] = {192, 168, 96, 2};

#define RX_ST20_UDP_PORT (20000)
#define RX_ST20_PAYLOAD_TYPE (112)
/* source ip address for rx video session, 239.19.96.1 */
static uint8_t g_rx_video_source_ip[ST_IP_ADDR_LEN] = {239, 19, 96, 1};

#define TX_ST20_UDP_PORT (20000)
#define TX_ST20_PAYLOAD_TYPE (112)
/* dst ip address for tx video session, 239.19.96.2 */
static uint8_t g_tx_st20_dst_ip[ST_IP_ADDR_LEN] = {239, 19, 96, 2};

#define FB_CNT (4) /* 2 is not enough for this case */

struct frame_info {
  void* frame_addr;
  int refcnt;
  uint64_t tmstamp;
};

struct tx {
  void* app;
  st20_tx_handle tx_handle;
  size_t fb_offset;
  int fb_idx;
};

struct app_context {
  st_handle st;
  st20_rx_handle rx_handle;

  struct rte_ring* frame_ring;
  struct frame_info* sending_frames[FB_CNT];

  struct tx tx[4];
  size_t fb_size;

  bool ready;

  int fb_fwd;
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

static int sending_frames_insert(struct app_context* app, struct frame_info* fi) {
  int i;
  for (i = 0; i < FB_CNT; i++) {
    if (app->sending_frames[i] == NULL) {
      app->sending_frames[i] = fi;
      break;
    }
  }
  if (i >= FB_CNT) {
    printf("%s, no slot\n", __func__);
    return -EIO;
  }
  return 0;
}

static int sending_frames_delete(struct app_context* app, void* frame_addr) {
  int i;
  for (i = 0; i < FB_CNT; i++) {
    struct frame_info* fi = app->sending_frames[i];
    if (fi && fi->frame_addr <= frame_addr &&
        frame_addr < fi->frame_addr + app->fb_size) {
      fi->refcnt--;
      if (fi->refcnt == 0) {
        /* all tx sent, release the frame */
        st20_rx_put_framebuff(app->rx_handle, fi->frame_addr);
        free(fi);
        app->sending_frames[i] = NULL;
        app->fb_fwd++;
      }
      break;
    }
  }
  if (i >= FB_CNT) {
    printf("%s, frame %p not found\n", __func__, frame_addr);
    return -EIO;
  }
  return 0;
}

static int rx_st20_frame_ready(void* priv, void* frame, struct st20_rx_frame_meta* meta) {
  struct app_context* s = (struct app_context*)priv;

  if (!s->ready) return -EIO;

  /* incomplete frame */
  if (!st_is_frame_complete(meta->status) || meta->tfmt != ST10_TIMESTAMP_FMT_MEDIA_CLK) {
    st20_rx_put_framebuff(s->rx_handle, frame);
    return -EIO;
  }

  struct frame_info* fi = (struct frame_info*)malloc(sizeof(struct frame_info));
  if (!fi) {
    st20_rx_put_framebuff(s->rx_handle, frame);
    return -EIO;
  }
  memset(fi, 0, sizeof(*fi));

  fi->frame_addr = frame;
  fi->refcnt = 0;
  fi->tmstamp = meta->timestamp;

  if (-ENOBUFS == rte_ring_sp_enqueue(s->frame_ring, fi)) {
    printf("%s, enqueue frame fail\n", __func__);
    return -EIO;
  }

  return 0;
}

static int tx_video_next_frame(void* priv, uint16_t* next_frame_idx,
                               struct st20_tx_frame_meta* meta) {
  struct tx* s = priv;
  struct app_context* app = s->app;

  if (!app->ready) return -EIO;

  int ret;
  int consumer_idx = s->fb_idx;

  struct frame_info* fi[1];
  uint32_t n = rte_ring_dequeue_bulk_start(app->frame_ring, (void**)&fi, 1, NULL);
  if (n != 0) { /* peak the frame from ring */
    ret = 0;
    *next_frame_idx = consumer_idx;
    meta->tfmt = ST10_TIMESTAMP_FMT_MEDIA_CLK;
    meta->timestamp = fi[0]->tmstamp;

    struct st20_ext_frame ext_frame;
    ext_frame.buf_addr = fi[0]->frame_addr + s->fb_offset;
    ext_frame.buf_iova = st_hp_virt2iova(app->st, fi[0]->frame_addr) + s->fb_offset;
    ext_frame.buf_len = 3840 * 2160 * 5 / 4; /* 2 1080p framesize */
    st20_tx_set_ext_frame(s->tx_handle, consumer_idx, &ext_frame);

    fi[0]->refcnt++;
    if (fi[0]->refcnt < 4) {
      /* keep it */
      rte_ring_dequeue_finish(app->frame_ring, 0);
    } else {
      /* all tx set, remove from ring */
      rte_ring_dequeue_finish(app->frame_ring, n);
      /* track the sending frame */
      ret = sending_frames_insert(app, fi[0]);
      if (ret < 0) {
        st20_rx_put_framebuff(app->rx_handle, fi[0]->frame_addr);
        free(fi[0]);
      }
    }

    consumer_idx++;
    if (consumer_idx >= FB_CNT) consumer_idx = 0;
    s->fb_idx = consumer_idx;
  } else {
    /* not ready */
    ret = -EIO;
  }

  return ret;
}

static int tx_video_frame_done(void* priv, uint16_t frame_idx,
                               struct st20_tx_frame_meta* meta) {
  struct tx* s = priv;
  int ret = 0;
  void* frame_addr = st20_tx_get_framebuffer(s->tx_handle, frame_idx);

  /* try to release the sending frame */
  sending_frames_delete(s->app, frame_addr);

  return ret;
}

static int free_app(struct app_context* app) {
  for (int i = 0; i < 4; i++) {
    if (app->tx[i].tx_handle) {
      st20_tx_free(app->tx[i].tx_handle);
      app->tx[i].tx_handle = NULL;
    }
  }
  if (app->rx_handle) {
    st20_rx_free(app->rx_handle);
    app->rx_handle = NULL;
  }
  if (app->st) {
    st_uninit(app->st);
    app->st = NULL;
  }
  if (app->frame_ring) {
    rte_ring_free(app->frame_ring);
    app->frame_ring = NULL;
  }

  return 0;
}

int main() {
  struct st_init_params param;
  struct app_context app;
  st_handle st;
  char* port = getenv("ST_PORT_P");
  if (!port) port = FWD_PORT_BDF;

  memset(&app, 0, sizeof(app));

  memset(&param, 0, sizeof(param));
  param.num_ports = 1;
  strncpy(param.port[ST_PORT_P], port, ST_PORT_MAX_LEN);
  memcpy(param.sip_addr[ST_PORT_P], g_fwd_local_ip, ST_IP_ADDR_LEN);
  param.flags =
      ST_FLAG_BIND_NUMA | ST_FLAG_DEV_AUTO_START_STOP | ST_FLAG_RX_SEPARATE_VIDEO_LCORE;
  param.log_level = ST_LOG_LEVEL_INFO;  // log level. ERROR, INFO, WARNING
  param.priv = NULL;                    // usr ctx pointer
  param.ptp_get_time_fn = NULL;
  param.tx_sessions_cnt_max = 4;
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

  g_st_handle = st;
  g_video_active = true;
  signal(SIGINT, app_sig_handler);

  /* this sample schedules all sessions on single core,
   * so the ring created is not multi-thread safe */
  struct rte_ring* frame_ring =
      rte_ring_create("frame_ring", FB_CNT, SOCKET_ID_ANY, RING_F_SP_ENQ | RING_F_SC_DEQ);
  if (!frame_ring) {
    free_app(&app);
    printf("%s, ring create fail\n", __func__);
    return -EIO;
  }
  app.frame_ring = frame_ring;

  struct st20_rx_ops ops_rx;
  memset(&ops_rx, 0, sizeof(ops_rx));
  ops_rx.name = "st20_fwd";
  ops_rx.priv = &app;
  ops_rx.num_port = 1;
  memcpy(ops_rx.sip_addr[ST_PORT_P], g_rx_video_source_ip, ST_IP_ADDR_LEN);
  strncpy(ops_rx.port[ST_PORT_P], port, ST_PORT_MAX_LEN);
  ops_rx.udp_port[ST_PORT_P] = RX_ST20_UDP_PORT;  // user config the udp port.
  ops_rx.pacing = ST21_PACING_NARROW;
  ops_rx.type = ST20_TYPE_FRAME_LEVEL;
  ops_rx.width = 3840;
  ops_rx.height = 2160;
  ops_rx.fps = ST_FPS_P59_94;
  ops_rx.fmt = ST20_FMT_YUV_422_10BIT;
  ops_rx.framebuff_cnt = FB_CNT;
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

  for (int i = 0; i < 4; i++) {
    struct tx* tx = &app.tx[i];
    tx->app = &app;
    struct st20_tx_ops ops_tx;
    memset(&ops_tx, 0, sizeof(ops_tx));
    ops_tx.name = "st20_fwd";
    ops_tx.priv = tx;
    ops_tx.num_port = 1;
    memcpy(ops_tx.dip_addr[ST_PORT_P], g_tx_st20_dst_ip, ST_IP_ADDR_LEN);
    strncpy(ops_tx.port[ST_PORT_P], port, ST_PORT_MAX_LEN);
    ops_tx.udp_port[ST_PORT_P] = TX_ST20_UDP_PORT + i;
    ops_tx.pacing = ST21_PACING_NARROW;
    ops_tx.packing = ST20_PACKING_BPM;
    ops_tx.type = ST20_TYPE_FRAME_LEVEL;
    ops_tx.width = 1920;
    ops_tx.height = 1080;
    ops_tx.linesize = 9600; /* double 1080p linesize */
    ops_tx.fps = ST_FPS_P59_94;
    ops_tx.fmt = ST20_FMT_YUV_422_10BIT;
    ops_tx.payload_type = TX_ST20_PAYLOAD_TYPE;
    ops_tx.flags |= ST20_TX_FLAG_EXT_FRAME;
    ops_tx.framebuff_cnt = FB_CNT;
    ops_tx.get_next_frame = tx_video_next_frame;
    ops_tx.notify_frame_done = tx_video_frame_done;
    st20_tx_handle tx_handle = st20_tx_create(st, &ops_tx);
    if (!tx_handle) {
      free_app(&app);
      printf("%s, st20_tx_create fail\n", __func__);
      return -EIO;
    }
    tx->tx_handle = tx_handle;
  }

  app.tx[0].fb_offset = 0;               /* origin */
  app.tx[1].fb_offset = 4800;            /* 1080p linesize */
  app.tx[2].fb_offset = 1920 * 1080 * 5; /* 2 1080p framesize */
  app.tx[3].fb_offset = app.tx[2].fb_offset + app.tx[1].fb_offset;
  app.fb_size = 3840 * 2160 * 5 / 2; /* 4k framesize */

  app.ready = true;

  while (g_video_active) {
    sleep(1);
  }

  // release session
  printf("%s, fb_fwd %d\n", __func__, app.fb_fwd);
  free_app(&app);

  return 0;
}