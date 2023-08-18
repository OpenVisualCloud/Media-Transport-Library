/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#include "mt_rtcp.h"

#include "mt_dev.h"
#include "mt_log.h"
#include "mt_stat.h"
#include "mt_util.h"

static int rtp_seq_num_cmp(uint16_t seq0, uint16_t seq1) {
  if (seq0 == seq1) {
    /* equal */
    return 0;
  } else if ((seq0 < seq1 && seq1 - seq0 < 32768) ||
             (seq0 > seq1 && seq0 - seq1 > 32768)) {
    /* seq1 newer than seq0 */
    return -1;
  } else {
    /* seq0 newer than seq1 */
    return 1;
  }
}

int mt_rtcp_tx_buffer_rtp_packets(struct mt_rtcp_tx* tx, struct rte_mbuf** mbufs,
                                  unsigned int bulk) {
  if (rte_ring_free_count(tx->mbuf_ring) < bulk) {
    struct rte_mbuf* clean_mbufs[bulk];
    if (rte_ring_sc_dequeue_bulk(tx->mbuf_ring, (void**)clean_mbufs, bulk, NULL) !=
        bulk) {
      err("%s(%s), failed to dequeue mbuf from ring\n", __func__, tx->name);
      return -EIO;
    }
    rte_pktmbuf_free_bulk(clean_mbufs, bulk);
  }

  if (rte_ring_sp_enqueue_bulk(tx->mbuf_ring, (void**)mbufs, bulk, NULL) == 0) {
    err("%s(%s), failed to enqueue %u mbuf to ring\n", __func__, tx->name, bulk);
    return -EIO;
  }

  for (int i = 0; i < bulk; i++) {
    rte_mbuf_refcnt_update(mbufs[i], 1);
    if (mbufs[i]->next) rte_mbuf_refcnt_update(mbufs[i]->next, 1);
  }

  tx->stat_rtp_sent += bulk;

  return 0;
}

static int rtcp_tx_retransmit_rtp_packets(struct mt_rtcp_tx* tx, uint16_t seq_id,
                                          uint16_t bulk) {
  int ret = 0;
  uint16_t send = 0;
  struct rte_mbuf *mbufs[bulk], *copy_mbufs[bulk];
  uint16_t ring_head_id = 0;
  uint32_t ts = 0;

  struct rte_mbuf* head_mbuf = NULL;

  uint32_t n = rte_ring_dequeue_bulk_start(tx->mbuf_ring, (void**)&head_mbuf, 1, NULL);
  if (n != 0) {
    struct st_rfc3550_rtp_hdr* rtp = rte_pktmbuf_mtod_offset(
        head_mbuf, struct st_rfc3550_rtp_hdr*, sizeof(struct mt_udp_hdr));
    ring_head_id = ntohs(rtp->seq_number);
    ts = ntohl(rtp->tmstamp);
    rte_ring_dequeue_finish(tx->mbuf_ring, 0);
  } else {
    err("%s(%s), empty ring\n", __func__, tx->name);
    ret = -EIO;
    goto rt_exit;
  }

  int cmp_result = rtp_seq_num_cmp(ring_head_id, seq_id);
  if (cmp_result < 0) {
    uint16_t clean = seq_id - ring_head_id;
    if (clean >= rte_ring_count(tx->mbuf_ring)) {
      err("%s(%s), ts %u seq %u not sent yet, ring head %u, how can you ask for it???\n",
          __func__, tx->name, ts, seq_id, ring_head_id);
      ret = -EIO;
      goto rt_exit;
    }
    struct rte_mbuf* clean_mbufs[clean];
    if (rte_ring_sc_dequeue_bulk(tx->mbuf_ring, (void**)clean_mbufs, clean, NULL) !=
        clean) {
      err("%s(%s), failed to dequeue clean mbuf from ring\n", __func__, tx->name);
      ret = -EIO;
      goto rt_exit;
    }
    rte_pktmbuf_free_bulk(clean_mbufs, clean);
  } else if (cmp_result > 0) {
    warn("%s(%s), ts %u seq %u out of date, ring head %u, you ask late\n", __func__,
         tx->name, ts, seq_id, ring_head_id);
    ret = -EIO;
    goto rt_exit;
  }
  unsigned int nb_rt = rte_ring_sc_dequeue_bulk(tx->mbuf_ring, (void**)mbufs, bulk, NULL);
  if (nb_rt == 0) {
    err("%s(%s), failed to dequeue retransmit mbuf from ring\n", __func__, tx->name);
    ret = -EIO;
    goto rt_exit;
  }

  /* deep copy the mbuf then send */
  for (int i = 0; i < bulk; i++) {
    copy_mbufs[i] = rte_pktmbuf_copy(mbufs[i], mt_get_tx_mempool(tx->parent, tx->port), 0,
                                     UINT32_MAX);
    if (!copy_mbufs[i]) {
      err("%s(%s), failed to copy mbuf\n", __func__, tx->name);
      rte_pktmbuf_free_bulk(mbufs, bulk);
      ret = -ENOMEM;
      goto rt_exit;
    }
  }
  rte_pktmbuf_free_bulk(mbufs, bulk);
  send = mt_dev_tx_sys_queue_burst(tx->parent, tx->port, copy_mbufs, nb_rt);
  ret = send;

  dbg("%s(%s), ts %u seq %u retransmit %u pkt(s)\n", __func__, tx->name, ts, seq_id,
      send);

rt_exit:
  tx->stat_rtp_retransmit_succ += send;
  tx->stat_rtp_retransmit_fail += bulk - send;

  return ret;
}

