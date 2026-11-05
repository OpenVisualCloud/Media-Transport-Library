/* SPDX-License-Identifier: LicenseRef-IPMX-TBD */

#include "app_ipmx.h"

#include <inttypes.h>
#include <mtl/ipmx/ipmx_api.h>

struct app_ipmx_tx {
  char path[ST_APP_URL_MAX_LEN + 64];
  char sdp[IPMX_SDP_MAX];
  bool sdp_err;
  bool write_err;
  const char* ts_refclk;
};

int app_ipmx_set_sdp_dir(struct st_app_context* ctx, const char* dir) {
  if (snprintf(ctx->ipmx_sdp_dir, sizeof(ctx->ipmx_sdp_dir), "%s", dir) >=
      (int)sizeof(ctx->ipmx_sdp_dir)) {
    err("%s, dir %s too long\n", __func__, dir);
    return -EINVAL;
  }
  return 0;
}

int app_ipmx_set_ts_refclk(struct st_app_context* ctx, const char* ts_refclk) {
  if (snprintf(ctx->ipmx_ts_refclk, sizeof(ctx->ipmx_ts_refclk), "%s", ts_refclk) >=
      (int)sizeof(ctx->ipmx_ts_refclk)) {
    err("%s, ts_refclk %s too long\n", __func__, ts_refclk);
    return -EINVAL;
  }
  return 0;
}

/* write via <path>.tmp and rename(), so a reader never sees a partial file */
static int app_ipmx_sdp_write(struct app_ipmx_tx* t, const char* sdp, size_t len) {
  char tmp[sizeof(t->path) + 4];
  FILE* f;
  size_t written;
  int closed;
  const char* o;

  snprintf(tmp, sizeof(tmp), "%s.tmp", t->path);
  f = fopen(tmp, "w");
  if (!f) {
    if (!t->write_err) err("%s, open %s fail: %s\n", __func__, tmp, strerror(errno));
    t->write_err = true;
    return -EIO;
  }
  written = fwrite(sdp, 1, len, f);
  closed = fclose(f);
  if (written != len || closed != 0 || rename(tmp, t->path) < 0) {
    if (!t->write_err) err("%s, write %s fail: %s\n", __func__, t->path, strerror(errno));
    t->write_err = true;
    unlink(tmp);
    return -EIO;
  }
  t->write_err = false;
  memcpy(t->sdp, sdp, len + 1);

  o = strstr(sdp, "o=");
  if (!o) o = "";
  info("%s, %s, %.*s\n", __func__, t->path, (int)strcspn(o, "\r\n"), o);
  return 0;
}

static int app_ipmx_tx_open(struct st_app_context* ctx, struct app_ipmx_tx** ipmx,
                            const char* name, const char* sdp, size_t len) {
  const char* dir = ctx->ipmx_sdp_dir[0] ? ctx->ipmx_sdp_dir : ".";
  struct app_ipmx_tx* t = st_app_zmalloc(sizeof(*t));
  int ret;

  if (!t) return -ENOMEM;
  if (snprintf(t->path, sizeof(t->path), "%s/%s.sdp", dir, name) >=
      (int)sizeof(t->path)) {
    err("%s(%s), sdp path too long\n", __func__, name);
    st_app_free(t);
    return -EINVAL;
  }
  t->ts_refclk = ctx->ipmx_ts_refclk;
  if (ctx->ipmx_ts_refclk[0]) {
    if (access(dir, W_OK) < 0) {
      err("%s(%s), dir %s not writable: %s\n", __func__, name, dir, strerror(errno));
      st_app_free(t);
      return -EIO;
    }
    info("%s(%s), %s waits for the first Sender Report with ts-refclk %s\n", __func__,
         name, t->path, ctx->ipmx_ts_refclk);
  } else {
    ret = app_ipmx_sdp_write(t, sdp, len);
    if (ret < 0) {
      st_app_free(t);
      return ret;
    }
  }
  *ipmx = t;
  return 0;
}

