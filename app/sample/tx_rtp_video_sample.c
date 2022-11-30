/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "sample_util.h"

struct tv_rtp_sample_ctx {
  int idx;
  st20_tx_handle handle;
  bool stop;
  int packet_size;
  int total_packet_in_frame;
  uint8_t payload_type;
  uint32_t rtp_tmstamp;
  uint32_t seq_id;
  uint32_t pkt_idx;
  pthread_t app_thread;
  int fb_send;

  pthread_cond_t wake_cond;
  pthread_mutex_t wake_mutex;
};

static int notify_rtp_done(void* priv) {
  struct tv_rtp_sample_ctx* s = (struct tv_rtp_sample_ctx*)priv;
  st_pthread_mutex_lock(&s->wake_mutex);
  st_pthread_cond_signal(&s->wake_cond);
  st_pthread_mutex_unlock(&s->wake_mutex);
  return 0;
}

static int app_tx_build_rtp_packet(struct tv_rtp_sample_ctx* s,
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
  rtp->base.payload_type = s->payload_type;

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
    dbg("%s(%d), frame %d done\n", __func__, s->idx, s->fb_send);
    /* end of current frame */
    rtp->base.marker = 1;

    s->pkt_idx = 0;
    s->rtp_tmstamp++;
    s->fb_send++;
  }

  return 0;
}

static void* app_tx_rtp_thread(void* arg) {
  struct tv_rtp_sample_ctx* s = arg;
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

int main(int argc, char** argv) {
  struct st_sample_context ctx;
  int ret;

  /* init sample(st) dev */
  memset(&ctx, 0, sizeof(ctx));
  ret = tx_sample_parse_args(&ctx, argc, argv);
  if (ret < 0) return ret;

  ctx.st = mtl_init(&ctx.param);
  if (!ctx.st) {
    err("%s: mtl_init fail\n", __func__);
    return -EIO;
  }

  uint32_t session_num = ctx.sessions;
  st20_tx_handle tx_handle[session_num];
  struct tv_rtp_sample_ctx* app[session_num];

  // create and register tx session
  for (int i = 0; i < session_num; i++) {
    app[i] = (struct tv_rtp_sample_ctx*)malloc(sizeof(struct tv_rtp_sample_ctx));
    if (!app[i]) {
      err("%s(%d), app context malloc fail\n", __func__, i);
      ret = -ENOMEM;
      goto error;
    }
    memset(app[i], 0, sizeof(struct tv_rtp_sample_ctx));
    st_pthread_mutex_init(&app[i]->wake_mutex, NULL);
    st_pthread_cond_init(&app[i]->wake_cond, NULL);
    app[i]->idx = i;

    struct st20_tx_ops ops_tx;
    memset(&ops_tx, 0, sizeof(ops_tx));
    ops_tx.name = "st20_test";
    ops_tx.priv = app[i];  // app handle register to lib
    ops_tx.num_port = 1;
    memcpy(ops_tx.dip_addr[MTL_PORT_P], ctx.tx_dip_addr[MTL_PORT_P], MTL_IP_ADDR_LEN);
    strncpy(ops_tx.port[MTL_PORT_P], ctx.param.port[MTL_PORT_P], MTL_PORT_MAX_LEN);
    ops_tx.udp_port[MTL_PORT_P] = ctx.udp_port + i;
    ops_tx.pacing = ST21_PACING_NARROW;
    ops_tx.type = ST20_TYPE_RTP_LEVEL;
    ops_tx.width = ctx.width;
    ops_tx.height = ctx.height;
    ops_tx.fps = ctx.fps;
    ops_tx.fmt = ctx.fmt;
    ops_tx.payload_type = ctx.payload_type;
    ops_tx.rtp_ring_size = 1024;  // the rtp ring size between app and lib. app is the
                                  // producer, lib is the consumer, should be 2^n

    // app regist non-block func, app could get the rtp tx done
    ops_tx.notify_rtp_done = notify_rtp_done;
    // 4320 for ex. it is for 1080p, each line, we have 4 packet.
    ops_tx.rtp_frame_total_pkts = 4320;
    ops_tx.rtp_pkt_size = 1200 + sizeof(struct st_rfc3550_rtp_hdr);
    // rtp_frame_total_pkts x rtp_pkt_size will be used for Rate limit in the lib.

    tx_handle[i] = st20_tx_create(ctx.st, &ops_tx);
    if (!tx_handle[i]) {
      err("%s(%d), st20_tx_create fail\n", __func__, i);
      ret = -EIO;
      goto error;
    }
    app[i]->payload_type = ops_tx.payload_type;
    app[i]->handle = tx_handle[i];
    app[i]->stop = false;
    app[i]->packet_size = ops_tx.rtp_pkt_size;
    app[i]->total_packet_in_frame = ops_tx.rtp_frame_total_pkts;

    ret = pthread_create(&app[i]->app_thread, NULL, app_tx_rtp_thread, app[i]);
    if (ret < 0) {
      err("%s(%d), app_thread create fail %d\n", __func__, ret, i);
      ret = -EIO;
      goto error;
    }
  }

  // start tx
  ret = mtl_start(ctx.st);

  while (!ctx.exit) {
    sleep(1);
  }

  // stop app thread
  for (int i = 0; i < session_num; i++) {
    app[i]->stop = true;
    st_pthread_mutex_lock(&app[i]->wake_mutex);
    st_pthread_cond_signal(&app[i]->wake_cond);
    st_pthread_mutex_unlock(&app[i]->wake_mutex);
    pthread_join(app[i]->app_thread, NULL);
    info("%s(%d), sent frames %d\n", __func__, i, app[i]->fb_send);
  }

  // stop tx
  ret = mtl_stop(ctx.st);

  // check result
  for (int i = 0; i < session_num; i++) {
    if (app[i]->fb_send <= 0) {
      err("%s(%d), error, no sent frames %d\n", __func__, i, app[i]->fb_send);
      ret = -EIO;
    }
  }

error:
  // release session
  for (int i = 0; i < session_num; i++) {
    if (!app[i]) continue;
    if (app[i]->handle) st20_tx_free(app[i]->handle);
    st_pthread_mutex_destroy(&app[i]->wake_mutex);
    st_pthread_cond_destroy(&app[i]->wake_cond);
    free(app[i]);
  }

  /* release sample(st) dev */
  if (ctx.st) {
    mtl_uninit(ctx.st);
    ctx.st = NULL;
  }
  return ret;
}