int mt_rtcp_tx_parse_nack_packet(struct mt_rtcp_tx* tx, struct mt_rtcp_hdr* rtcp) {
  if (rtcp->flags != 0x80) {
    err("%s(%s), wrong rtcp flags %u\n", __func__, tx->name, rtcp->flags);
    return -EIO;
  }

  if (rtcp->ptype == MT_RTCP_PTYPE_NACK) {
    if (memcmp(rtcp->name, "IMTL", 4) != 0) {
      err("%s(%s), not IMTL RTCP packet\n", __func__, tx->name);
      return -EIO;
    }
    tx->stat_nack_received++;

    uint16_t num_fcis = ntohs(rtcp->len) + 1 - sizeof(struct mt_rtcp_hdr) / 4;
    struct mt_rtcp_fci* fci = (struct mt_rtcp_fci*)(rtcp->name + 4);
    for (uint16_t i = 0; i < num_fcis; i++) {
      uint16_t start = ntohs(fci->start);
      uint16_t follow = ntohs(fci->follow);
      dbg("%s(%s), nack %u,%u\n", __func__, tx->name, start, follow);

      if (rtcp_tx_retransmit_rtp_packets(tx, start, follow + 1) < 0) {
        warn("%s(%s), failed to retransmit rtp packets %u,%u\n", __func__, tx->name,
             start, follow);
      }

      fci++;
    }
  }

  return 0;
}

