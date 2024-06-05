/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#include <limits.h>

#include "sample_util.h"

struct rx_tp_stat {
  /* stat report */
  int32_t cinst_max;
  int32_t cinst_min;
  int32_t vrx_max;
  int32_t vrx_min;
  int32_t ipt_max;
  int32_t ipt_min;
  int32_t fpt_max;
  int32_t fpt_min;
  int32_t latency_max;
  int32_t latency_min;
  int32_t rtp_offset_max;
  int32_t rtp_offset_min;
  int32_t rtp_ts_delta_max;
  int32_t rtp_ts_delta_min;
  /* result */
  uint32_t compliant_result[ST_RX_TP_COMPLIANT_MAX];
};

struct rx_timing_parser_sample_ctx {
  int idx;
  int fb_cnt;
  st20p_rx_handle handle;

  bool stop;
  pthread_t frame_thread;
  int fb_recv;

  struct st20_rx_tp_pass pass;
  bool pass_get; /* pass critical is only available */

  /* stat report */
  struct rx_tp_stat stat[MTL_SESSION_PORT_MAX];
  uint8_t num_port;
};

static void rx_st20p_tp_stat_init(struct rx_tp_stat* stat) {
  memset(stat, 0, sizeof(*stat));

  stat->cinst_max = INT_MIN;
  stat->cinst_min = INT_MAX;
  stat->vrx_max = INT_MIN;
  stat->vrx_min = INT_MAX;
  stat->ipt_max = INT_MIN;
  stat->ipt_min = INT_MAX;
  stat->fpt_max = INT_MIN;
  stat->fpt_min = INT_MAX;
  stat->latency_max = INT_MIN;
  stat->latency_min = INT_MAX;
  stat->rtp_offset_max = INT_MIN;
  stat->rtp_offset_min = INT_MAX;
  stat->rtp_ts_delta_max = INT_MIN;
  stat->rtp_ts_delta_min = INT_MAX;
}

static void rx_st20p_tp_stat_print(struct rx_timing_parser_sample_ctx* s,
                                   enum mtl_session_port port, struct rx_tp_stat* stat) {
  int idx = s->idx;
  info("%s(%d,%d), COMPLIANT NARROW %d WIDE %d FAILED %d!\n", __func__, idx, port,
       stat->compliant_result[ST_RX_TP_COMPLIANT_NARROW],
       stat->compliant_result[ST_RX_TP_COMPLIANT_WIDE],
       stat->compliant_result[ST_RX_TP_COMPLIANT_FAILED]);
  info("%s(%d), CINST MIN %d MAX %d!\n", __func__, idx, stat->cinst_min, stat->cinst_max);
  info("%s(%d), VRX MIN %d MAX %d!\n", __func__, idx, stat->vrx_min, stat->vrx_max);
  info("%s(%d), IPT MIN %d MAX %d!\n", __func__, idx, stat->ipt_min, stat->ipt_max);
  info("%s(%d), FPT MIN %d MAX %d!\n", __func__, idx, stat->fpt_min, stat->fpt_max);
  info("%s(%d), LATENCY MIN %d MAX %d!\n", __func__, idx, stat->latency_min,
       stat->latency_max);
  info("%s(%d), RTP OFFSET MIN %d MAX %d!\n", __func__, idx, stat->rtp_offset_min,
       stat->rtp_offset_max);
  info("%s(%d), RTP TS DELTA MIN %d MAX %d!\n", __func__, idx, stat->rtp_ts_delta_min,
       stat->rtp_ts_delta_max);
}

