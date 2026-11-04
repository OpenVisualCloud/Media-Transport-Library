/* SPDX-License-Identifier: LicenseRef-IPMX-TBD */

#include "../st2110/pipeline/st20_pipeline_rx.h"
#include "../st2110/pipeline/st20_pipeline_tx.h"
#include "../st2110/pipeline/st22_pipeline_rx.h"
#include "../st2110/pipeline/st22_pipeline_tx.h"
/* first, mt_handle_guard.h needs the DPDK types it pulls in */
#include "../mt_handle_guard.h"
#include "../mt_log.h"
#include "ipmx_sr.h"
#include "ipmx_sr_rx.h"

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

int ipmx_st20_rx_get_sr_info(st20_rx_handle handle, enum mtl_session_port port,
                             struct ipmx_rx_sr_info* info) {
  struct st_rx_video_session_handle_impl* s_impl = handle;
  int ret;

  MT_HANDLE_GUARD(s_impl, MT_HANDLE_RX_VIDEO, -EIO);
  ret = ipmx_sr_rx_get_info(s_impl->impl, port, info);
  MT_HANDLE_RELEASE(s_impl);
  return ret;
}

int ipmx_st20p_rx_get_sr_info(st20p_rx_handle handle, enum mtl_session_port port,
                              struct ipmx_rx_sr_info* info) {
  struct st20p_rx_ctx* ctx = handle;
  int ret;

  MT_HANDLE_GUARD(ctx, MT_ST20_HANDLE_PIPELINE_RX, -EIO);
  ret = ipmx_st20_rx_get_sr_info(ctx->transport, port, info);
  MT_HANDLE_RELEASE(ctx);
  return ret;
}

int ipmx_st22_rx_get_sr_info(st22_rx_handle handle, enum mtl_session_port port,
                             struct ipmx_rx_sr_info* info) {
  struct st22_rx_video_session_handle_impl* s_impl = handle;
  int ret;

  MT_HANDLE_GUARD(s_impl, MT_ST22_HANDLE_RX_VIDEO, -EIO);
  ret = ipmx_sr_rx_get_info(s_impl->impl, port, info);
  MT_HANDLE_RELEASE(s_impl);
  return ret;
}

int ipmx_st22p_rx_get_sr_info(st22p_rx_handle handle, enum mtl_session_port port,
                              struct ipmx_rx_sr_info* info) {
  struct st22p_rx_ctx* ctx = handle;
  int ret;

  MT_HANDLE_GUARD(ctx, MT_ST22_HANDLE_PIPELINE_RX, -EIO);
  ret = ipmx_st22_rx_get_sr_info(ctx->transport, port, info);
  MT_HANDLE_RELEASE(ctx);
  return ret;
}

int ipmx_st20_tx_set_block_version(st20_tx_handle handle, uint8_t version) {
  struct st_tx_video_session_handle_impl* s_impl = handle;
  int ret;

  MT_HANDLE_GUARD(s_impl, MT_HANDLE_TX_VIDEO, -EIO);
  ret = ipmx_sr_tx_set_block_version(s_impl->impl, version);
  MT_HANDLE_RELEASE(s_impl);
  return ret;
}

int ipmx_st20p_tx_set_block_version(st20p_tx_handle handle, uint8_t version) {
  struct st20p_tx_ctx* ctx = handle;
  int ret;

  MT_HANDLE_GUARD(ctx, MT_ST20_HANDLE_PIPELINE_TX, -EIO);
  ret = ipmx_st20_tx_set_block_version(ctx->transport, version);
  MT_HANDLE_RELEASE(ctx);
  return ret;
}

int ipmx_st20_tx_set_ts_refclk(st20_tx_handle handle, const char* ts_refclk) {
  struct st_tx_video_session_handle_impl* s_impl = handle;
  int ret;

  MT_HANDLE_GUARD(s_impl, MT_HANDLE_TX_VIDEO, -EIO);
  ret = ipmx_sr_tx_set_ts_refclk(s_impl->impl, ts_refclk);
  MT_HANDLE_RELEASE(s_impl);
  return ret;
}

int ipmx_st20p_tx_set_ts_refclk(st20p_tx_handle handle, const char* ts_refclk) {
  struct st20p_tx_ctx* ctx = handle;
  int ret;

  MT_HANDLE_GUARD(ctx, MT_ST20_HANDLE_PIPELINE_TX, -EIO);
  ret = ipmx_st20_tx_set_ts_refclk(ctx->transport, ts_refclk);
  MT_HANDLE_RELEASE(ctx);
  return ret;
}

int ipmx_st20_tx_get_info(st20_tx_handle handle, struct ipmx_video_info* info) {
  struct st_tx_video_session_handle_impl* s_impl = handle;
  int ret;

  MT_HANDLE_GUARD(s_impl, MT_HANDLE_TX_VIDEO, -EIO);
  ret = ipmx_sr_tx_get_info(s_impl->impl, info);
  MT_HANDLE_RELEASE(s_impl);
  return ret;
}

