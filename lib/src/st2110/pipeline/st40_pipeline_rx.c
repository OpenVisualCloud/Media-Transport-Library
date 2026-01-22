/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include "st40_pipeline_rx.h"

#include "../../mt_log.h"
#include "../../mt_stat.h"

static const char* st40p_rx_frame_stat_name[ST40P_RX_FRAME_STATUS_MAX] = {
    "free",
    "receiving",
    "ready",
    "in_user",
};

static const char* rx_st40p_stat_name(enum st40p_rx_frame_status stat) {
  return st40p_rx_frame_stat_name[stat];
}

static uint16_t rx_st40p_next_idx(struct st40p_rx_ctx* ctx, uint16_t idx) {
  /* point to next */
  uint16_t next_idx = idx;
  next_idx++;
  if (next_idx >= ctx->framebuff_cnt) next_idx = 0;
  return next_idx;
}

static void rx_st40p_block_wake(struct st40p_rx_ctx* ctx) {
  /* notify block */
  mt_pthread_mutex_lock(&ctx->block_wake_mutex);
  ctx->block_wake_pending = true;
  mt_pthread_cond_signal(&ctx->block_wake_cond);
  mt_pthread_mutex_unlock(&ctx->block_wake_mutex);
}

static void rx_st40p_notify_frame_available(struct st40p_rx_ctx* ctx) {
  if (ctx->ops.notify_frame_available) { /* notify app */
    ctx->ops.notify_frame_available(ctx->ops.priv);
  }

  if (ctx->block_get) {
    /* notify block */
    rx_st40p_block_wake(ctx);
  }
}

static struct st40p_rx_frame* rx_st40p_next_available(
    struct st40p_rx_ctx* ctx, uint16_t idx_start, enum st40p_rx_frame_status desired) {
  uint16_t idx = idx_start;
  struct st40p_rx_frame* framebuff;

  /* check ready frame from idx_start */
  while (1) {
    framebuff = &ctx->framebuffs[idx];
    if (desired == framebuff->stat) {
      /* find one desired */
      return framebuff;
    }
    idx = rx_st40p_next_idx(ctx, idx);
    if (idx == idx_start) {
      /* loop all frames end */
      break;
    }
  }

  /* no any desired frame */
  return NULL;
}