static int rx_st20p_tp_consume(struct rx_timing_parser_sample_ctx* s,
                               enum mtl_session_port port, struct st20_rx_tp_meta* tp) {
  if (tp->compliant != ST_RX_TP_COMPLIANT_NARROW) {
    dbg("%s(%d), compliant failed %d cause: %s, frame idx %d\n", __func__, s->idx,
        tp->compliant, tp->failed_cause, s->fb_recv);
  }

  /* update stat */
  struct rx_tp_stat* stat = &s->stat[port];
  stat->vrx_min = ST_MIN(tp->vrx_min, stat->vrx_min);
  stat->vrx_max = ST_MAX(tp->vrx_max, stat->vrx_max);
  stat->cinst_min = ST_MIN(tp->cinst_min, stat->cinst_min);
  stat->cinst_max = ST_MAX(tp->cinst_max, stat->cinst_max);
  stat->ipt_min = ST_MIN(tp->ipt_min, stat->ipt_min);
  stat->ipt_max = ST_MAX(tp->ipt_max, stat->ipt_max);
  stat->fpt_min = ST_MIN(tp->fpt, stat->fpt_min);
  stat->fpt_max = ST_MAX(tp->fpt, stat->fpt_max);
  stat->latency_min = ST_MIN(tp->latency, stat->latency_min);
  stat->latency_max = ST_MAX(tp->latency, stat->latency_max);
  stat->rtp_offset_min = ST_MIN(tp->rtp_offset, stat->rtp_offset_min);
  stat->rtp_offset_max = ST_MAX(tp->rtp_offset, stat->rtp_offset_max);
  stat->rtp_ts_delta_min = ST_MIN(tp->rtp_ts_delta, stat->rtp_ts_delta_min);
  stat->rtp_ts_delta_max = ST_MAX(tp->rtp_ts_delta, stat->rtp_ts_delta_max);

  stat->compliant_result[tp->compliant]++;

  return 0;
}

static void* rx_st20p_tp_thread(void* arg) {
  struct rx_timing_parser_sample_ctx* s = arg;
  st20p_rx_handle handle = s->handle;
  struct st_frame* frame;
  int idx = s->idx;
  int ret;

  info("%s(%d), start\n", __func__, idx);
  while (!s->stop) {
    frame = st20p_rx_get_frame(handle);
    if (!frame) { /* no frame */
      warn("%s(%d), get frame time out\n", __func__, idx);
      continue;
    }
    if (!s->pass_get) {
      ret = st20p_rx_timing_parser_critical(handle, &s->pass);
      if (ret >= 0) {
        s->pass_get = true;
        info("%s(%d), pass critical, cinst narrow %d wide %d, vrx narrow %d wide %d\n",
             __func__, idx, s->pass.cinst_max_narrow, s->pass.cinst_max_wide,
             s->pass.vrx_max_narrow, s->pass.vrx_max_wide);
      }
    }
    rx_st20p_tp_consume(s, MTL_SESSION_PORT_P, frame->tp[MTL_SESSION_PORT_P]);
    if (s->num_port > 1)
      rx_st20p_tp_consume(s, MTL_SESSION_PORT_R, frame->tp[MTL_SESSION_PORT_R]);

    s->fb_recv++;
    st20p_rx_put_frame(handle, frame);
  }
  info("%s(%d), stop\n", __func__, idx);

  return NULL;
}