int mt_rtcp_rx_parse_rtp_packet(struct mt_rtcp_rx* rx, struct st_rfc3550_rtp_hdr* rtp) {
  struct mtl_main_impl* impl = rx->parent;
  enum mtl_port port = rx->port;

  uint16_t seq_id = ntohs(rtp->seq_number);

  if (rx->ssrc == 0) { /* first received */
    rx->ssrc = ntohl(rtp->ssrc);
    rx->last_seq_id = seq_id;
    rx->stat_rtp_received++;
    return 0;
  }

  int cmp_result = rtp_seq_num_cmp(seq_id, rx->last_seq_id + 1);
  if (cmp_result == 0) { /* pkt received in sequence */
    rx->last_seq_id = seq_id;
  } else if (cmp_result > 0) { /* pkt(s) lost */
    uint16_t lost_packets = seq_id - rx->last_seq_id - 1;
    rx->stat_rtp_lost_detected += lost_packets;
    /* insert nack */
    struct mt_rtcp_nack_item* nack =
        mt_rte_zmalloc_socket(sizeof(*nack), mt_socket_id(impl, port));
    if (!nack) {
      err("%s(%s), failed to alloc nack item\n", __func__, rx->name);
      return -ENOMEM;
    }
    nack->expire_time = mt_get_tsc(impl) + rx->nack_expire_interval;
    nack->seq_id = rx->last_seq_id + 1;
    nack->bulk = lost_packets - 1;
    nack->retry_count = rx->max_retry;
    MT_TAILQ_INSERT_TAIL(&rx->nack_list, nack, next);
    dbg("%s(%s), pkt lost, ts %u seq %u last_seq %u, insert nack %u,%u\n", __func__,
        rx->name, ntohl(rtp->tmstamp), seq_id, rx->last_seq_id, nack->seq_id, nack->bulk);
    rx->last_seq_id = seq_id;
  } else {
    /* remove out-of-date/recovered pkt from nack list, split nack to left and right */
    dbg("%s(%s), pkt recovered, ts %u seq %u last_seq %u\n", __func__, rx->name, ts,
        seq_id, rx->last_seq_id);
    struct mt_rtcp_nack_item *nack, *tmp_nack;
    for (nack = MT_TAILQ_FIRST(&rx->nack_list); nack != NULL; nack = tmp_nack) {
      tmp_nack = MT_TAILQ_NEXT(nack, next);
      if (seq_id - nack->seq_id >= 0 && seq_id - nack->seq_id <= nack->bulk) {
        if (nack->seq_id + nack->bulk - seq_id > 0) {
          /* insert right nack */
          struct mt_rtcp_nack_item* right_nack =
              mt_rte_zmalloc_socket(sizeof(*right_nack), mt_socket_id(impl, port));
          if (!right_nack) {
            err("%s(%s), failed to alloc right nack item\n", __func__, rx->name);
            return -ENOMEM;
          }
          right_nack->seq_id = seq_id + 1;
          right_nack->bulk = nack->seq_id + nack->bulk - seq_id - 1;
          right_nack->retry_count = nack->retry_count;
          right_nack->expire_time = nack->expire_time;
          MT_TAILQ_INSERT_HEAD(&rx->nack_list, right_nack, next);
        }
        if (seq_id - nack->seq_id > 0) {
          /* insert left nack */
          struct mt_rtcp_nack_item* left_nack =
              mt_rte_zmalloc_socket(sizeof(*left_nack), mt_socket_id(impl, port));
          if (!left_nack) {
            err("%s(%s), failed to alloc left nack item\n", __func__, rx->name);
            return -ENOMEM;
          }
          left_nack->seq_id = nack->seq_id;
          left_nack->bulk = seq_id - nack->seq_id - 1;
          left_nack->retry_count = nack->retry_count;
          left_nack->expire_time = nack->expire_time;
          MT_TAILQ_INSERT_HEAD(&rx->nack_list, left_nack, next);
        }
        MT_TAILQ_REMOVE(&rx->nack_list, nack, next);
        mt_rte_free(nack);
        rx->stat_rtp_retransmit_succ++;
        break;
      }
    }
  }

  rx->stat_rtp_received++;

  return 0;
}

