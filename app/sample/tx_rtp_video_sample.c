/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include <pthread.h>
#include <st20_dpdk_api.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/app_platform.h"

#define TX_VIDEO_PORT_BDF "0000:af:00.1"
#define TX_VIDEO_UDP_PORT (20000)
#define TX_VIDEO_PAYLOAD_TYPE (112)
/* local ip address for current bdf port */
static uint8_t g_tx_video_local_ip[ST_IP_ADDR_LEN] = {192, 168, 0, 2};
/* dst ip address for tx video session */
static uint8_t g_tx_video_dst_ip[ST_IP_ADDR_LEN] = {239, 168, 85, 20};

struct app_context {
  int idx;
  void* handle;
  bool stop;
  int packet_size;
  int total_packet_in_frame;
  uint32_t rtp_tmstamp;
  uint32_t seq_id;
  uint32_t pkt_idx;
  pthread_t app_thread;
  int fb_send;

  pthread_cond_t wake_cond;
  pthread_mutex_t wake_mutex;
};

static bool g_video_active = false;
static st_handle g_st_handle;

static int notify_rtp_done(void* priv) {
  struct app_context* s = (struct app_context*)priv;
  st_pthread_mutex_lock(&s->wake_mutex);
  st_pthread_cond_signal(&s->wake_cond);
  st_pthread_mutex_unlock(&s->wake_mutex);
  return 0;
}

static int app_tx_build_rtp_packet(struct app_context* s,
                                   struct st20_rfc4175_rtp_hdr* rtp, uint16_t* pkt_len) {
  uint8_t* payload = (uint8_t*)rtp + sizeof(*rtp);

  /* update hdr */
  rtp->base.tmstamp = htonl(s->rtp_tmstamp);
  rtp->base.seq_number = htons(s->seq_id);
  rtp->seq_number_ext = htons((uint16_t)(s->seq_id >> 16));
  rtp->base.csrc_count = 0;
  rtp->base.extension = 0;
  rtp->base.padding = 0;
  rtp->base.version = 2;
  rtp->base.marker = 0;
  rtp->base.payload_type = TX_VIDEO_PAYLOAD_TYPE;

  // 4320 for ex. it is for 1080p, each line, we have 4 packet, each 1200 bytes.
  uint16_t row_number, row_offset;
  row_number = s->pkt_idx / 4;         /* 0 to 1079 for 1080p */
  row_offset = 480 * (s->pkt_idx % 4); /* [0, 480, 960, 1440] for 1080p */
  rtp->row_number = htons(row_number);
  rtp->row_offset = htons(row_offset);
  rtp->row_length = htons(1200); /* 1200 for 1080p */

  /* feed payload, memset to 0 as example */
  memset(payload, 0, s->packet_size - sizeof(*rtp));

  *pkt_len = s->packet_size;
  s->seq_id++;
  s->pkt_idx++;
  if (s->pkt_idx >= s->total_packet_in_frame) {
    // printf("%s(%d), frame %d done\n", __func__, s->idx, s->fb_send);
    /* end of current frame */
    rtp->base.marker = 1;

    s->pkt_idx = 0;
    s->rtp_tmstamp++;
    s->fb_send++;
  }

  return 0;
}

