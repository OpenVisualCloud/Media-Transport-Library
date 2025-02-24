/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#include "mt_rtcp.h"

#include "datapath/mt_queue.h"
#include "mt_log.h"
#include "mt_stat.h"
#include "mt_util.h"

#define UINT16_MAX_HALF (1 << 15)

static int rtp_seq_num_cmp(uint16_t seq0, uint16_t seq1) {
  if (seq0 == seq1) {
    /* equal */
    return 0;
  } else if ((seq0 < seq1 && seq1 - seq0 < UINT16_MAX_HALF) ||
             (seq0 > seq1 && seq0 - seq1 > UINT16_MAX_HALF)) {
    /* seq1 newer than seq0 */
    return -1;
  } else {
    /* seq0 newer than seq1 */
    return 1;
  }
}

int mt_rtcp_tx_buffer_rtp_packets(struct mt_rtcp_tx *tx, struct rte_mbuf **mbufs,
                                  unsigned int bulk) {
  if (!tx->active) return 0;
  if (mt_u64_fifo_free_count(tx->mbuf_ring) < bulk) {
    struct rte_mbuf *clean_mbufs[bulk];
    if (mt_u64_fifo_get_bulk(tx->mbuf_ring, (uint64_t *)clean_mbufs, bulk) < 0) {
      err("%s(%s), failed to dequeue mbuf from ring\n", __func__, tx->name);
      return -EIO;
    }
    rte_pktmbuf_free_bulk(clean_mbufs, bulk);
  }

  /* check the seq num in order, if err happens user should check the enqueue
   * logic */
  struct st_rfc3550_rtp_hdr *rtp = rte_pktmbuf_mtod_offset(
      mbufs[0], struct st_rfc3550_rtp_hdr *, sizeof(struct mt_udp_hdr));
  uint16_t seq = ntohs(rtp->seq_number);
  uint16_t diff = seq - tx->last_seq_num; /* uint16_t wrap-around should be ok */
  if (diff != 1 && mt_u64_fifo_count(tx->mbuf_ring) != 0) {
    uint32_t ts = ntohl(rtp->tmstamp);
    err("%s(%s), ts 0x%x seq %u out of order, last seq %u\n", __func__, tx->name, ts, seq,
        tx->last_seq_num);
    return -EIO;
  }

  if (mt_u64_fifo_put_bulk(tx->mbuf_ring, (uint64_t *)mbufs, bulk) < 0) {
    err("%s(%s), failed to enqueue %u mbuf to ring\n", __func__, tx->name, bulk);
    return -EIO;
  }
  mt_mbuf_refcnt_inc_bulk(mbufs, bulk);

  /* save the last rtp seq num */
  rtp = rte_pktmbuf_mtod_offset(mbufs[bulk - 1], struct st_rfc3550_rtp_hdr *,
                                sizeof(struct mt_udp_hdr));
  tx->last_seq_num = ntohs(rtp->seq_number);

  tx->stat_rtp_sent += bulk;

  return 0;
}

