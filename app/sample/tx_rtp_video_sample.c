/*
 * Copyright (C) 2021 Intel Corporation.
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

#include <arpa/inet.h>
#include <pthread.h>
#include <st_dpdk_api.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct app_context {
  int idx;
  void* handle;
  bool stop;
  int packet_size;
  int total_packet_in_frame;
  uint32_t rtp_tmstamp;
  uint16_t seq_id;
  uint32_t pkt_idx;
  pthread_t app_thread;
  int fb_send;

  pthread_cond_t wake_cond;
  pthread_mutex_t wake_mutex;
};

static int notify_rtp_done(void* priv) {
  struct app_context* s = (struct app_context*)priv;
  pthread_mutex_lock(&s->wake_mutex);
  pthread_cond_signal(&s->wake_cond);
  pthread_mutex_unlock(&s->wake_mutex);
  return 0;
}

static int app_tx_build_rtp_packet(struct app_context* s, struct st_rfc3550_rtp_hdr* rtp,
                                   uint16_t* pkt_len) {
  uint8_t* payload = (uint8_t*)rtp + sizeof(*rtp);

  /* update hdr */
  rtp->tmstamp = htonl(s->rtp_tmstamp);
  rtp->seq_number = htons(s->seq_id);
  rtp->csrc_count = 0;
  rtp->extension = 0;
  rtp->padding = 0;
  rtp->version = 2;
  rtp->marker = 0;
  rtp->payload_type = 96;
  s->seq_id++;
  *pkt_len = s->packet_size;

  /* feed payload, memset to 0 as example */
  memset(payload, 0, s->packet_size - sizeof(*rtp));

  s->pkt_idx++;
  if (s->pkt_idx >= s->total_packet_in_frame) {
    printf("%s(%d), frame %d done\n", __func__, s->idx, s->fb_send);
    /* end of current frame */
    rtp->marker = 1;

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
    mbuf = st22_tx_get_mbuf(s->handle, &usrptr);
    if (!mbuf) {
      pthread_mutex_lock(&s->wake_mutex);
      /* try again */
      mbuf = st22_tx_get_mbuf(s->handle, &usrptr);
      if (mbuf) {
        pthread_mutex_unlock(&s->wake_mutex);
      } else {
        if (!s->stop) pthread_cond_wait(&s->wake_cond, &s->wake_mutex);
        pthread_mutex_unlock(&s->wake_mutex);
        continue;
      }
    }
    app_tx_build_rtp_packet(s, (struct st_rfc3550_rtp_hdr*)usrptr, &mbuf_len);
    st22_tx_put_mbuf(s->handle, mbuf, mbuf_len);
  }

  return NULL;
}

