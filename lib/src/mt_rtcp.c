/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#include "mt_rtcp.h"

#include "mt_dev.h"
#include "mt_log.h"
#include "mt_stat.h"
#include "mt_util.h"

int mt_rtcp_tx_buffer_rtp_packets(struct mt_rtcp_tx* tx, struct rte_mbuf** mbufs,
                                  unsigned int bulk) {
  if (rte_ring_free_count(tx->mbuf_ring) < bulk) {
    struct rte_mbuf* clean_mbufs[bulk];
    if (rte_ring_sc_dequeue_bulk(tx->mbuf_ring, (void**)clean_mbufs, bulk, NULL) !=
        bulk) {
      err("%s, failed to dequeue mbuf from ring\n", __func__);
      return -EIO;
    }
    rte_pktmbuf_free_bulk(clean_mbufs, bulk);
    tx->ring_first_idx += bulk;
  }

  if (rte_ring_sp_enqueue_bulk(tx->mbuf_ring, (void**)mbufs, bulk, NULL) == 0) {
    err("%s, failed to enqueue %u mbuf to ring\n", __func__, bulk);
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
  struct mtl_main_impl* impl = tx->parent;
  enum mtl_port port = tx->port;
  struct rte_mbuf* mbufs[bulk];
  if (tx->ring_first_idx < seq_id) {
    int clean = seq_id - tx->ring_first_idx;
    struct rte_mbuf* clean_mbufs[clean];
    if (rte_ring_sc_dequeue_bulk(tx->mbuf_ring, (void**)clean_mbufs, clean, NULL) !=
        clean) {
      err("%s, failed to dequeue old mbufs from ring\n", __func__);
      return -EIO;
    }
    rte_pktmbuf_free_bulk(clean_mbufs, clean);
    tx->ring_first_idx = seq_id;
  }
  unsigned int nb_rt =
      rte_ring_sc_dequeue_burst(tx->mbuf_ring, (void**)mbufs, bulk, NULL);
  if (nb_rt == 0) {
    warn("%s, no mbufs to retransmit\n", __func__);
    return 0;
  }
  tx->ring_first_idx += nb_rt;

  uint16_t send = mt_dev_tx_sys_queue_burst(impl, port, mbufs, nb_rt);

  tx->stat_rtp_retransmit_succ += send;
  tx->stat_rtp_retransmit_drop += bulk - send;

  return send;
}

/* where to receive rtcp packet? add flow or rss? */
int mt_rtcp_tx_parse_nack_packet(struct mt_rtcp_tx* tx, struct mt_rtcp_hdr* rtcp) {
  if (rtcp->flags != 0x80) {
    err("%s, wrong rtcp flags %u\n", __func__, rtcp->flags);
    return -EIO;
  }

  if (rtcp->ptype == MT_RTCP_PTYPE_NACK) {
    if (memcmp(rtcp->name, "IMTL", 4) != 0) {
      err("%s, not IMTL RTCP packet\n", __func__);
      return -EIO;
    }
    tx->stat_nack_received++;
    uint16_t num_fcis = ntohs(rtcp->len) + 1 - sizeof(struct mt_rtcp_hdr) / 4;
    for (uint16_t i = 0; i < num_fcis; i++) {
      struct mt_rtcp_fci* fci =
          (struct mt_rtcp_fci*)(rtcp->name + 4 + i * sizeof(struct mt_rtcp_fci));
      uint16_t start = ntohs(fci->start);
      uint16_t follow = ntohs(fci->follow);
      if (rtcp_tx_retransmit_rtp_packets(tx, start, follow + 1) < 0) {
        warn("%s, failed to retransmit rtp packets %u,%u\n", __func__, start, follow);
      }
    }
  }

  return 0;
}

static int rtp_seq_num_cmp(uint16_t seq0, uint16_t seq1) {
  if (seq0 == seq1) {
    return 0;
  } else if ((seq0 < seq1 && seq1 - seq0 < 32768) ||
             (seq0 > seq1 && seq0 - seq1 > 32768)) {
    return -1;
  } else {
    return 1;
  }
}

int mt_rtcp_rx_parse_rtp_packet(struct mt_rtcp_rx* rx, struct st_rfc3550_rtp_hdr* rtp) {
  struct mtl_main_impl* impl = rx->parent;
  enum mtl_port port = rx->port;

  uint16_t seq_id = ntohs(rtp->seq_number);

  if (rx->ssrc == 0) { /* first received */
    rx->ssrc = ntohl(rtp->ssrc);
    rx->last_seq_id = seq_id;
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
      err("%s, failed to alloc nack item\n", __func__);
      return -ENOMEM;
    }
    nack->seq_id = rx->last_seq_id + 1;
    nack->bulk = lost_packets - 1;
    nack->retry_count = rx->max_retry;
    MT_TAILQ_INSERT_TAIL(&rx->nack_list, nack, next);
    rx->last_seq_id = seq_id;
  } else {
    /* TODO: remove out-of-date / recovered pkt from nack list */
  }

  rx->stat_rtp_received++;

  if (seq_id % 128 == 0) mt_rtcp_rx_send_nack_packet(rx);

  return 0;
}