static int rx_st40p_rtp_ready(void* priv) {
  struct st40p_rx_ctx* ctx = priv;
  struct st40p_rx_frame* framebuff;
  void* mbuf;
  void* usrptr = NULL;
  uint16_t len = 0;
  struct st40_rfc8331_rtp_hdr* hdr;
  struct st40_frame_info* frame_info = NULL;
  uint32_t anc_count;
  uint8_t* payload;
  struct st40_rfc8331_payload_hdr* payload_hdr;
  uint32_t payload_offset = 0;
  bool notify_frame = false;
  struct st40p_rx_frame* done_frames[2] = {0};
  struct st40_frame_info* done_infos[2] = {0};
  int done_count = 0;
  int ret = 0;

  if (!ctx->ready) return -EBUSY; /* not ready */

  /* get mbuf from transport */
  mbuf = st40_rx_get_mbuf(ctx->transport, &usrptr, &len);
  if (!mbuf) return -EBUSY;

  struct rte_mbuf* pkt = mbuf;
  uint16_t pkt_port_id = pkt->port;
  int s_port = -1;
  enum mtl_port phy_port = MTL_PORT_MAX;
  for (int i = 0; i < MTL_SESSION_PORT_MAX; i++) {
    if (pkt_port_id == ctx->port_id[i]) {
      s_port = i;
      phy_port = ctx->port_map[i];
      break;
    }
  }
  if (s_port < 0 || phy_port >= MTL_PORT_MAX) {
    warn("%s(%d), drop pkt: unmapped port_id %u\n", __func__, ctx->idx, pkt_port_id);
    st40_rx_put_mbuf(ctx->transport, mbuf);
    return -EIO;
  }
  uint64_t receive_timestamp = mt_mbuf_time_stamp(ctx->impl, pkt, phy_port);

  uint32_t hdr_bytes = sizeof(struct st40_rfc8331_rtp_hdr);
  if (len < hdr_bytes) {
    warn("%s(%d), RTP packet too small (%u < %u)\n", __func__, ctx->idx, len, hdr_bytes);
    st40_rx_put_mbuf(ctx->transport, mbuf);
    return -EIO;
  }

  hdr = (struct st40_rfc8331_rtp_hdr*)usrptr;
  anc_count = hdr->first_hdr_chunk.anc_count;
  uint32_t rtp_timestamp = ntohl(hdr->base.tmstamp);
  uint16_t seq_number = ntohs(hdr->base.seq_number);

  mt_pthread_mutex_lock(&ctx->lock);

  /* complete previous frame if timestamp advanced */
  if (ctx->inflight_frame && ctx->inflight_rtp_timestamp != rtp_timestamp) {
    ctx->inflight_frame->stat = ST40P_RX_FRAME_READY;
    ctx->framebuff_producer_idx = rx_st40p_next_idx(ctx, ctx->inflight_frame->idx);
    notify_frame = true;
    done_frames[done_count] = ctx->inflight_frame;
    done_infos[done_count] = &ctx->inflight_frame->frame_info;
    done_count++;
    ctx->inflight_frame = NULL;
  }

  if (!ctx->inflight_frame) {
    framebuff =
        rx_st40p_next_available(ctx, ctx->framebuff_producer_idx, ST40P_RX_FRAME_FREE);

    if (!framebuff) {
      ctx->stat_busy++;
      ret = -EBUSY;
      goto out;
    }

    framebuff->stat = ST40P_RX_FRAME_RECEIVING;
    frame_info = &framebuff->frame_info;
    frame_info->meta_num = 0;
    frame_info->udw_buffer_fill = 0;
    frame_info->pkts_total = 0;
    frame_info->pkts_recv[MTL_SESSION_PORT_P] = 0;
    frame_info->pkts_recv[MTL_SESSION_PORT_R] = 0;
    frame_info->seq_discont = false;
    frame_info->seq_lost = 0;
    frame_info->rtp_marker = false;
    frame_info->receive_timestamp = receive_timestamp;
    frame_info->tfmt = ST10_TIMESTAMP_FMT_MEDIA_CLK;
    frame_info->rtp_timestamp = rtp_timestamp;
    frame_info->timestamp = rtp_timestamp;
    frame_info->epoch = 0;
    ctx->inflight_frame = framebuff;
    ctx->inflight_rtp_timestamp = rtp_timestamp;
  } else {
    framebuff = ctx->inflight_frame;
    frame_info = &framebuff->frame_info;
    if (!frame_info->receive_timestamp ||
        (frame_info->receive_timestamp > receive_timestamp))
      frame_info->receive_timestamp = receive_timestamp;
  }

  if (ctx->last_seq_valid[s_port]) {
    uint16_t expected = (uint16_t)(ctx->last_seq[s_port] + 1);
    if (expected != seq_number) {
      frame_info->seq_discont = true;
      if (mt_seq16_greater(seq_number, expected))
        frame_info->seq_lost += (uint16_t)(seq_number - expected);
    }
  }
  ctx->last_seq[s_port] = seq_number;
  ctx->last_seq_valid[s_port] = true;

  frame_info->pkts_total++;
  frame_info->pkts_recv[s_port]++;

  /* parse RTP packet and copy metadata */
  payload = (uint8_t*)(hdr + 1);
  uint32_t payload_room = (len > sizeof(*hdr)) ? (len - sizeof(*hdr)) : 0;

  for (uint32_t anc_idx = 0; anc_idx < anc_count; anc_idx++) {
    if (frame_info->meta_num >= ST40_MAX_META) {
      warn("%s(%d), meta slots exhausted after %u packets\n", __func__, ctx->idx,
           frame_info->meta_num);
      break;
    }

    if (payload_offset + sizeof(struct st40_rfc8331_payload_hdr) > payload_room) {
      warn("%s(%d), payload offset exceeds RTP payload (offset=%u, room=%u)\n", __func__,
           ctx->idx, payload_offset, payload_room);
      break;
    }

    payload_hdr = (struct st40_rfc8331_payload_hdr*)(payload + payload_offset);

    struct st40_rfc8331_payload_hdr hdr_local = {0};
    hdr_local.swapped_first_hdr_chunk = ntohl(payload_hdr->swapped_first_hdr_chunk);
    hdr_local.swapped_second_hdr_chunk = ntohl(payload_hdr->swapped_second_hdr_chunk);

    uint16_t udw_words = hdr_local.second_hdr_chunk.data_count & 0xFF;
    struct st40_meta* meta_entry = &frame_info->meta[frame_info->meta_num];
    meta_entry->c = hdr_local.first_hdr_chunk.c;
    meta_entry->line_number = hdr_local.first_hdr_chunk.line_number;
    meta_entry->hori_offset = hdr_local.first_hdr_chunk.horizontal_offset;
    meta_entry->s = hdr_local.first_hdr_chunk.s;
    meta_entry->stream_num = hdr_local.first_hdr_chunk.stream_num;
    meta_entry->did = hdr_local.second_hdr_chunk.did & 0xFF;
    meta_entry->sdid = hdr_local.second_hdr_chunk.sdid & 0xFF;
    meta_entry->udw_size = udw_words;
    meta_entry->udw_offset = frame_info->udw_buffer_fill;

    uint32_t total_bits = (3 + udw_words + 1) * 10;
    /* Match TX padding: floor to bytes then pad to the next 4-byte multiple */
    uint32_t total_size = total_bits / 8;
    uint32_t total_size_aligned = (total_size + 3) & ~0x3U;
    uint32_t anc_packet_bytes =
        sizeof(struct st40_rfc8331_payload_hdr) - 4 + total_size_aligned;
    if (payload_offset + anc_packet_bytes > payload_room) {
      warn("%s(%d), ANC packet bytes exceed payload (offset=%u, size=%u, room=%u)\n",
           __func__, ctx->idx, payload_offset, anc_packet_bytes, payload_room);
      break;
    }

    /* If this is an empty ANC frame (udw_words == 0), still preserve and count it */
    bool meta_valid = true;
    if (udw_words == 0) {
      /* Accept and preserve empty ANC frame as valid */
      meta_valid = true;
    } else {
      /* Parse and validate UDW as before */
      uint8_t* udw_src = (uint8_t*)&payload_hdr->second_hdr_chunk;
      uint32_t original_fill = frame_info->udw_buffer_fill;
      for (uint16_t udw_idx = 0; udw_idx < udw_words; udw_idx++) {
        uint16_t udw = st40_get_udw(udw_idx + 3, udw_src);
        if (!st40_check_parity_bits(udw)) {
          warn("%s(%d), UDW parity failure packet %u word %u\n", __func__, ctx->idx,
               anc_idx, udw_idx);
          meta_valid = false;
          break;
        }
        if (frame_info->udw_buffer_fill >= frame_info->udw_buffer_size) {
          warn("%s(%d), UDW buffer overflow for packet %u\n", __func__, ctx->idx,
               anc_idx);
          meta_valid = false;
          break;
        }
        frame_info->udw_buff_addr[frame_info->udw_buffer_fill++] = (uint8_t)(udw & 0xFF);
      }
      if (meta_valid) {
        uint16_t checksum_udw = st40_get_udw(udw_words + 3, udw_src);
        uint16_t checksum_calc = st40_calc_checksum(3 + udw_words, udw_src);
        if (checksum_udw != checksum_calc) {
          warn("%s(%d), checksum mismatch packet %u (0x%03x != 0x%03x)\n", __func__,
               ctx->idx, anc_idx, checksum_udw, checksum_calc);
          meta_valid = false;
        }
      }
      if (!meta_valid) {
        frame_info->udw_buffer_fill = original_fill;
        break;
      }
    }

    frame_info->meta_num++;
    payload_offset += anc_packet_bytes;
  }

  if (hdr->base.marker) {
    frame_info->rtp_marker = true;
    framebuff->stat = ST40P_RX_FRAME_READY;
    ctx->framebuff_producer_idx = rx_st40p_next_idx(ctx, framebuff->idx);
    ctx->inflight_frame = NULL;
    notify_frame = true;
    done_frames[done_count] = framebuff;
    done_infos[done_count] = frame_info;
    done_count++;
  }

out:
  mt_pthread_mutex_unlock(&ctx->lock);

  /* return mbuf to transport */
  st40_rx_put_mbuf(ctx->transport, mbuf);

  if (notify_frame) {
    for (int n = 0; n < done_count; n++) {
      struct st40p_rx_frame* report_frame = done_frames[n];
      struct st40_frame_info* report_info = done_infos[n];
      if (!report_frame || !report_info) continue;
      dbg("%s(%d), frame %u succ, meta_num %u\n", __func__, ctx->idx, report_frame->idx,
          report_info->meta_num);
      /* notify app to a ready frame */
      rx_st40p_notify_frame_available(ctx);
      MT_USDT_ST40P_RX_FRAME_AVAILABLE(ctx->idx, report_frame->idx,
                                       report_info->meta_num);
    }
  }

  return ret;
}

