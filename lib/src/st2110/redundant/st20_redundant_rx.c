/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "st20_redundant_rx.h"

#include "../../mt_log.h"
#include "../st_rx_video_session.h"

static int rx_st20r_frame_pop(struct st20r_rx_ctx* ctx, void* frame) {
  struct st20r_rx_frame* rx_frame;

  for (int i = 0; i < ctx->frames_cnt; i++) {
    rx_frame = &ctx->frames[i];
    if (rx_frame->frame != frame) continue;
    /* find the slot */
    rx_frame->frame = NULL;
    return st20_rx_put_framebuff(ctx->transport[rx_frame->port]->handle, frame);
  }

  err("%s(%d), not known frame %p\n", __func__, ctx->idx, frame);
  return -EIO;
}

static int rx_st20r_frame_push(struct st20r_rx_ctx* ctx, void* frame,
                               enum mtl_session_port port,
                               struct st20_rx_frame_meta* meta) {
  struct st20r_rx_frame* rx_frame;
  int ret;

  for (int i = 0; i < ctx->frames_cnt; i++) {
    rx_frame = &ctx->frames[i];
    if (rx_frame->frame) continue;
    /* find a empty slot, notify user */
    rx_frame->frame = frame;
    rx_frame->port = port;
    rx_frame->meta = *meta;
    ret = ctx->ops.notify_frame_ready(ctx->ops.priv, frame, meta);
    dbg("%s(%d), notify frame %p(%d:%u) to user, ret %d\n", __func__, ctx->idx, frame,
        port, i, ret);
    if (ret < 0) {
      /* clear the frame */
      rx_frame->frame = NULL;
    }
    return ret;
  }

  err("%s(%d), no space\n", __func__, ctx->idx);
  return -EIO;
}

static int rx_st20r_frame_ready(void* priv, void* frame,
                                struct st20_rx_frame_meta* meta) {
  struct st20r_rx_transport* transport = priv;
  struct st20r_rx_ctx* ctx = transport->parent;
  int idx = ctx->idx;
  enum mtl_session_port port = transport->port;
  int ret = -EIO;

  if (!ctx->ready) return -EBUSY; /* not ready */

  dbg("%s(%d), get frame %p at port %d\n", __func__, idx, frame, port);

  mt_pthread_mutex_lock(&ctx->lock);
  /* assume p and r has same timestamp */
  if (ctx->cur_timestamp != meta->timestamp) {
    /* new timestamp */
    ctx->cur_timestamp = meta->timestamp;
    ctx->cur_frame_complete = false;
    if (st_is_frame_complete(meta->status)) {
      /* full frame get */
      ret = rx_st20r_frame_push(ctx, frame, port, meta);
      if (ret >= 0) {
        ctx->cur_frame_complete = true;
        dbg("%s(%d), push frame %p at port %d\n", __func__, idx, frame, port);
      }
    } else {
      ret = -EIO; /* simply drop now, todo later to recover the full frame */
    }
  } else {
    if (st_is_frame_complete(meta->status)) {
      if (!ctx->cur_frame_complete) {
        /* full frame get at r session */
        ret = rx_st20r_frame_push(ctx, frame, port, meta);
        if (ret >= 0) {
          ctx->cur_frame_complete = true;
          info("%s(%d), push frame %p at r_port %d\n", __func__, idx, frame, port);
        }
      } else {
        ret = -EIO; /* simply drop as frame get already */
      }
    } else {
      ret = -EIO; /* simply drop now, todo later to recover the full frame */
    }
  }
  mt_pthread_mutex_unlock(&ctx->lock);

  /* always return 0 to suppress the error log */
  if (ret < 0) st20_rx_put_framebuff(ctx->transport[port]->handle, frame);
  return 0;
}

static int rx_st20r_notify_event(void* priv, enum st_event event, void* args) {
  struct st20r_rx_ctx* ctx = priv;

  if (ctx->ops.notify_event) {
    ctx->ops.notify_event(ctx->ops.priv, event, args);
  }

  return 0;
}

static int rx_st20r_free_transport(struct st20r_rx_transport* transport) {
  if (transport->handle) {
    st20_rx_free(transport->handle);
    transport->handle = NULL;
  }

  mt_rte_free(transport);
  return 0;
}