static void* app_tx_rtp_thread(void* arg) {
  struct app_context* s = arg;
  void *mbuf, *usrptr;
  uint16_t mbuf_len;
  while (!s->stop) {
    /* get available buffer*/
    mbuf = st20_tx_get_mbuf(s->handle, &usrptr);
    if (!mbuf) {
      st_pthread_mutex_lock(&s->wake_mutex);
      /* try again */
      mbuf = st20_tx_get_mbuf(s->handle, &usrptr);
      if (mbuf) {
        st_pthread_mutex_unlock(&s->wake_mutex);
      } else {
        if (!s->stop) st_pthread_cond_wait(&s->wake_cond, &s->wake_mutex);
        st_pthread_mutex_unlock(&s->wake_mutex);
        continue;
      }
    }
    app_tx_build_rtp_packet(s, (struct st20_rfc4175_rtp_hdr*)usrptr, &mbuf_len);
    st20_tx_put_mbuf(s->handle, mbuf, mbuf_len);
  }

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
  char* port = getenv("ST_PORT_P");
  if (!port) port = TX_VIDEO_PORT_BDF;

  memset(&param, 0, sizeof(param));
  param.num_ports = 1;
  strncpy(param.port[ST_PORT_P], port, ST_PORT_MAX_LEN);
  memcpy(param.sip_addr[ST_PORT_P], g_tx_video_local_ip, ST_IP_ADDR_LEN);
  param.flags = ST_FLAG_BIND_NUMA;      // default bind to numa
  param.log_level = ST_LOG_LEVEL_INFO;  // log level. ERROR, INFO, WARNING
  param.priv = NULL;                    // usr ctx pointer
  param.ptp_get_time_fn =
      NULL;  // user regist ptp func, if not regist, the internal pt p will be used
  param.tx_sessions_cnt_max = session_num;
  param.rx_sessions_cnt_max = 0;
  param.lcores =
      NULL;  // didnot indicate lcore; let lib decide to core or user could define it.
  // create device
  st_handle dev_handle = st_init(&param);
  if (!dev_handle) {
    printf("st_init fail\n");
    return -1;
  }

  g_st_handle = dev_handle;
  signal(SIGINT, app_sig_handler);

  st20_tx_handle tx_handle[session_num];
  struct app_context* app[session_num];
  int ret;
  // create and register tx session
  for (int i = 0; i < session_num; i++) {
    app[i] = (struct app_context*)malloc(sizeof(struct app_context));
    if (!app[i]) {
      printf(" app struct is not correctly malloc");
      return -1;
    }
    memset(app[i], 0, sizeof(struct app_context));
    app[i]->idx = i;
    struct st20_tx_ops ops_tx;
    memset(&ops_tx, 0, sizeof(ops_tx));
    ops_tx.name = "st20_test";
    ops_tx.priv = app[i];  // app handle register to lib
    ops_tx.num_port = 1;
    // tx src ip like 239.0.0.1
    memcpy(ops_tx.dip_addr[ST_PORT_P], g_tx_video_dst_ip, ST_IP_ADDR_LEN);
    // send port interface like 0000:af:00.0
    strncpy(ops_tx.port[ST_PORT_P], port, ST_PORT_MAX_LEN);
    ops_tx.udp_port[ST_PORT_P] =
        TX_VIDEO_UDP_PORT + i;  // user could config the udp port in this interface.
    ops_tx.pacing = ST21_PACING_NARROW;
    ops_tx.type = ST20_TYPE_RTP_LEVEL;
    ops_tx.width = 1920;
    ops_tx.height = 1080;
    ops_tx.fps = ST_FPS_P59_94;
    ops_tx.fmt = ST20_FMT_YUV_422_10BIT;
    ops_tx.payload_type = TX_VIDEO_PAYLOAD_TYPE;
    ops_tx.rtp_ring_size = 1024;  // the rtp ring size between app and lib. app is the
                                  // producer, lib is the consumer, should be 2^n

    // app regist non-block func, app could get the rtp tx done
    ops_tx.notify_rtp_done = notify_rtp_done;
    // 4320 for ex. it is for 1080p, each line, we have 4 packet.
    ops_tx.rtp_frame_total_pkts = 4320;
    ops_tx.rtp_pkt_size = 1200 + sizeof(struct st_rfc3550_rtp_hdr);
    // rtp_frame_total_pkts x rtp_pkt_size will be used for Rate limit in the lib.

    tx_handle[i] = st20_tx_create(dev_handle, &ops_tx);
    if (!tx_handle[i]) {
      printf(" tx_session is not correctly created");
      free(app[i]);
      return -1;
    }
    app[i]->handle = tx_handle[i];
    app[i]->stop = false;
    app[i]->packet_size = ops_tx.rtp_pkt_size;
    app[i]->total_packet_in_frame = ops_tx.rtp_frame_total_pkts;
    st_pthread_mutex_init(&app[i]->wake_mutex, NULL);
    st_pthread_cond_init(&app[i]->wake_cond, NULL);
    ret = pthread_create(&app[i]->app_thread, NULL, app_tx_rtp_thread, app[i]);
    if (ret < 0) {
      printf("%s(%d), app_thread create fail %d\n", __func__, ret, i);
      ret = st20_tx_free(tx_handle[i]);
      if (ret) {
        printf("session free failed\n");
      }
      free(app[i]);
      return -1;
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

  // release session
  for (int i = 0; i < session_num; i++) {
    ret = st20_tx_free(tx_handle[i]);
    if (ret) {
      printf("session free failed\n");
    }
    st_pthread_mutex_destroy(&app[i]->wake_mutex);
    st_pthread_cond_destroy(&app[i]->wake_cond);

    free(app[i]);
  }

  // destroy device
  st_uninit(dev_handle);
  return 0;
}