static int rx_st40p_create_transport(struct mtl_main_impl* impl, struct st40p_rx_ctx* ctx,
                                     struct st40p_rx_ops* ops) {
  int idx = ctx->idx;
  struct st40_rx_ops ops_rx;

  memset(&ops_rx, 0, sizeof(ops_rx));
  ops_rx.name = ops->name;
  ops_rx.priv = ctx;
  ops_rx.num_port = RTE_MIN(ops->port.num_port, MTL_SESSION_PORT_MAX);
  ops_rx.payload_type = ops->port.payload_type;
  ops_rx.ssrc = ops->port.ssrc;
  ops_rx.interlaced = ops->interlaced;

  for (int i = 0; i < ops_rx.num_port; i++) {
    memcpy(ops_rx.ip_addr[i], ops->port.ip_addr[i], MTL_IP_ADDR_LEN);
    memcpy(ops_rx.mcast_sip_addr[i], ops->port.mcast_sip_addr[i], MTL_IP_ADDR_LEN);
    snprintf(ops_rx.port[i], MTL_PORT_MAX_LEN, "%s", ops->port.port[i]);
    ops_rx.udp_port[i] = ops->port.udp_port[i];

    enum mtl_port phy = mt_port_by_name(impl, ops->port.port[i]);
    ctx->port_map[i] = phy;
    ctx->port_id[i] = mt_port_id(impl, phy);
  }

  ops_rx.rtp_ring_size = ops->rtp_ring_size;
  ops_rx.notify_rtp_ready = rx_st40p_rtp_ready;

  if (ops->flags & ST40P_RX_FLAG_DATA_PATH_ONLY)
    ops_rx.flags |= ST40_RX_FLAG_DATA_PATH_ONLY;
  if (ops->flags & ST40P_RX_FLAG_ENABLE_RTCP) ops_rx.flags |= ST40_RX_FLAG_ENABLE_RTCP;

  ctx->transport = st40_rx_create(impl, &ops_rx);
  if (!ctx->transport) {
    err("%s(%d), Failed to create transport\n", __func__, idx);
    return -EIO;
  }

  return 0;
}