static int rx_st20r_create_transport(struct st20r_rx_ctx* ctx, struct st20r_rx_ops* ops,
                                     enum mtl_session_port port) {
  int idx = ctx->idx;
  struct mtl_main_impl* impl = ctx->impl;
  struct st20r_rx_transport* transport;
  struct st20_rx_ops ops_rx;

  if (ctx->transport[port]) {
    err("%s(%d), exist transport for port %d\n", __func__, idx, port);
    return -EIO;
  }

  transport = mt_rte_zmalloc_socket(sizeof(*transport), mt_socket_id(impl, MTL_PORT_P));
  if (!transport) {
    err("%s, transport malloc fail\n", __func__);
    return -ENOMEM;
  }

  transport->port = port;
  transport->parent = ctx;

  memset(&ops_rx, 0, sizeof(ops_rx));
  ops_rx.name = ops->name;
  ops_rx.priv = transport;
  ops_rx.num_port = 1;
  memcpy(ops_rx.sip_addr[MTL_SESSION_PORT_P], ops->sip_addr[port], MTL_IP_ADDR_LEN);
  strncpy(ops_rx.port[MTL_SESSION_PORT_P], ops->port[port], MTL_PORT_MAX_LEN - 1);
  ops_rx.udp_src_port[MTL_SESSION_PORT_P] = ops->udp_src_port[port];
  ops_rx.udp_port[MTL_SESSION_PORT_P] = ops->udp_port[port];

  if (ops->flags & ST20R_RX_FLAG_DATA_PATH_ONLY)
    ops_rx.flags |= ST20_RX_FLAG_DATA_PATH_ONLY;
  /* always enable incomplete frame */
  ops_rx.flags |= ST20_RX_FLAG_RECEIVE_INCOMPLETE_FRAME;
  /* disable migrate since it may migrate the two sessions into one sch */
  ops_rx.flags |= ST20_RX_FLAG_DISABLE_MIGRATE;
  if (ops->flags & ST20R_RX_FLAG_DMA_OFFLOAD) ops_rx.flags |= ST20_RX_FLAG_DMA_OFFLOAD;
  if (ops->flags & ST20R_RX_FLAG_HDR_SPLIT) ops_rx.flags |= ST20_RX_FLAG_HDR_SPLIT;
  if (ops->flags & ST20R_RX_FLAG_ENABLE_VSYNC) ops_rx.flags |= ST20_RX_FLAG_ENABLE_VSYNC;

  ops_rx.pacing = ops->pacing;
  ops_rx.width = ops->width;
  ops_rx.height = ops->height;
  ops_rx.fps = ops->fps;
  ops_rx.fmt = ops->fmt;
  ops_rx.interlaced = ops->interlaced;
  ops_rx.payload_type = ops->payload_type;
  ops_rx.type = ST20_TYPE_FRAME_LEVEL;
  ops_rx.framebuff_cnt = ops->framebuff_cnt;
  ops_rx.notify_frame_ready = rx_st20r_frame_ready;
  if (port == MTL_SESSION_PORT_P) /* only register vsync to p port now */
    ops_rx.notify_event = rx_st20r_notify_event;

  mt_sch_mask_t sch_mask = MT_SCH_MASK_ALL;
  if (port == MTL_SESSION_PORT_R) {
    /* let R port select a different sch */
    sch_mask &=
        ~(MTL_BIT64(st20_rx_get_sch_idx(ctx->transport[MTL_SESSION_PORT_P]->handle)));
  }
  dbg("%s(%d,%d), sch_mask %" PRIx64 "\n", __func__, idx, port, sch_mask);
  transport->handle = st20_rx_create_with_mask(impl, &ops_rx, sch_mask);
  if (!transport->handle) {
    err("%s(%d), transport create fail on port %d\n", __func__, idx, port);
    rx_st20r_free_transport(transport);
    return -EIO;
  }

  ctx->transport[port] = transport;
  info("%s(%d,%d), succ on sch %d\n", __func__, idx, port,
       st20_rx_get_sch_idx(transport->handle));
  return 0;
}

int st20r_rx_free(st20r_rx_handle handle) {
  struct st20r_rx_ctx* ctx = handle;

  if (ctx->type != MT_HANDLE_RX_VIDEO_R) {
    err("%s(%d), invalid type %d\n", __func__, ctx->idx, ctx->type);
    return -EIO;
  }

  ctx->ready = false;

  for (int i = 0; i < MTL_SESSION_PORT_MAX; i++) {
    if (ctx->transport[i]) {
      rx_st20r_free_transport(ctx->transport[i]);
      ctx->transport[i] = NULL;
    }
  }

  mt_pthread_mutex_destroy(&ctx->lock);
  if (ctx->frames) {
    mt_rte_free(ctx->frames);
    ctx->frames = NULL;
  }
  mt_rte_free(ctx);

  return 0;
}