int main(int argc, char** argv) {
  struct st_sample_context ctx;
  int ret;

  /* init sample(st) dev */
  memset(&ctx, 0, sizeof(ctx));
  ret = rx_sample_parse_args(&ctx, argc, argv);
  if (ret < 0) return ret;

  /* enable hw offload timestamp */
  ctx.param.flags |= MTL_FLAG_ENABLE_HW_TIMESTAMP;

  /* enable auto start/stop */
  ctx.param.flags |= MTL_FLAG_DEV_AUTO_START_STOP;
  ctx.st = mtl_init(&ctx.param);
  if (!ctx.st) {
    err("%s: mtl_init fail\n", __func__);
    return -EIO;
  }

  uint32_t session_num = ctx.sessions;
  struct rx_timing_parser_sample_ctx* app[session_num];

  // create and register rx session
  for (int i = 0; i < session_num; i++) {
    app[i] = malloc(sizeof(struct rx_timing_parser_sample_ctx));
    if (!app[i]) {
      err("%s(%d), app context malloc fail\n", __func__, i);
      ret = -ENOMEM;
      goto error;
    }
    memset(app[i], 0, sizeof(struct rx_timing_parser_sample_ctx));
    app[i]->idx = i;
    app[i]->stop = false;
    app[i]->fb_cnt = ctx.framebuff_cnt;
    app[i]->num_port = ctx.param.num_ports;

    rx_st20p_tp_stat_init(&app[i]->stat[MTL_SESSION_PORT_P]);
    rx_st20p_tp_stat_init(&app[i]->stat[MTL_SESSION_PORT_R]);

    struct st20p_rx_ops ops_rx;
    memset(&ops_rx, 0, sizeof(ops_rx));
    ops_rx.name = "st20p_test";
    ops_rx.priv = app[i];  // app handle register to lib
    ops_rx.port.num_port = app[i]->num_port;
    memcpy(ops_rx.port.ip_addr[MTL_SESSION_PORT_P], ctx.rx_ip_addr[MTL_PORT_P],
           MTL_IP_ADDR_LEN);
    snprintf(ops_rx.port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
             ctx.param.port[MTL_PORT_P]);
    ops_rx.port.udp_port[MTL_SESSION_PORT_P] = ctx.udp_port + i * 2;
    if (ops_rx.port.num_port > 1) {
      memcpy(ops_rx.port.ip_addr[MTL_SESSION_PORT_R], ctx.rx_ip_addr[MTL_PORT_R],
             MTL_IP_ADDR_LEN);
      snprintf(ops_rx.port.port[MTL_SESSION_PORT_R], MTL_PORT_MAX_LEN, "%s",
               ctx.param.port[MTL_PORT_R]);
      ops_rx.port.udp_port[MTL_SESSION_PORT_R] = ctx.udp_port + i * 2;
    }
    if (ctx.multi_inc_addr) {
      /* use a new ip addr instead of a new udp port for multi sessions */
      ops_rx.port.udp_port[MTL_SESSION_PORT_P] = ctx.udp_port;
      ops_rx.port.ip_addr[MTL_SESSION_PORT_P][3] += i;
    }
    ops_rx.port.payload_type = ctx.payload_type;
    ops_rx.width = ctx.width;
    ops_rx.height = ctx.height;
    ops_rx.fps = ctx.fps;
    ops_rx.interlaced = ctx.interlaced;
    ops_rx.transport_fmt = ctx.fmt;
    ops_rx.output_fmt = ctx.output_fmt;
    ops_rx.device = ST_PLUGIN_DEVICE_AUTO;
    ops_rx.framebuff_cnt = app[i]->fb_cnt;
    ops_rx.rx_burst_size = ctx.rx_burst_size;
    ops_rx.flags = ST20P_RX_FLAG_BLOCK_GET;
    ops_rx.flags |= ST20P_RX_FLAG_TIMING_PARSER_META;

    st20p_rx_handle rx_handle = st20p_rx_create(ctx.st, &ops_rx);
    if (!rx_handle) {
      err("%s(%d), st20p_rx_create fail\n", __func__, i);
      ret = -EIO;
      goto error;
    }
    app[i]->handle = rx_handle;

    ret = pthread_create(&app[i]->frame_thread, NULL, rx_st20p_tp_thread, app[i]);
    if (ret < 0) {
      err("%s(%d), thread create fail %d\n", __func__, ret, i);
      ret = -EIO;
      goto error;
    }
  }

  int cnt = 0;
  while (!ctx.exit) {
    sleep(1);
    cnt++;
    if ((cnt % 10) == 0) {
      for (int i = 0; i < session_num; i++) {
        rx_st20p_tp_stat_print(app[i], MTL_SESSION_PORT_P,
                               &app[i]->stat[MTL_SESSION_PORT_P]);
        rx_st20p_tp_stat_init(&app[i]->stat[MTL_SESSION_PORT_P]);
        if (app[i]->num_port > 1) {
          rx_st20p_tp_stat_print(app[i], MTL_SESSION_PORT_R,
                                 &app[i]->stat[MTL_SESSION_PORT_R]);
          rx_st20p_tp_stat_init(&app[i]->stat[MTL_SESSION_PORT_R]);
        }
      }
    }
  }

  // stop app thread
  for (int i = 0; i < session_num; i++) {
    app[i]->stop = true;
    if (app[i]->handle) st20p_rx_wake_block(app[i]->handle);
    pthread_join(app[i]->frame_thread, NULL);
    info("%s(%d), received frames %d\n", __func__, i, app[i]->fb_recv);
  }

  // check result
  for (int i = 0; i < session_num; i++) {
    if (app[i]->fb_recv <= 0) {
      err("%s(%d), error, no received frames %d\n", __func__, i, app[i]->fb_recv);
      ret = -EIO;
    }
  }

error:
  for (int i = 0; i < session_num; i++) {
    if (app[i]) {
      if (app[i]->handle) st20p_rx_free(app[i]->handle);
      free(app[i]);
    }
  }

  /* release sample(st) dev */
  if (ctx.st) {
    mtl_uninit(ctx.st);
    ctx.st = NULL;
  }
  return ret;
}