int ipmx_st20p_tx_get_info(st20p_tx_handle handle, struct ipmx_video_info* info) {
  struct st20p_tx_ctx* ctx = handle;
  int ret;

  MT_HANDLE_GUARD(ctx, MT_ST20_HANDLE_PIPELINE_TX, -EIO);
  ret = ipmx_st20_tx_get_info(ctx->transport, info);
  MT_HANDLE_RELEASE(ctx);
  return ret;
}

int ipmx_st22_tx_set_block_version(st22_tx_handle handle, uint8_t version) {
  struct st22_tx_video_session_handle_impl* s_impl = handle;
  int ret;

  MT_HANDLE_GUARD(s_impl, MT_ST22_HANDLE_TX_VIDEO, -EIO);
  ret = ipmx_sr_tx_set_block_version(s_impl->impl, version);
  MT_HANDLE_RELEASE(s_impl);
  return ret;
}

int ipmx_st22p_tx_set_block_version(st22p_tx_handle handle, uint8_t version) {
  struct st22p_tx_ctx* ctx = handle;
  int ret;

  MT_HANDLE_GUARD(ctx, MT_ST22_HANDLE_PIPELINE_TX, -EIO);
  ret = ipmx_st22_tx_set_block_version(ctx->transport, version);
  MT_HANDLE_RELEASE(ctx);
  return ret;
}

int ipmx_st22_tx_set_ts_refclk(st22_tx_handle handle, const char* ts_refclk) {
  struct st22_tx_video_session_handle_impl* s_impl = handle;
  int ret;

  MT_HANDLE_GUARD(s_impl, MT_ST22_HANDLE_TX_VIDEO, -EIO);
  ret = ipmx_sr_tx_set_ts_refclk(s_impl->impl, ts_refclk);
  MT_HANDLE_RELEASE(s_impl);
  return ret;
}

int ipmx_st22p_tx_set_ts_refclk(st22p_tx_handle handle, const char* ts_refclk) {
  struct st22p_tx_ctx* ctx = handle;
  int ret;

  MT_HANDLE_GUARD(ctx, MT_ST22_HANDLE_PIPELINE_TX, -EIO);
  ret = ipmx_st22_tx_set_ts_refclk(ctx->transport, ts_refclk);
  MT_HANDLE_RELEASE(ctx);
  return ret;
}

int ipmx_st22_tx_get_info(st22_tx_handle handle, struct ipmx_video_info* info) {
  struct st22_tx_video_session_handle_impl* s_impl = handle;
  int ret;

  MT_HANDLE_GUARD(s_impl, MT_ST22_HANDLE_TX_VIDEO, -EIO);
  ret = ipmx_sr_tx_get_info(s_impl->impl, info);
  MT_HANDLE_RELEASE(s_impl);
  return ret;
}

int ipmx_st22p_tx_get_info(st22p_tx_handle handle, struct ipmx_video_info* info) {
  struct st22p_tx_ctx* ctx = handle;
  int ret;

  MT_HANDLE_GUARD(ctx, MT_ST22_HANDLE_PIPELINE_TX, -EIO);
  ret = ipmx_st22_tx_get_info(ctx->transport, info);
  MT_HANDLE_RELEASE(ctx);
  return ret;
}

int ipmx_st20_tx_get_sdp(st20_tx_handle handle, char* buf, size_t len) {
  struct st_tx_video_session_handle_impl* s_impl = handle;
  int ret;

  MT_HANDLE_GUARD(s_impl, MT_HANDLE_TX_VIDEO, -EIO);
  ret = ipmx_sdp_tx_get(s_impl->impl, buf, len);
  MT_HANDLE_RELEASE(s_impl);
  return ret;
}

int ipmx_st20p_tx_get_sdp(st20p_tx_handle handle, char* buf, size_t len) {
  struct st20p_tx_ctx* ctx = handle;
  int ret;

  MT_HANDLE_GUARD(ctx, MT_ST20_HANDLE_PIPELINE_TX, -EIO);
  ret = ipmx_st20_tx_get_sdp(ctx->transport, buf, len);
  MT_HANDLE_RELEASE(ctx);
  return ret;
}

int ipmx_st22_tx_get_sdp(st22_tx_handle handle, char* buf, size_t len) {
  struct st22_tx_video_session_handle_impl* s_impl = handle;
  int ret;

  MT_HANDLE_GUARD(s_impl, MT_ST22_HANDLE_TX_VIDEO, -EIO);
  ret = ipmx_sdp_tx_get(s_impl->impl, buf, len);
  MT_HANDLE_RELEASE(s_impl);
  return ret;
}

int ipmx_st22p_tx_get_sdp(st22p_tx_handle handle, char* buf, size_t len) {
  struct st22p_tx_ctx* ctx = handle;
  int ret;

  MT_HANDLE_GUARD(ctx, MT_ST22_HANDLE_PIPELINE_TX, -EIO);
  ret = ipmx_st22_tx_get_sdp(ctx->transport, buf, len);
  MT_HANDLE_RELEASE(ctx);
  return ret;
}