static int rtcp_tx_retransmit_rtp_packets(struct mt_rtcp_tx *tx, uint16_t seq,
                                          uint16_t bulk) {
  int ret = 0;
  uint16_t nb_rt = bulk, send = 0;
  struct rte_mbuf *mbufs[bulk], *copy_mbufs[bulk];
  uint16_t ring_head_seq = 0;
  uint32_t ts = 0;
  MTL_MAY_UNUSED(ts);

  struct rte_mbuf *head_mbuf = NULL;
  if (mt_u64_fifo_read_front(tx->mbuf_ring, (uint64_t *)&head_mbuf) < 0 || !head_mbuf) {
    err("%s(%s), empty ring\n", __func__, tx->name);
    ret = -EIO;
    goto rt_exit;
  }

  struct st_rfc3550_rtp_hdr *rtp = rte_pktmbuf_mtod_offset(
      head_mbuf, struct st_rfc3550_rtp_hdr *, sizeof(struct mt_udp_hdr));
  ring_head_seq = ntohs(rtp->seq_number);
  ts = ntohl(rtp->tmstamp);

  int cmp_result = rtp_seq_num_cmp(ring_head_seq, seq);
  if (cmp_result > 0) {
    dbg("%s(%s), ts 0x%x seq %u out of date, ring head %u, you ask late\n", __func__,
        tx->name, ts, seq, ring_head_seq);
    tx->stat_rtp_retransmit_fail_obsolete += bulk;
    ret = -EIO;
    goto rt_exit;
  }

  uint16_t diff = seq - ring_head_seq;
  if (mt_u64_fifo_read_any_bulk(tx->mbuf_ring, (uint64_t *)mbufs, bulk, diff) < 0) {
    dbg("%s(%s), failed to read retransmit mbufs from ring\n", __func__, tx->name);
    tx->stat_rtp_retransmit_fail_read += bulk;
    ret = -EIO;
    goto rt_exit;
  }

  /* deep copy the mbuf then send */
  for (int i = 0; i < bulk; i++) {
    struct rte_mbuf *copied = rte_pktmbuf_copy(mbufs[i], tx->mbuf_pool, 0, UINT32_MAX);
    if (!copied) {
      dbg("%s(%s), failed to copy mbuf\n", __func__, tx->name);
      tx->stat_rtp_retransmit_fail_nobuf += bulk - i;
      nb_rt = i;
      break;
    }
    copy_mbufs[i] = copied;
    if (tx->payload_format == MT_RTP_PAYLOAD_FORMAT_RFC4175) {
      /* set the retransmit bit */
      struct st20_rfc4175_rtp_hdr *rtp = rte_pktmbuf_mtod_offset(
          copied, struct st20_rfc4175_rtp_hdr *, sizeof(struct mt_udp_hdr));
      uint16_t line1_length = ntohs(rtp->row_length);
      rtp->row_length = htons(line1_length | ST20_RETRANSMIT);
    }
  }
  send = mt_txq_burst(tx->mbuf_queue, copy_mbufs, nb_rt);
  if (send < nb_rt) {
    uint16_t burst_fail = nb_rt - send;
    rte_pktmbuf_free_bulk(&copy_mbufs[send], burst_fail);
    tx->stat_rtp_retransmit_fail_burst += burst_fail;
  }
  ret = send;

  dbg("%s(%s), ts 0x%x seq %u retransmit %u pkt(s)\n", __func__, tx->name, ts, seq, send);

rt_exit:
  tx->stat_rtp_retransmit_succ += send;
  tx->stat_rtp_retransmit_fail += bulk - send;

  return ret;
}

int mt_rtcp_tx_parse_rtcp_packet(struct mt_rtcp_tx *tx, struct mt_rtcp_hdr *rtcp) {
  if (!tx->active) return 0;
  if (rtcp->flags != 0x80) {
    err("%s(%s), wrong rtcp flags %u\n", __func__, tx->name, rtcp->flags);
    return -EIO;
  }

  if (rtcp->ptype == MT_RTCP_PTYPE_NACK) { /* nack packet */
    if (memcmp(rtcp->name, "IMTL", 4) != 0) {
      err("%s(%s), not IMTL RTCP packet\n", __func__, tx->name);
      return -EIO;
    }
    tx->stat_nack_received++;

    uint16_t num_fcis = ntohs(rtcp->len) + 1 - sizeof(struct mt_rtcp_hdr) / 4;
    struct mt_rtcp_fci *fci = rtcp->fci;
    for (uint16_t i = 0; i < num_fcis; i++) {
      uint16_t start = ntohs(fci->start);
      uint16_t follow = ntohs(fci->follow);
      dbg("%s(%s), nack %u,%u\n", __func__, tx->name, start, follow);

      if (rtcp_tx_retransmit_rtp_packets(tx, start, follow + 1) < 0) {
        dbg("%s(%s), failed to retransmit rtp packets %u,%u\n", __func__, tx->name, start,
            follow);
      }

      fci++;
    }
  }

  return 0;
}

static int rtcp_rx_update_last_cont(struct mt_rtcp_rx *rx) {
  uint16_t last_cont = rx->last_cont;
  uint16_t last_seq = rx->last_seq;
  /* find the last continuous seq */
  for (uint16_t i = last_cont + 1; rtp_seq_num_cmp(i, last_seq) <= 0; i++) {
    if (!mt_bitmap_test(rx->seq_bitmap, i % rx->seq_window_size)) break;
    rx->last_cont = i;
  }

  return 0;
}