static int rx_st40p_uinit_fbs(struct st40p_rx_ctx* ctx) {
  if (!ctx->framebuffs) return 0;

  for (uint16_t i = 0; i < ctx->framebuff_cnt; i++) {
    if (ctx->framebuffs[i].frame_info.udw_buff_addr) {
      mt_rte_free(ctx->framebuffs[i].frame_info.udw_buff_addr);
      ctx->framebuffs[i].frame_info.udw_buff_addr = NULL;
    }
  }

  mt_rte_free(ctx->framebuffs);
  ctx->framebuffs = NULL;

  return 0;
}

static int rx_st40p_init_fbs(struct st40p_rx_ctx* ctx, struct st40p_rx_ops* ops) {
  int idx = ctx->idx;
  int soc_id = ctx->socket_id;
  struct st40p_rx_frame *frames, *framebuff;
  struct st40_frame_info* frame_info;

  if (!ops->max_udw_buff_size) {
    err("%s(%d), invalid max_udw_buff_size %u\n", __func__, idx, ops->max_udw_buff_size);
    return -EINVAL;
  }

  frames = mt_rte_zmalloc_socket(sizeof(*frames) * ctx->framebuff_cnt, soc_id);
  if (!frames) {
    err("%s(%d), frames malloc failed\n", __func__, idx);
    return -ENOMEM;
  }
  ctx->framebuffs = frames;

  for (uint16_t i = 0; i < ctx->framebuff_cnt; i++) {
    framebuff = &frames[i];
    frame_info = &framebuff->frame_info;
    framebuff->stat = ST40P_RX_FRAME_FREE;
    framebuff->idx = i;

    frame_info->udw_buff_addr = mt_rte_zmalloc_socket(ops->max_udw_buff_size, soc_id);
    if (!frame_info->udw_buff_addr) {
      err("%s(%d), udw_buff malloc failed for frame %u\n", __func__, idx, i);
      rx_st40p_uinit_fbs(ctx);
      return -ENOMEM;
    }
    frame_info->udw_buffer_size = ops->max_udw_buff_size;
    frame_info->udw_buffer_fill = 0;
    frame_info->meta_num = 0;
    frame_info->meta = framebuff->meta;
    frame_info->pkts_total = 0;
    frame_info->pkts_recv[MTL_SESSION_PORT_P] = 0;
    frame_info->pkts_recv[MTL_SESSION_PORT_R] = 0;
    frame_info->seq_discont = false;
    frame_info->seq_lost = 0;
    frame_info->rtp_marker = false;
    frame_info->receive_timestamp = 0;
    frame_info->priv = framebuff;

    dbg("%s(%d), init fb %u\n", __func__, idx, i);
  }

  info("%s(%d), max_udw_buff_size %u with %u frames\n", __func__, idx,
       ops->max_udw_buff_size, ctx->framebuff_cnt);

  return 0;
}