int app_ipmx_tx_st20p_init(struct st_app_context* ctx, st_json_st20p_session_t* st20p,
                           struct st_app_tx_st20p_session* s, const char* name) {
  char sdp[IPMX_SDP_MAX];
  int ret;

  if (!st20p || !st20p->ipmx) return 0;

  ret = ipmx_st20p_tx_set_block_version(s->handle, st20p->ipmx_block_version);
  if (ret < 0) {
    err("%s(%s), set block version fail %d\n", __func__, name, ret);
    return ret;
  }
  if (ctx->ipmx_ts_refclk[0]) {
    ret = ipmx_st20p_tx_set_ts_refclk(s->handle, ctx->ipmx_ts_refclk);
    if (ret < 0) {
      err("%s(%s), set ts_refclk %s fail %d\n", __func__, name, ctx->ipmx_ts_refclk, ret);
      return ret;
    }
  }
  ret = ipmx_st20p_tx_get_sdp(s->handle, sdp, sizeof(sdp));
  if (ret < 0) {
    err("%s(%s), get sdp fail %d\n", __func__, name, ret);
    return ret;
  }
  return app_ipmx_tx_open(ctx, &s->ipmx, name, sdp, ret);
}

int app_ipmx_tx_st22p_init(struct st_app_context* ctx, st_json_st22p_session_t* st22p,
                           struct st_app_tx_st22p_session* s, const char* name) {
  char sdp[IPMX_SDP_MAX];
  int ret;

  if (!st22p || !st22p->ipmx) return 0;

  ret = ipmx_st22p_tx_set_block_version(s->handle, st22p->ipmx_block_version);
  if (ret < 0) {
    err("%s(%s), set block version fail %d\n", __func__, name, ret);
    return ret;
  }
  if (ctx->ipmx_ts_refclk[0]) {
    ret = ipmx_st22p_tx_set_ts_refclk(s->handle, ctx->ipmx_ts_refclk);
    if (ret < 0) {
      err("%s(%s), set ts_refclk %s fail %d\n", __func__, name, ctx->ipmx_ts_refclk, ret);
      return ret;
    }
  }
  ret = ipmx_st22p_tx_get_sdp(s->handle, sdp, sizeof(sdp));
  if (ret < 0) {
    err("%s(%s), get sdp fail %d\n", __func__, name, ret);
    return ret;
  }
  return app_ipmx_tx_open(ctx, &s->ipmx, name, sdp, ret);
}

void app_ipmx_tx_uinit(struct app_ipmx_tx** ipmx) {
  /* without --ipmx_ts_refclk create fails unless the first write worked */
  if (*ipmx && !(*ipmx)->sdp[0])
    warn("%s, no SDP published, ts-refclk %s not in the Sender Reports or write failed\n",
         (*ipmx)->path, (*ipmx)->ts_refclk);
  st_app_free(*ipmx);
  *ipmx = NULL;
}

/* true if the SDP advertises exactly the --ipmx_ts_refclk string, or there is none */
static bool app_ipmx_sdp_has_refclk(const char* sdp, const char* ts_refclk) {
  char attr[96];
  const char* at;

  if (!ts_refclk[0]) return true;
  snprintf(attr, sizeof(attr), "a=ts-refclk:%s", ts_refclk);
  at = strstr(sdp, attr);
  return at && (at[strlen(attr)] == '\r' || at[strlen(attr)] == '\n');
}

static void app_ipmx_tx_refresh(struct st_app_context* ctx, struct app_ipmx_tx* t,
                                const char* sdp, int len) {
  if (len < 0) {
    if (!t->sdp_err) err("%s, %s, get sdp fail %d\n", __func__, t->path, len);
    t->sdp_err = true;
    return;
  }
  t->sdp_err = false;
  if (!app_ipmx_sdp_has_refclk(sdp, ctx->ipmx_ts_refclk)) return;
  if (strcmp(sdp, t->sdp)) app_ipmx_sdp_write(t, sdp, len);
}