int mt_rtcp_rx_parse_rtp_packet(struct mt_rtcp_rx *rx, struct st_rfc3550_rtp_hdr *rtp) {
  if (!rx->active) return 0;
  uint16_t seq = ntohs(rtp->seq_number);

  if (rx->ssrc == 0) { /* first received */
    rx->ssrc = ntohl(rtp->ssrc);
    rx->last_cont = seq;
    rx->last_seq = seq;
    mt_bitmap_test_and_set(rx->seq_bitmap, seq % rx->seq_window_size);
    rx->stat_rtp_received++;
    return 0;
  }

  int cmp_result = rtp_seq_num_cmp(seq, rx->last_seq);
  if (cmp_result > 0) { /* new seq */
    /* clean the bitmap for missing packets */
    for (uint16_t i = rx->last_seq + 1; rtp_seq_num_cmp(i, seq) < 0; i++) {
      mt_bitmap_test_and_unset(rx->seq_bitmap, i % rx->seq_window_size);
    }
    rx->last_seq = seq;

    uint16_t last_cont_diff = seq - rx->last_cont;
    if (last_cont_diff > rx->seq_window_size) {
      /* last cont is out of bitmap window, re-calculate from bitmap begin */
      rx->last_cont = seq - rx->seq_window_size;
      rtcp_rx_update_last_cont(rx);
    } else if (last_cont_diff == 1) {
      /* the ideal case where all pkts come in sequence */
      rx->last_cont = seq;
    }
  } else if (cmp_result < 0) {      /* old seq */
    if (seq == rx->last_cont + 1) { /* need to update cont */
      rx->last_cont = seq;
      rtcp_rx_update_last_cont(rx);
    }
  } /* else, ignore duplicate seq */

  mt_bitmap_test_and_set(rx->seq_bitmap, seq % rx->seq_window_size);
  rx->stat_rtp_received++;

  return 0;
}

int mt_rtcp_rx_send_nack_packet(struct mt_rtcp_rx *rx) {
  if (!rx->active) return 0;
  struct mtl_main_impl *impl = rx->parent;
  enum mtl_port port = rx->port;
  struct rte_mbuf *pkt;
  struct rte_ipv4_hdr *ipv4;
  struct rte_udp_hdr *udp;
  uint16_t num_fci = 0;

  uint64_t now = mt_get_tsc(impl);
  if (now < rx->nacks_send_time) return 0;
  rx->nacks_send_time = now + rx->nacks_send_interval;

  pkt = rte_pktmbuf_alloc(mt_sys_tx_mempool(impl, port));
  if (!pkt) {
    err("%s(%s), pkt alloc fail\n", __func__, rx->name);
    return -ENOMEM;
  }

  struct mt_udp_hdr *hdr = rte_pktmbuf_mtod(pkt, struct mt_udp_hdr *);
  ipv4 = &hdr->ipv4;
  udp = &hdr->udp;

  rte_memcpy(hdr, &rx->udp_hdr, sizeof(*hdr));
  mt_mbuf_init_ipv4(pkt);
  pkt->data_len = sizeof(*hdr);

  struct mt_rtcp_hdr *rtcp =
      rte_pktmbuf_mtod_offset(pkt, struct mt_rtcp_hdr *, sizeof(*hdr));
  struct mt_rtcp_fci *fcis = &rtcp->fci[0];

  /* check missing pkts with bitmap, update fci fields */
  uint16_t seq = rx->last_cont + 1;
  uint16_t start = seq;
  uint16_t end = rx->last_seq - rx->seq_skip_window;
  uint16_t miss = 0;
  bool end_state = mt_bitmap_test_and_set(rx->seq_bitmap, end % rx->seq_window_size);
  while (rtp_seq_num_cmp(seq, end) <= 0) {
    if (!mt_bitmap_test(rx->seq_bitmap, seq % rx->seq_window_size)) {
      miss++;
    } else {
      if (miss != 0) {
        fcis[num_fci].start = htons(start);
        fcis[num_fci].follow = htons(miss - 1);
        if (++num_fci >= MT_RTCP_MAX_FCIS) {
          dbg("%s(%s), too many nack items %u\n", __func__, rx->name, num_fci);
          rx->stat_nack_drop_exceed += num_fci;
          if (!end_state)
            mt_bitmap_test_and_unset(rx->seq_bitmap, end % rx->seq_window_size);
          rte_pktmbuf_free(pkt);
          return -EINVAL;
        }
        rx->stat_rtp_lost_detected += miss;
        miss = 0;
      }
      start = seq + 1;
    }
    seq++;
  }
  if (!end_state) mt_bitmap_test_and_unset(rx->seq_bitmap, end % rx->seq_window_size);
  if (num_fci == 0) {
    rte_pktmbuf_free(pkt);
    return 0;
  }

  /* update other rtcp fields */
  rtcp->flags = 0x80;
  rtcp->ptype = MT_RTCP_PTYPE_NACK;
  rtcp->len = htons(sizeof(struct mt_rtcp_hdr) / 4 - 1 + num_fci);
  rtcp->ssrc = htonl(rx->ssrc);
  rte_memcpy(rtcp->name, "IMTL", 4);

  pkt->data_len += sizeof(struct mt_rtcp_hdr) + num_fci * sizeof(struct mt_rtcp_fci);
  pkt->pkt_len = pkt->data_len;

  /* update length */
  ipv4->total_length = htons(pkt->pkt_len - sizeof(struct rte_ether_hdr));
  udp->dgram_len = htons(pkt->pkt_len - sizeof(struct rte_ether_hdr) - sizeof(*ipv4));

  uint16_t send = mt_sys_queue_tx_burst(impl, port, &pkt, 1);
  if (send != 1) {
    err("%s(%s), failed to send nack packet\n", __func__, rx->name);
    rte_pktmbuf_free(pkt);
    return -EIO;
  }

  rx->stat_nack_sent++;

  return 0;
}