static int rx_st40p_stat(void* priv) {
  struct st40p_rx_ctx* ctx = priv;
  struct st40p_rx_frame* framebuff = ctx->framebuffs;

  if (!ctx->ready) return -EBUSY; /* not ready */

  uint16_t producer_idx;
  uint16_t consumer_idx;
  enum st40p_rx_frame_status producer_stat;
  enum st40p_rx_frame_status consumer_stat;

  mt_pthread_mutex_lock(&ctx->lock);
  producer_idx = ctx->framebuff_producer_idx;
  consumer_idx = ctx->framebuff_consumer_idx;
  producer_stat = framebuff[producer_idx].stat;
  consumer_stat = framebuff[consumer_idx].stat;
  mt_pthread_mutex_unlock(&ctx->lock);

  notice("RX_st40p(%d,%s), p(%d:%s) c(%d:%s)\n", ctx->idx, ctx->ops_name, producer_idx,
         rx_st40p_stat_name(producer_stat), consumer_idx,
         rx_st40p_stat_name(consumer_stat));

  notice("RX_st40p(%d), frame get try %d succ %d, put %d\n", ctx->idx,
         ctx->stat_get_frame_try, ctx->stat_get_frame_succ, ctx->stat_put_frame);

  ctx->stat_get_frame_try = 0;
  ctx->stat_get_frame_succ = 0;
  ctx->stat_put_frame = 0;

  if (ctx->stat_busy) {
    notice("RX_st40p(%d), busy %d\n", ctx->idx, ctx->stat_busy);
    ctx->stat_busy = 0;
  }

  return 0;
}

static int rx_st40p_get_block_wait(struct st40p_rx_ctx* ctx) {
  dbg("%s(%d), start\n", __func__, ctx->idx);
  /* wait on the block cond */
  mt_pthread_mutex_lock(&ctx->block_wake_mutex);
  while (!ctx->block_wake_pending) {
    int ret = mt_pthread_cond_timedwait_ns(&ctx->block_wake_cond, &ctx->block_wake_mutex,
                                           ctx->block_timeout_ns);
    if (ret) break;
  }
  ctx->block_wake_pending = false;
  mt_pthread_mutex_unlock(&ctx->block_wake_mutex);
  dbg("%s(%d), end\n", __func__, ctx->idx);
  return 0;
}

static int rx_st40p_usdt_dump_frame(struct st40p_rx_ctx* ctx,
                                    struct st40_frame_info* frame_info) {
  int idx = ctx->idx;
  struct mtl_main_impl* impl = ctx->impl;
  int fd;
  char usdt_dump_path[64];
  uint64_t tsc_s = mt_get_tsc(impl);

  snprintf(usdt_dump_path, sizeof(usdt_dump_path), "imtl_usdt_st40prx_s%d_%d_XXXXXX.bin",
           idx, ctx->usdt_dump_frame_cnt);
  fd = mt_mkstemps(usdt_dump_path, strlen(".bin"));
  if (fd < 0) {
    err("%s(%d), mkstemps %s fail %d\n", __func__, idx, usdt_dump_path, fd);
    return fd;
  }

  /* write UDW data to dump file */
  ssize_t n = write(fd, frame_info->udw_buff_addr, frame_info->udw_buffer_fill);
  if (n != frame_info->udw_buffer_fill) {
    warn("%s(%d), write fail %" PRId64 "\n", __func__, idx, (int64_t)n);
  }
  MT_USDT_ST40P_RX_FRAME_DUMP(idx, usdt_dump_path, frame_info->meta_num, n);

  info("%s(%d), write %" PRId64 " to %s(fd:%d), time %fms\n", __func__, idx, (int64_t)n,
       usdt_dump_path, fd, (float)(mt_get_tsc(impl) - tsc_s) / NS_PER_MS);
  ctx->usdt_dump_frame_cnt++;
  close(fd);
  return 0;
}