/* where to trigger this nack sending? */
int mt_rtcp_rx_send_nack_packet(struct mt_rtcp_rx* rx) {
  struct mtl_main_impl* impl = rx->parent;
  enum mtl_port port = rx->port;
  struct rte_mbuf* pkt;
  struct rte_ipv4_hdr* ipv4;

  if (MT_TAILQ_FIRST(&rx->nack_list) == NULL) return 0;

  pkt = rte_pktmbuf_alloc(mt_get_tx_mempool(impl, port));
  if (!pkt) {
    err("%s(%d), pkt alloc fail\n", __func__, port);
    return -ENOMEM;
  }

  struct mt_udp_hdr* hdr = rte_pktmbuf_mtod(pkt, struct mt_udp_hdr*);
  ipv4 = &hdr->ipv4;

  rte_memcpy(hdr, &rx->udp_hdr, sizeof(*hdr));
  ipv4->packet_id = rx->ipv4_packet_id++;

  mt_mbuf_init_ipv4(pkt);
  pkt->data_len = sizeof(*hdr);

  struct mt_rtcp_hdr* rtcp =
      rte_pktmbuf_mtod_offset(pkt, struct mt_rtcp_hdr*, pkt->pkt_len);
  rtcp->flags = 0x80;
  rtcp->ptype = MT_RTCP_PTYPE_NACK;
  rtcp->ssrc = htonl(rx->ssrc);
  mtl_memcpy(rtcp->name, "IMTL", 4);
  uint16_t num_fci = 0;

  struct mt_rtcp_nack_item* nack;
  while ((nack = MT_TAILQ_FIRST(&rx->nack_list))) {
    struct mt_rtcp_fci* fci =
        (struct mt_rtcp_fci*)(rtcp->name + 4 + num_fci * sizeof(struct mt_rtcp_fci));
    fci->start = htons(nack->seq_id);
    fci->follow = htons(nack->bulk);
    num_fci++;
    nack->retry_count--;
    if (nack->retry_count == 0) {
      MT_TAILQ_REMOVE(&rx->nack_list, nack, next);
      mt_rte_free(nack);
    }
  }
  rtcp->len = htons(sizeof(struct mt_rtcp_hdr) / 4 - 1 + num_fci);

  pkt->data_len += sizeof(struct mt_rtcp_hdr) + num_fci * sizeof(struct mt_rtcp_fci);
  pkt->pkt_len = pkt->data_len;

  uint16_t send = mt_dev_tx_sys_queue_burst(impl, port, &pkt, 1);
  if (send != 1) {
    err("%s, failed to send nack packet\n", __func__);
    rte_pktmbuf_free(pkt);
    return -EIO;
  }

  rx->stat_nack_sent++;

  return 0;
}