int mt_rtcp_rx_send_nack_packet(struct mt_rtcp_rx* rx) {
  struct mtl_main_impl* impl = rx->parent;
  enum mtl_port port = rx->port;
  struct rte_mbuf* pkt;
  struct rte_ipv4_hdr* ipv4;
  struct rte_udp_hdr* udp;

  uint64_t now = mt_get_tsc(impl);
  if (now < rx->nacks_send_time) return 0;
  rx->nacks_send_time = now + rx->nacks_send_interval;

  if (MT_TAILQ_FIRST(&rx->nack_list) == NULL) return 0;

  pkt = rte_pktmbuf_alloc(mt_get_tx_mempool(impl, port));
  if (!pkt) {
    err("%s(%s), pkt alloc fail\n", __func__, rx->name);
    return -ENOMEM;
  }

  struct mt_udp_hdr* hdr = rte_pktmbuf_mtod(pkt, struct mt_udp_hdr*);
  ipv4 = &hdr->ipv4;
  udp = &hdr->udp;

  rte_memcpy(hdr, &rx->udp_hdr, sizeof(*hdr));
  ipv4->packet_id = htons(rx->ipv4_packet_id++);

  mt_mbuf_init_ipv4(pkt);
  pkt->data_len = sizeof(*hdr);

  struct mt_rtcp_hdr* rtcp =
      rte_pktmbuf_mtod_offset(pkt, struct mt_rtcp_hdr*, sizeof(*hdr));
  rtcp->flags = 0x80;
  rtcp->ptype = MT_RTCP_PTYPE_NACK;
  rtcp->ssrc = htonl(rx->ssrc);
  rte_memcpy(rtcp->name, "IMTL", 4);
  uint16_t num_fci = 0;

  struct mt_rtcp_nack_item *nack, *tmp_nack;
  struct mt_rtcp_fci* fci = (struct mt_rtcp_fci*)(rtcp->name + 4);
  for (nack = MT_TAILQ_FIRST(&rx->nack_list); nack != NULL; nack = tmp_nack) {
    tmp_nack = MT_TAILQ_NEXT(nack, next);
    if (now > nack->expire_time) {
      MT_TAILQ_REMOVE(&rx->nack_list, nack, next);
      mt_rte_free(nack);
      rx->stat_nack_expire++;
      continue;
    }
    if (nack->retry_count == 0) continue;
    fci->start = htons(nack->seq_id);
    fci->follow = htons(nack->bulk);
    num_fci++;
    fci++;
    nack->retry_count--;
  }
  rtcp->len = htons(sizeof(struct mt_rtcp_hdr) / 4 - 1 + num_fci);

  pkt->data_len += sizeof(struct mt_rtcp_hdr) + num_fci * sizeof(struct mt_rtcp_fci);
  pkt->pkt_len = pkt->data_len;

  /* update length */
  ipv4->total_length = htons(pkt->pkt_len - sizeof(struct rte_ether_hdr));
  udp->dgram_len = htons(pkt->pkt_len - sizeof(struct rte_ether_hdr) - sizeof(*ipv4));

  uint16_t send = mt_dev_tx_sys_queue_burst(impl, port, &pkt, 1);
  if (send != 1) {
    err("%s(%s), failed to send nack packet\n", __func__, rx->name);
    rte_pktmbuf_free(pkt);
    return -EIO;
  }

  rx->stat_nack_sent++;

  return 0;
}

static int rtcp_tx_stat(void* priv) {
  struct mt_rtcp_tx* tx = priv;

  notice("%s(%s), rtp sent %u nack recv %u rtp retransmit succ %u\n", __func__, tx->name,
         tx->stat_rtp_sent, tx->stat_nack_received, tx->stat_rtp_retransmit_succ);
  tx->stat_rtp_sent = 0;
  tx->stat_nack_received = 0;
  tx->stat_rtp_retransmit_succ = 0;
  if (tx->stat_rtp_retransmit_fail) {
    warn("%s(%s), rtp retransmit fail %u\n", __func__, tx->name,
         tx->stat_rtp_retransmit_fail);
    tx->stat_rtp_retransmit_fail = 0;
  }

  return 0;
}

static int rtcp_rx_stat(void* priv) {
  struct mt_rtcp_rx* rx = priv;

  notice("%s(%s), rtp recv %u lost %u nack sent %u\n", __func__, rx->name,
         rx->stat_rtp_received, rx->stat_rtp_lost_detected, rx->stat_nack_sent);
  rx->stat_rtp_received = 0;
  rx->stat_rtp_lost_detected = 0;
  rx->stat_nack_sent = 0;
  if (rx->stat_nack_expire) {
    warn("%s(%s), nack expire %u\n", __func__, rx->name, rx->stat_nack_expire);
    rx->stat_nack_expire = 0;
  }
  if (rx->stat_rtp_retransmit_succ) {
    notice("%s(%s), rtp recovered %u\n", __func__, rx->name,
           rx->stat_rtp_retransmit_succ);
    rx->stat_rtp_retransmit_succ = 0;
  }

  return 0;
}