struct st40_frame_info* st40p_rx_get_frame(st40p_rx_handle handle) {
  struct st40p_rx_ctx* ctx = handle;
  int idx = ctx->idx;
  struct st40p_rx_frame* framebuff;
  struct st40_frame_info* frame_info;

  if (MT_ST40_HANDLE_PIPELINE_RX != ctx->type) {
    err("%s(%d), invalid type %d\n", __func__, idx, ctx->type);
    return NULL;
  }

  if (!ctx->ready) return NULL; /* not ready */

  ctx->stat_get_frame_try++;

  mt_pthread_mutex_lock(&ctx->lock);

  framebuff =
      rx_st40p_next_available(ctx, ctx->framebuff_consumer_idx, ST40P_RX_FRAME_READY);
  if (!framebuff && ctx->block_get) { /* wait here */
    mt_pthread_mutex_unlock(&ctx->lock);
    rx_st40p_get_block_wait(ctx);
    /* get again */
    mt_pthread_mutex_lock(&ctx->lock);
    framebuff =
        rx_st40p_next_available(ctx, ctx->framebuff_consumer_idx, ST40P_RX_FRAME_READY);
  }

  /* not any ready frame */
  if (!framebuff) {
    mt_pthread_mutex_unlock(&ctx->lock);
    return NULL;
  }

  framebuff->stat = ST40P_RX_FRAME_IN_USER;
  /* point to next */
  ctx->framebuff_consumer_idx = rx_st40p_next_idx(ctx, framebuff->idx);
  mt_pthread_mutex_unlock(&ctx->lock);

  frame_info = &framebuff->frame_info;
  ctx->stat_get_frame_succ++;
  MT_USDT_ST40P_RX_FRAME_GET(idx, framebuff->idx, frame_info->meta_num);
  dbg("%s(%d), frame %u succ, meta_num %u\n", __func__, idx, framebuff->idx,
      frame_info->meta_num);

  /* check if dump USDT enabled */
  if (MT_USDT_ST40P_RX_FRAME_DUMP_ENABLED()) {
    rx_st40p_usdt_dump_frame(ctx, frame_info);
  }

  return frame_info;
}

int st40p_rx_put_frame(st40p_rx_handle handle, struct st40_frame_info* frame_info) {
  struct st40p_rx_ctx* ctx = handle;
  int idx = ctx->idx;
  struct st40p_rx_frame* framebuff = frame_info->priv;
  uint16_t consumer_idx = framebuff->idx;
  uint16_t meta_num_before_reset = frame_info->meta_num;

  if (MT_ST40_HANDLE_PIPELINE_RX != ctx->type) {
    err("%s(%d), invalid type %d\n", __func__, idx, ctx->type);
    return -EIO;
  }

  if (ST40P_RX_FRAME_IN_USER != framebuff->stat) {
    err("%s(%d), frame %u not in user %d\n", __func__, idx, consumer_idx,
        framebuff->stat);
    return -EIO;
  }

  /* reset frame for reuse */
  frame_info->meta_num = 0;
  frame_info->udw_buffer_fill = 0;
  frame_info->pkts_total = 0;
  frame_info->pkts_recv[MTL_SESSION_PORT_P] = 0;
  frame_info->pkts_recv[MTL_SESSION_PORT_R] = 0;
  frame_info->seq_discont = false;
  frame_info->seq_lost = 0;
  frame_info->rtp_marker = false;
  frame_info->receive_timestamp = 0;
  framebuff->stat = ST40P_RX_FRAME_FREE;
  ctx->stat_put_frame++;

  MT_USDT_ST40P_RX_FRAME_PUT(idx, consumer_idx, meta_num_before_reset);
  dbg("%s(%d), frame %u succ\n", __func__, idx, consumer_idx);
  return 0;
}