static int rtcp_tx_stat(void *priv) {
  struct mt_rtcp_tx *tx = priv;

  notice("%s(%s), rtp sent %u nack recv %u rtp retransmit succ %u\n", __func__, tx->name,
         tx->stat_rtp_sent, tx->stat_nack_received, tx->stat_rtp_retransmit_succ);
  tx->stat_rtp_sent = 0;
  tx->stat_nack_received = 0;
  tx->stat_rtp_retransmit_succ = 0;
  if (tx->stat_rtp_retransmit_fail) {
    notice("%s(%s), retransmit fail %u no mbuf %u read %u obsolete %u burst %u\n",
           __func__, tx->name, tx->stat_rtp_retransmit_fail,
           tx->stat_rtp_retransmit_fail_nobuf, tx->stat_rtp_retransmit_fail_read,
           tx->stat_rtp_retransmit_fail_obsolete, tx->stat_rtp_retransmit_fail_burst);
    tx->stat_rtp_retransmit_fail_nobuf = 0;
    tx->stat_rtp_retransmit_fail_read = 0;
    tx->stat_rtp_retransmit_fail_obsolete = 0;
    tx->stat_rtp_retransmit_fail_burst = 0;
    tx->stat_rtp_retransmit_fail = 0;
  }

  return 0;
}

static int rtcp_rx_stat(void *priv) {
  struct mt_rtcp_rx *rx = priv;

  notice("%s(%s), rtp recv %u lost %u nack sent %u\n", __func__, rx->name,
         rx->stat_rtp_received, rx->stat_rtp_lost_detected, rx->stat_nack_sent);
  rx->stat_rtp_received = 0;
  rx->stat_rtp_lost_detected = 0;
  rx->stat_nack_sent = 0;
  if (rx->stat_nack_drop_exceed) {
    notice("%s(%s), nack drop exceed %u\n", __func__, rx->name,
           rx->stat_nack_drop_exceed);
    rx->stat_nack_drop_exceed = 0;
  }

  return 0;
}