struct mt_rtcp_tx* mt_rtcp_tx_create(struct mtl_main_impl* impl,
                                     struct mt_rtcp_tx_ops* ops) {
  struct mt_rtcp_tx* tx =
      mt_rte_zmalloc_socket(sizeof(struct mt_rtcp_tx), mt_socket_id(impl, ops->port));
  if (!tx) {
    err("%s(%s), failed to allocate memory for mt_rtcp_tx\n", __func__, ops->name);
    return NULL;
  }
  tx->parent = impl;
  tx->port = ops->port;

  char ring_name[32];
  snprintf(ring_name, sizeof(ring_name), MT_RTCP_TX_RING_PREFIX "%s", ops->name);
  struct rte_ring* ring =
      rte_ring_create(ring_name, ops->buffer_size, mt_socket_id(impl, ops->port),
                      RING_F_SP_ENQ | RING_F_SC_DEQ);
  if (!ring) {
    err("%s(%s), failed to create ring for mt_rtcp_tx\n", __func__, ops->name);
    mt_rtcp_tx_free(tx);
    return NULL;
  }
  tx->mbuf_ring = ring;
  tx->ipv4_packet_id = 0;
  tx->ssrc = ops->ssrc;
  snprintf(tx->name, sizeof(tx->name) - 1, "%s", ops->name);
  rte_memcpy(&tx->udp_hdr, ops->udp_hdr, sizeof(tx->udp_hdr));

  mt_stat_register(impl, rtcp_tx_stat, tx, tx->name);

  info("%s(%s), suss\n", __func__, tx->name);

  return tx;
}

void mt_rtcp_tx_free(struct mt_rtcp_tx* tx) {
  mt_stat_unregister(tx->parent, rtcp_tx_stat, tx);

  if (tx->mbuf_ring) {
    mt_ring_dequeue_clean(tx->mbuf_ring);
    rte_ring_free(tx->mbuf_ring);
  }

  mt_rte_free(tx);
}

struct mt_rtcp_rx* mt_rtcp_rx_create(struct mtl_main_impl* impl,
                                     struct mt_rtcp_rx_ops* ops) {
  struct mt_rtcp_rx* rx =
      mt_rte_zmalloc_socket(sizeof(struct mt_rtcp_rx), mt_socket_id(impl, ops->port));
  if (!rx) {
    err("%s(%s), failed to allocate memory for mt_rtcp_rx\n", __func__, ops->name);
    return NULL;
  }

  rx->parent = impl;
  rx->port = ops->port;
  rx->max_retry = ops->max_retry;
  rx->ipv4_packet_id = 0;
  rx->ssrc = 0;
  rx->nack_expire_interval = ops->nack_expire_interval;
  rx->nacks_send_interval = ops->nacks_send_interval;
  rx->nacks_send_time = mt_get_tsc(impl);
  snprintf(rx->name, sizeof(rx->name) - 1, "%s", ops->name);
  rte_memcpy(&rx->udp_hdr, ops->udp_hdr, sizeof(rx->udp_hdr));

  MT_TAILQ_INIT(&rx->nack_list);

  mt_stat_register(impl, rtcp_rx_stat, rx, rx->name);

  info("%s(%s), suss\n", __func__, rx->name);

  return rx;
}

void mt_rtcp_rx_free(struct mt_rtcp_rx* rx) {
  mt_stat_unregister(rx->parent, rtcp_rx_stat, rx);

  /* free all nack items */
  struct mt_rtcp_nack_item* nack;
  while ((nack = MT_TAILQ_FIRST(&rx->nack_list))) {
    MT_TAILQ_REMOVE(&rx->nack_list, nack, next);
    mt_rte_free(nack);
  }

  mt_rte_free(rx);
}