int st40p_rx_free(st40p_rx_handle handle) {
  struct st40p_rx_ctx* ctx = handle;
  struct mtl_main_impl* impl;

  if (!handle) {
    err("%s, invalid handle\n", __func__);
    return -EINVAL;
  }

  impl = ctx->impl;

  if (MT_ST40_HANDLE_PIPELINE_RX != ctx->type) {
    err("%s(%d), invalid type %d\n", __func__, ctx->idx, ctx->type);
    return -EIO;
  }

  notice("%s(%d), start\n", __func__, ctx->idx);

  if (ctx->ready) {
    mt_stat_unregister(impl, rx_st40p_stat, ctx);
  }

  if (ctx->transport) {
    st40_rx_free(ctx->transport);
    ctx->transport = NULL;
  }

  rx_st40p_uinit_fbs(ctx);

  mt_pthread_mutex_destroy(&ctx->lock);
  mt_pthread_mutex_destroy(&ctx->block_wake_mutex);
  mt_pthread_cond_destroy(&ctx->block_wake_cond);
  notice("%s(%d), succ\n", __func__, ctx->idx);
  mt_rte_free(ctx);

  return 0;
}

st40p_rx_handle st40p_rx_create(mtl_handle mt, struct st40p_rx_ops* ops) {
  static int st40p_rx_idx;
  struct mtl_main_impl* impl = mt;
  struct st40p_rx_ctx* ctx;
  int ret;
  int idx = st40p_rx_idx;

  /* validate the input parameters */
  if (!mt || !ops) {
    err("%s, NULL input parameters\n", __func__);
    return NULL;
  }

  notice("%s, start for %s\n", __func__, mt_string_safe(ops->name));

  if (MT_HANDLE_MAIN != impl->type) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return NULL;
  }

  enum mtl_port port = mt_port_by_name(impl, ops->port.port[MTL_SESSION_PORT_P]);
  if (port >= MTL_PORT_MAX) return NULL;
  int socket = mt_socket_id(impl, port);

  if (ops->flags & ST40P_RX_FLAG_FORCE_NUMA) {
    err("%s(%d), force numa not supported\n", __func__, idx);
    return NULL;
  }

  ctx = mt_rte_zmalloc_socket(sizeof(*ctx), socket);
  if (!ctx) {
    err("%s, ctx malloc failed on socket %d\n", __func__, socket);
    return NULL;
  }

  ctx->idx = idx;
  ctx->socket_id = socket;
  ctx->ready = false;
  ctx->impl = impl;
  ctx->type = MT_ST40_HANDLE_PIPELINE_RX;
  for (int i = 0; i < MTL_SESSION_PORT_MAX; i++) ctx->last_seq_valid[i] = false;
  for (int i = 0; i < MTL_SESSION_PORT_MAX; i++) {
    ctx->port_map[i] = MTL_PORT_MAX;
    ctx->port_id[i] = UINT16_MAX;
  }

  mt_pthread_mutex_init(&ctx->lock, NULL);
  mt_pthread_mutex_init(&ctx->block_wake_mutex, NULL);
  mt_pthread_cond_wait_init(&ctx->block_wake_cond);
  ctx->block_timeout_ns = NS_PER_S;
  ctx->block_wake_pending = false;
  if (ops->flags & ST40P_RX_FLAG_BLOCK_GET) ctx->block_get = true;

  /* copy ops */
  if (ops->name) {
    snprintf(ctx->ops_name, sizeof(ctx->ops_name), "%s", ops->name);
  } else {
    snprintf(ctx->ops_name, sizeof(ctx->ops_name), "ST40P_RX_%d", idx);
  }
  ctx->ops = *ops;

  ctx->framebuff_cnt = ops->framebuff_cnt;
  /* init fbs */
  ret = rx_st40p_init_fbs(ctx, ops);
  if (ret < 0) {
    err("%s(%d), init fbs failed %d\n", __func__, idx, ret);
    st40p_rx_free(ctx);
    return NULL;
  }

  /* create transport handle */
  ret = rx_st40p_create_transport(impl, ctx, ops);
  if (ret < 0) {
    err("%s(%d), Failed to create transport\n", __func__, idx);
    st40p_rx_free(ctx);
    return NULL;
  }

  /* all ready now */
  ctx->ready = true;
  notice("%s(%d), flags 0x%x\n", __func__, idx, ops->flags);
  st40p_rx_idx++;

  if (!ctx->block_get) rx_st40p_notify_frame_available(ctx);

  mt_stat_register(impl, rx_st40p_stat, ctx, ctx->ops_name);

  return ctx;
}

