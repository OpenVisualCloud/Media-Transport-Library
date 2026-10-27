/* SPDX-License-Identifier: LicenseRef-IPMX-TBD */

#include "../st2110/pipeline/st20_pipeline_rx.h"
/* first, mt_handle_guard.h needs the DPDK types it pulls in */
#include "../mt_handle_guard.h"
#include "../mt_log.h"

int ipmx_vrx_stat_get(struct st_rx_video_session_impl* s, enum mtl_session_port port,
                      struct ipmx_rx_tp_stat* stat) {
  struct st_rx_video_tp* tp = s->tp;

  if (!s->enable_timing_parser || !(s->ops.flags & ST20_RX_FLAG_IPMX)) {
    err("%s(%d), IPMX timing parser not enabled\n", __func__, s->idx);
    return -EIO;
  }
  if (port >= s->ops.num_port) {
    err("%s(%d), invalid port %d\n", __func__, s->idx, port);
    return -EINVAL;
  }
  if (!tp) {
    dbg("%s(%d), timing parser waits for the stream detection\n", __func__, s->idx);
    return -EBUSY;
  }

  mt_memcpy(stat, &tp->ipmx.port[port].stat, sizeof(*stat));
  return 0;
}

int ipmx_st20_rx_get_tp_stat(st20_rx_handle handle, enum mtl_session_port port,
                             struct ipmx_rx_tp_stat* stat) {
  struct st_rx_video_session_handle_impl* s_impl = handle;
  int ret;

  MT_HANDLE_GUARD(s_impl, MT_HANDLE_RX_VIDEO, -EIO);
  ret = ipmx_vrx_stat_get(s_impl->impl, port, stat);
  MT_HANDLE_RELEASE(s_impl);
  return ret;
}

int ipmx_st20p_rx_get_tp_stat(st20p_rx_handle handle, enum mtl_session_port port,
                              struct ipmx_rx_tp_stat* stat) {
  struct st20p_rx_ctx* ctx = handle;
  int ret;

  MT_HANDLE_GUARD(ctx, MT_ST20_HANDLE_PIPELINE_RX, -EIO);
  ret = ipmx_st20_rx_get_tp_stat(ctx->transport, port, stat);
  MT_HANDLE_RELEASE(ctx);
  return ret;
}