static int rtcp_tx_stat(void* priv) {
  struct mt_rtcp_tx* tx = priv;
  enum mtl_port port = tx->port;

  notice("%s(%d,%s), rtp sent %u nack recv %u rtp retansmit %u\n", __func__, port,
         tx->name, tx->stat_rtp_sent, tx->stat_nack_received,
         tx->stat_rtp_retransmit_succ);
  tx->stat_rtp_sent = 0;
  tx->stat_nack_received = 0;
  tx->stat_rtp_retransmit_succ = 0;
  if (tx->stat_rtp_retransmit_drop) {
    warn("%s(%d,%s), rtp retansmit fail %u\n", __func__, port, tx->name,
         tx->stat_rtp_retransmit_drop);
    tx->stat_rtp_retransmit_drop = 0;
  }

  return 0;
}

static int rtcp_rx_stat(void* priv) {
  struct mt_rtcp_rx* rx = priv;
  enum mtl_port port = rx->port;

  notice("%s(%d,%s), rtp recv %u lost %u nack sent %u rtp retransmit %u\n", __func__,
         port, rx->name, rx->stat_rtp_received, rx->stat_rtp_lost_detected,
         rx->stat_nack_sent, rx->stat_rtp_retransmit_succ);
  rx->stat_rtp_received = 0;
  rx->stat_rtp_lost_detected = 0;
  rx->stat_nack_sent = 0;
  rx->stat_rtp_retransmit_succ = 0;

  return 0;
}

struct mt_rtcp_tx* mt_rtcp_tx_create(struct mtl_main_impl* impl,
                                     struct mt_rtcp_tx_ops* ops) {
  struct mt_rtcp_tx* tx =
      mt_rte_zmalloc_socket(sizeof(struct mt_rtcp_tx), mt_socket_id(impl, ops->port));
  if (!tx) {
    err("%s, failed to allocate memory for mt_rtcp_tx\n", __func__);
    return NULL;
  }

  struct rte_ring* ring =
      rte_ring_create(ops->name, ops->buffer_size, mt_socket_id(impl, ops->port),
                      RING_F_SP_ENQ | RING_F_SC_DEQ);
  if (!ring) {
    err("%s, failed to create ring for mt_rtcp_tx\n", __func__);
    mt_rtcp_tx_free(tx);
    return NULL;
  }

  tx->parent = impl;
  tx->port = ops->port;
  tx->mbuf_ring = ring;
  tx->ipv4_packet_id = 0;
  tx->ssrc = ops->ssrc;
  strncpy(tx->name, ops->name, sizeof(tx->name) - 1);
  mtl_memcpy(&tx->udp_hdr, ops->udp_hdr, sizeof(tx->udp_hdr));

  mt_stat_register(impl, rtcp_tx_stat, tx, tx->name);

  info("%s(%d,%s), suss\n", __func__, tx->port, tx->name);

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
    err("%s, failed to allocate memory for mt_rtcp_rx\n", __func__);
    return NULL;
  }

  rx->parent = impl;
  rx->port = ops->port;
  rx->max_retry = ops->max_retry;
  rx->ipv4_packet_id = 0;
  rx->ssrc = 0;
  strncpy(rx->name, ops->name, sizeof(rx->name) - 1);
  mtl_memcpy(&rx->udp_hdr, ops->udp_hdr, sizeof(rx->udp_hdr));

  MT_TAILQ_INIT(&rx->nack_list);

  mt_stat_register(impl, rtcp_rx_stat, rx, rx->name);

  info("%s(%d,%s), suss\n", __func__, rx->port, rx->name);

  return rx;
}

void mt_rtcp_rx_free(struct mt_rtcp_rx* rx) {
  mt_stat_unregister(rx->parent, rtcp_rx_stat, rx);

  mt_rte_free(rx);
}