size_t st40p_rx_max_udw_buff_size(st40p_rx_handle handle) {
  struct st40p_rx_ctx* ctx = handle;
  int cidx = ctx->idx;

  if (MT_ST40_HANDLE_PIPELINE_RX != ctx->type) {
    err("%s(%d), invalid type %d\n", __func__, cidx, ctx->type);
    return 0;
  }

  return ctx->ops.max_udw_buff_size;
}

int st40p_rx_get_queue_meta(st40p_rx_handle handle, struct st_queue_meta* meta) {
  struct st40p_rx_ctx* ctx = handle;
  int cidx = ctx->idx;

  if (MT_ST40_HANDLE_PIPELINE_RX != ctx->type) {
    err("%s(%d), invalid type %d\n", __func__, cidx, ctx->type);
    return -EIO;
  }

  return st40_rx_get_queue_meta(ctx->transport, meta);
}

int st40p_rx_get_session_stats(st40p_rx_handle handle, struct st40_rx_user_stats* stats) {
  struct st40p_rx_ctx* ctx = handle;
  int cidx;

  if (!handle || !stats) {
    err("%s, invalid handle %p or stats %p\n", __func__, handle, stats);
    return -EINVAL;
  }

  cidx = ctx->idx;
  if (ctx->type != MT_ST40_HANDLE_PIPELINE_RX) {
    err("%s(%d), invalid type %d\n", __func__, cidx, ctx->type);
    return -EIO;
  }

  return st40_rx_get_session_stats(ctx->transport, stats);
}

int st40p_rx_reset_session_stats(st40p_rx_handle handle) {
  struct st40p_rx_ctx* ctx = handle;
  int cidx;

  if (!handle) {
    err("%s, invalid handle %p\n", __func__, handle);
    return -EINVAL;
  }

  cidx = ctx->idx;
  if (ctx->type != MT_ST40_HANDLE_PIPELINE_RX) {
    err("%s(%d), invalid type %d\n", __func__, cidx, ctx->type);
    return -EIO;
  }

  return st40_rx_reset_session_stats(ctx->transport);
}

int st40p_rx_update_source(st40p_rx_handle handle, struct st_rx_source_info* src) {
  struct st40p_rx_ctx* ctx = handle;
  int cidx = ctx->idx;

  if (MT_ST40_HANDLE_PIPELINE_RX != ctx->type) {
    err("%s(%d), invalid type %d\n", __func__, cidx, ctx->type);
    return -EIO;
  }

  return st40_rx_update_source(ctx->transport, src);
}

int st40p_rx_wake_block(st40p_rx_handle handle) {
  struct st40p_rx_ctx* ctx = handle;
  int cidx = ctx->idx;

  if (MT_ST40_HANDLE_PIPELINE_RX != ctx->type) {
    err("%s(%d), invalid type %d\n", __func__, cidx, ctx->type);
    return -EIO;
  }

  if (ctx->block_get) rx_st40p_block_wake(ctx);

  return 0;
}

int st40p_rx_set_block_timeout(st40p_rx_handle handle, uint64_t timedwait_ns) {
  struct st40p_rx_ctx* ctx = handle;
  int cidx = ctx->idx;

  if (MT_ST40_HANDLE_PIPELINE_RX != ctx->type) {
    err("%s(%d), invalid type %d\n", __func__, cidx, ctx->type);
    return -EIO;
  }

  ctx->block_timeout_ns = timedwait_ns;
  return 0;
}

void* st40p_rx_get_udw_buff_addr(st40p_rx_handle handle, uint16_t idx) {
  struct st40p_rx_ctx* ctx = handle;
  int cidx = ctx->idx;

  if (MT_ST40_HANDLE_PIPELINE_RX != ctx->type) {
    err("%s(%d), invalid type %d\n", __func__, cidx, ctx->type);
    return NULL;
  }

  if (idx >= ctx->framebuff_cnt) {
    err("%s, invalid idx %d, should be in range [0, %u]\n", __func__, cidx,
        ctx->framebuff_cnt);
    return NULL;
  }

  return ctx->framebuffs[idx].frame_info.udw_buff_addr;
}