int main() {
  struct st_init_params param;
  memset(&param, 0, sizeof(param));
  int session_num = 1;
  param.num_ports = 1;
  strncpy(param.port[ST_PORT_P], "0000:af:00.1", ST_PORT_MAX_LEN);
  uint8_t ip[4] = {192, 168, 0, 2};
  memcpy(param.sip_addr[ST_PORT_P], ip, ST_IP_ADDR_LEN);
  param.flags = ST_FLAG_BIND_NUMA;      // default bind to numa
  param.log_level = ST_LOG_LEVEL_INFO;  // log level. ERROR, INFO, WARNING
  param.priv = NULL;                    // usr ctx pointer
  param.ptp_get_time_fn =
      NULL;  // user regist ptp func, if not regist, the internal pt p will be used
  param.tx_sessions_cnt_max = session_num;
  param.rx_sessions_cnt_max = 0;
  param.lcores =
      NULL;  // didnot indicate lcore; let lib decide to core or user could define it.
  st_handle dev_handle = st_init(&param);
  if (!dev_handle) {
    printf("st_init fail\n");
    return -1;
  }
  st22_tx_handle tx_handle[session_num];
  struct app_context* app[session_num];
  int ret;

  // create and register rx session
  for (int i = 0; i < session_num; i++) {
    app[i] = (struct app_context*)malloc(sizeof(struct app_context));
    if (!app[i]) {
      printf(" app struct is not correctly malloc");
      return -1;
    }
    memset(app[i], 0, sizeof(struct app_context));
    app[i]->idx = i;
    struct st22_tx_ops ops_tx;
    memset(&ops_tx, 0, sizeof(ops_tx));
    ops_tx.name = "st22_test";
    ops_tx.priv = app[i];  // app handle register to lib
    ops_tx.num_port = 1;
    uint8_t ip[4] = {239, 168, 0, 1};
    memcpy(ops_tx.dip_addr[ST_PORT_P], ip, ST_IP_ADDR_LEN);  // tx src ip like 239.0.0.1
    strncpy(ops_tx.port[ST_PORT_P], "0000:af:00.1",
            ST_PORT_MAX_LEN);  // send port interface like 0000:af:00.0
    ops_tx.udp_port[ST_PORT_P] =
        10000 + i;  // user could config the udp port in this interface.
    ops_tx.pacing = ST21_PACING_NARROW;
    ops_tx.width = 1920;
    ops_tx.height = 1080;
    ops_tx.fps = ST_FPS_P59_94;
    ops_tx.fmt = ST20_FMT_YUV_422_10BIT;
    ops_tx.payload_type = 96;
    ops_tx.rtp_ring_size = 1024;  // the rtp ring size between app and lib. app is the
                                  // producer, lib is the consumer, should be 2^n

    // app regist non-block func, app could get the rtp tx done
    ops_tx.notify_rtp_done = notify_rtp_done;
    ops_tx.rtp_frame_total_pkts =
        4320;  // 4320 for ex. it is for 1080p, each line, we have 4 packet.
    ops_tx.rtp_pkt_size = 1280 + sizeof(struct st_rfc3550_rtp_hdr);
    // rtp_frame_total_pkts x rtp_pkt_size will be used for Rate limit in the lib.

    tx_handle[i] = st22_tx_create(dev_handle, &ops_tx);
    if (!tx_handle[i]) {
      printf(" tx_session is not correctly created");
      free(app[i]);
      return -1;
    }
    app[i]->handle = tx_handle[i];
    app[i]->stop = false;
    app[i]->packet_size = ops_tx.rtp_pkt_size;
    app[i]->total_packet_in_frame = ops_tx.rtp_frame_total_pkts;
    pthread_mutex_init(&app[i]->wake_mutex, NULL);
    pthread_cond_init(&app[i]->wake_cond, NULL);
    ret = pthread_create(&app[i]->app_thread, NULL, app_tx_rtp_thread, app[i]);
    if (ret < 0) {
      printf("%s(%d), app_thread create fail %d\n", __func__, ret, i);
      ret = st22_tx_free(tx_handle[i]);
      if (ret) {
        printf("session free failed\n");
      }
      free(app[i]);
      return -1;
    }
  }
  // start tx
  ret = st_start(dev_handle);
  // tx 120s
  sleep(120);
  // stop app thread
  for (int i = 0; i < session_num; i++) {
    app[i]->stop = true;
    pthread_mutex_lock(&app[i]->wake_mutex);
    pthread_cond_signal(&app[i]->wake_cond);
    pthread_mutex_unlock(&app[i]->wake_mutex);
    pthread_join(app[i]->app_thread, NULL);
  }

  // stop tx
  ret = st_stop(dev_handle);

  // release session
  for (int i = 0; i < session_num; i++) {
    ret = st22_tx_free(tx_handle[i]);
    if (ret) {
      printf("session free failed\n");
    }
    pthread_mutex_destroy(&app[i]->wake_mutex);
    pthread_cond_destroy(&app[i]->wake_cond);

    free(app[i]);
  }

  // destroy device
  st_uninit(dev_handle);
  return 0;
}