struct mt_rtcp_tx *mt_rtcp_tx_create(struct mtl_main_impl *impl,
                                     struct mt_rtcp_tx_ops *ops) {
  const enum mtl_port port = ops->port;
  const char *name = ops->name;
  struct mt_rtcp_tx *tx =
      mt_rte_zmalloc_socket(sizeof(struct mt_rtcp_tx), mt_socket_id(impl, port));
  if (!tx) {
    err("%s(%s), failed to allocate memory for mt_rtcp_tx\n", __func__, name);
    return NULL;
  }
  tx->parent = impl;
  tx->port = port;
  tx->payload_format = ops->payload_format;

  if (ops->buffer_size < mt_if_nb_tx_desc(impl, port)) {
    warn("%s(%s), buffer_size(%u) is small, adjust to nb_tx_desc(%u)\n", __func__, name,
         ops->buffer_size, mt_if_nb_tx_desc(impl, port));
    ops->buffer_size = mt_if_nb_tx_desc(impl, port);
  }

  uint32_t n = ops->buffer_size + mt_if_nb_tx_desc(impl, port);
  struct rte_mempool *pool =
      mt_mempool_create(impl, port, name, n, MT_MBUF_CACHE_SIZE, 0, MTL_MTU_MAX_BYTES);
  if (!pool) {
    err("%s(%s), failed to create mempool for mt_rtcp_tx\n", __func__, name);
    mt_rtcp_tx_free(tx);
    return NULL;
  }
  tx->mbuf_pool = pool;

  struct mt_txq_flow flow;
  memset(&flow, 0, sizeof(flow));
  mtl_memcpy(&flow.dip_addr, &ops->udp_hdr->ipv4.dst_addr, MTL_IP_ADDR_LEN);
  flow.dst_port = ntohs(ops->udp_hdr->udp.dst_port) - 1; /* rtp port */
  struct mt_txq_entry *q = mt_txq_get(impl, port, &flow);
  if (!q) {
    err("%s(%s), failed to create queue for mt_rtcp_tx\n", __func__, name);
    mt_rtcp_tx_free(tx);
    return NULL;
  }
  tx->mbuf_queue = q;

  struct mt_u64_fifo *ring = mt_u64_fifo_init(ops->buffer_size, mt_socket_id(impl, port));
  if (!ring) {
    err("%s(%s), failed to create ring for mt_rtcp_tx\n", __func__, name);
    mt_rtcp_tx_free(tx);
    return NULL;
  }
  tx->mbuf_ring = ring;

  tx->ssrc = ops->ssrc;
  snprintf(tx->name, sizeof(tx->name) - 1, "%s", name);
  rte_memcpy(&tx->udp_hdr, ops->udp_hdr, sizeof(tx->udp_hdr));

  mt_stat_register(impl, rtcp_tx_stat, tx, tx->name);
  tx->active = true;

  info("%s(%s), suss\n", __func__, name);

  return tx;
}

void mt_rtcp_tx_free(struct mt_rtcp_tx *tx) {
  struct mtl_main_impl *impl = tx->parent;
  enum mtl_port port = tx->port;

  mt_stat_unregister(impl, rtcp_tx_stat, tx);

  tx->active = false;

  rtcp_tx_stat(tx);

  if (tx->mbuf_ring) {
    mt_fifo_mbuf_clean(tx->mbuf_ring);
    mt_u64_fifo_uinit(tx->mbuf_ring);
    tx->mbuf_ring = NULL;
  }

  if (tx->mbuf_queue) {
    mt_txq_flush(tx->mbuf_queue, mt_get_pad(impl, port));
    mt_txq_put(tx->mbuf_queue);
    tx->mbuf_queue = NULL;
  }

  if (tx->mbuf_pool) {
    mt_mempool_free(tx->mbuf_pool);
    tx->mbuf_pool = NULL;
  }

  mt_rte_free(tx);
}

struct mt_rtcp_rx *mt_rtcp_rx_create(struct mtl_main_impl *impl,
                                     struct mt_rtcp_rx_ops *ops) {
  const enum mtl_port port = ops->port;
  const char *name = ops->name;
  struct mt_rtcp_rx *rx =
      mt_rte_zmalloc_socket(sizeof(struct mt_rtcp_rx), mt_socket_id(impl, port));
  if (!rx) {
    err("%s(%s), failed to allocate memory for mt_rtcp_rx\n", __func__, name);
    return NULL;
  }

  rx->parent = impl;
  rx->port = port;
  rx->ssrc = 0;
  rx->nacks_send_interval = ops->nacks_send_interval;
  rx->nacks_send_time = mt_get_tsc(impl);
  snprintf(rx->name, sizeof(rx->name) - 1, "%s", name);
  rte_memcpy(&rx->udp_hdr, ops->udp_hdr, sizeof(rx->udp_hdr));

  uint8_t *seq_bitmap = mt_rte_zmalloc_socket(sizeof(uint8_t) * ops->seq_bitmap_size,
                                              mt_socket_id(impl, port));
  if (!seq_bitmap) {
    err("%s(%s), failed to allocate memory for seq_bitmap\n", __func__, name);
    mt_rtcp_rx_free(rx);
    return NULL;
  }
  rx->seq_bitmap = seq_bitmap;
  rx->seq_window_size = ops->seq_bitmap_size * 8;

  mt_stat_register(impl, rtcp_rx_stat, rx, rx->name);
  rx->active = true;

  info("%s(%s), suss\n", __func__, name);

  return rx;
}

void mt_rtcp_rx_free(struct mt_rtcp_rx *rx) {
  mt_stat_unregister(rx->parent, rtcp_rx_stat, rx);

  rx->active = false;

  rtcp_rx_stat(rx);

  if (rx->seq_bitmap) mt_rte_free(rx->seq_bitmap);

  mt_rte_free(rx);
}