st20r_rx_handle st20r_rx_create(mtl_handle mt, struct st20r_rx_ops* ops) {
  struct mtl_main_impl* impl = mt;
  struct st20r_rx_ctx* ctx;
  int ret;
  int idx = 0; /* todo */
  int num_port = ops->num_port;

  if (impl->type != MT_HANDLE_MAIN) {
    err("%s, invalid st type %d\n", __func__, impl->type);
    return NULL;
  }
  if (num_port != MTL_SESSION_PORT_MAX) {
    err("%s, invalid num_port %u\n", __func__, num_port);
    return NULL;
  }
  if (0 == memcmp(ops->sip_addr[MTL_SESSION_PORT_P], ops->sip_addr[MTL_SESSION_PORT_R],
                  MTL_IP_ADDR_LEN)) {
    uint8_t* ip = ops->sip_addr[MTL_SESSION_PORT_P];
    err("%s, same %d.%d.%d.%d for both ip\n", __func__, ip[0], ip[1], ip[2], ip[3]);
    return NULL;
  }
  if (!ops->notify_frame_ready) {
    err("%s, pls set notify_frame_ready\n", __func__);
    return NULL;
  }

  ctx = mt_rte_zmalloc_socket(sizeof(*ctx), mt_socket_id(impl, MTL_PORT_P));
  if (!ctx) {
    err("%s, ctx malloc fail\n", __func__);
    return NULL;
  }

  ctx->frames_cnt = ops->framebuff_cnt * 2; /* more for redundant */
  ctx->frames = mt_rte_zmalloc_socket(sizeof(*ctx->frames) * ctx->frames_cnt,
                                      mt_socket_id(impl, MTL_PORT_P));
  if (!ctx->frames) {
    err("%s, ctx frames malloc fail\n", __func__);
    st20r_rx_free(ctx);
    return NULL;
  }

  ctx->idx = idx;
  ctx->impl = impl;
  ctx->type = MT_HANDLE_RX_VIDEO_R;
  mt_pthread_mutex_init(&ctx->lock, NULL);

  /* copy ops */
  strncpy(ctx->ops_name, ops->name, ST_MAX_NAME_LEN - 1);
  ctx->ops = *ops;

  /* crete transport handle */
  for (int i = 0; i < num_port; i++) {
    ret = rx_st20r_create_transport(ctx, ops, i);
    if (ret < 0) {
      err("%s(%d), create transport fail\n", __func__, idx);
      st20r_rx_free(ctx);
      return NULL;
    }
  }

  ctx->ready = true;
  return ctx;
}

int st20r_rx_put_frame(st20r_rx_handle handle, void* frame) {
  struct st20r_rx_ctx* ctx = handle;

  if (ctx->type != MT_HANDLE_RX_VIDEO_R) {
    err("%s(%d), invalid type %d\n", __func__, ctx->idx, ctx->type);
    return -EIO;
  }

  return rx_st20r_frame_pop(ctx, frame);
}

size_t st20r_rx_get_framebuffer_size(st20r_rx_handle handle) {
  struct st20r_rx_ctx* ctx = handle;

  if (ctx->type != MT_HANDLE_RX_VIDEO_R) {
    err("%s(%d), invalid type %d\n", __func__, ctx->idx, ctx->type);
    return -EIO;
  }

  return st20_rx_get_framebuffer_size(ctx->transport[MTL_SESSION_PORT_P]->handle);
}

int st20r_rx_get_framebuffer_count(st20r_rx_handle handle) {
  struct st20r_rx_ctx* ctx = handle;

  if (ctx->type != MT_HANDLE_RX_VIDEO_R) {
    err("%s(%d), invalid type %d\n", __func__, ctx->idx, ctx->type);
    return -EIO;
  }

  return st20_rx_get_framebuffer_count(ctx->transport[MTL_SESSION_PORT_P]->handle);
}

int st20r_rx_pcapng_dump(st20r_rx_handle handle, uint32_t max_dump_packets, bool sync,
                         struct st_pcap_dump_meta* meta) {
  struct st20r_rx_ctx* ctx = handle;
  int ret = 0;

  if (ctx->type != MT_HANDLE_RX_VIDEO_R) {
    err("%s(%d), invalid type %d\n", __func__, ctx->idx, ctx->type);
    return -EIO;
  }

  ret += st20_rx_pcapng_dump(ctx->transport[MTL_SESSION_PORT_P]->handle, max_dump_packets,
                             sync, meta);
  ret += st20_rx_pcapng_dump(ctx->transport[MTL_SESSION_PORT_R]->handle, max_dump_packets,
                             sync, meta);
  return ret;
}