void app_ipmx_tx_poll(struct st_app_context* ctx) {
  char sdp[IPMX_SDP_MAX];

  for (int i = 0; i < ctx->tx_st20p_session_cnt; i++) {
    struct st_app_tx_st20p_session* s = &ctx->tx_st20p_sessions[i];
    if (!s->ipmx) continue;
    app_ipmx_tx_refresh(ctx, s->ipmx, sdp,
                        ipmx_st20p_tx_get_sdp(s->handle, sdp, sizeof(sdp)));
  }
  for (int i = 0; i < ctx->tx_st22p_session_cnt; i++) {
    struct st_app_tx_st22p_session* s = &ctx->tx_st22p_sessions[i];
    if (!s->ipmx) continue;
    app_ipmx_tx_refresh(ctx, s->ipmx, sdp,
                        ipmx_st22p_tx_get_sdp(s->handle, sdp, sizeof(sdp)));
  }
}

/* -EBUSY (no Sender Report yet) still fills the counters, so it prints as FAILED */
static void app_ipmx_rx_sr_line(const char* tag, int idx, int port,
                                struct ipmx_rx_sr_info* sr, bool need_jxs_mib) {
  bool ok = sr->received > 0 && !sr->malformed && !sr->info_malformed &&
            sr->has_video_mib && (!need_jxs_mib || sr->has_jxs_mib);

  notce("%s(%d), ipmx sr port %d, received %" PRIu64 " malformed %" PRIu64
        " info_malformed %" PRIu64 " no_info_block %" PRIu64 " version_changes %" PRIu64
        ", version %u, ts_refclk %.*s, cname %s, %s\n",
        tag, idx, port, sr->received, sr->malformed, sr->info_malformed,
        sr->no_info_block, sr->version_changes, sr->info.version,
        (int)sizeof(sr->info.ts_refclk), sr->info.ts_refclk, sr->cname,
        ok ? "OK" : "FAILED");
}

void app_ipmx_rx_st20p_result(struct st_app_rx_st20p_session* s) {
  struct ipmx_rx_sr_info sr;
  struct ipmx_rx_tp_stat tp;
  int ret;

  if (!s->ipmx) return;

  for (int port = 0; port < s->num_port; port++) {
    memset(&sr, 0, sizeof(sr));
    ret = ipmx_st20p_rx_get_sr_info(s->handle, port, &sr);
    if (ret < 0 && ret != -EBUSY)
      err("%s(%d), port %d, get sr info fail %d\n", __func__, s->idx, port, ret);
    else
      app_ipmx_rx_sr_line(__func__, s->idx, port, &sr, false);

    if (!s->ctx->enable_timing_parser) continue;
    ret = ipmx_st20p_rx_get_tp_stat(s->handle, port, &tp);
    if (ret == -EBUSY) {
      info("%s(%d), ipmx tp port %d, timing parser not started\n", __func__, s->idx,
           port);
      continue;
    }
    if (ret < 0) {
      err("%s(%d), port %d, get ipmx tp stat fail %d\n", __func__, s->idx, port, ret);
      continue;
    }
    notce(
        "%s(%d), ipmx tp port %d, frames compliant %u failed %u unmeasured %u, "
        "cinst_max %d cmax %d, vrx_min %d vrx_max %d vrx_full %d, failed_cause %.*s\n",
        __func__, s->idx, port, tp.frames_compliant, tp.frames_failed,
        tp.frames_unmeasured, tp.cinst_max, tp.cmax, tp.vrx_min, tp.vrx_max, tp.vrx_full,
        (int)sizeof(tp.failed_cause), tp.failed_cause);
  }
}

void app_ipmx_rx_st22p_result(struct st_app_rx_st22p_session* s) {
  struct ipmx_rx_sr_info sr;
  int ret;

  if (!s->ipmx) return;

  for (int port = 0; port < s->num_port; port++) {
    memset(&sr, 0, sizeof(sr));
    ret = ipmx_st22p_rx_get_sr_info(s->handle, port, &sr);
    if (ret < 0 && ret != -EBUSY)
      err("%s(%d), port %d, get sr info fail %d\n", __func__, s->idx, port, ret);
    else
      app_ipmx_rx_sr_line(__func__, s->idx, port, &sr, true);
  }
}
