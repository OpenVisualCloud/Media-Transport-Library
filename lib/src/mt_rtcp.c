/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#include "mt_rtcp.h"

#include "mt_dev.h"
#include "mt_log.h"
#include "mt_stat.h"

int mt_rtcp_tx_buffer_rtp_packets(struct mt_rtcp_tx* tx, struct rte_mbuf** mbufs,
                                  unsigned int bulk) {
  if (!mbufs) {
    err("%s, no mbufs\n", __func__);
    return -EIO;
  }

  int free = rte_ring_get_capacity(tx->mbuf_ring);
  if (free < bulk) {
    int clean = bulk - free;
    struct rte_mbuf* clean_mbufs[clean];
    if (rte_ring_sc_dequeue_bulk(tx->mbuf_ring, (void**)clean_mbufs, clean, NULL) !=
        clean) {
      err("%s, failed to dequeue mbuf from ring\n", __func__);
      return -EIO;
    }
    rte_pktmbuf_free_bulk(clean_mbufs, clean);
    tx->ring_first_idx += clean;
  }

  if (rte_ring_sp_enqueue_bulk(tx->mbuf_ring, (void**)mbufs, bulk, NULL) != 0) {
    err("%s, failed to enqueue mbuf to ring\n", __func__);
    //? rte_pktmbuf_free(mbuf);
    return -EIO;
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
      err("%s, failed to dequeue mbuf from ring\n", __func__);
      return -EIO;
    }
    rte_pktmbuf_free_bulk(clean_mbufs, clean);
    tx->ring_first_idx = seq_id;
  }
  unsigned int nb_rt =
      rte_ring_sc_dequeue_burst(tx->mbuf_ring, (void**)mbufs, bulk, NULL);
  if (nb_rt == 0) {
    return 0;
  }
  tx->ring_first_idx += nb_rt;

  uint16_t send = mt_dev_tx_sys_queue_burst(impl, port, mbufs, nb_rt);

  tx->stat_rtp_retransmit_succ += send;
  tx->stat_rtp_retransmit_drop += bulk - send;

  return send;
}

/* where to receive rtcp packet? add flow or rss? */
int mt_rtcp_tx_parse_nack_packet(struct mt_rtcp_tx* tx, struct rte_mbuf* m) {
  if (!m) {
    err("%s, no packet\n", __func__);
    return -EIO;
  }

  struct mt_rtcp_hdr* rtcp =
      rte_pktmbuf_mtod_offset(m, struct mt_rtcp_hdr*, sizeof(struct mt_udp_hdr));

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

int mt_rtcp_rx_parse_rtp_packet(struct mt_rtcp_rx* rx, struct rte_mbuf* m) {
  if (!m) {
    err("%s, invalid packet\n", __func__);
    return -EINVAL;
  }

  // struct st_rfc3550_rtp_hdr* rtp =
  //     rte_pktmbuf_mtod_offset(m, struct st_rfc3550_rtp_hdr*, sizeof(struct
  //     mt_udp_hdr));

  // uint16_t seq_id = ntohs(rtp->seq_number);

  /* TODO: update nack list
   * 1. remove received item from nack list (rx->stat_rtp_retransmit_succ++)
   * 2. calculate missing seq_id(s)
   * 3. make nack item
   * 4. insert to nack list
   **/

  rx->stat_rtp_received++;

  return 0;
}

/* where to trigger this nack sending? */
int mt_rtcp_rx_send_nack_packet(struct mt_rtcp_rx* rx) {
  struct mtl_main_impl* impl = rx->parent;
  enum mtl_port port = rx->port;
  struct rte_mbuf* pkt;
  struct rte_ipv4_hdr* ipv4;

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
  pkt->pkt_len = pkt->data_len;

  /* TODO: build the nack packet
   * 1. fill rtcp header
   * 2. walk through the nack list
   * 3. get the retransmit requests
   * 4. fill the FCIs
   * 5. update mbuf len
   **/

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

  notice("%s(%d,%s), rtp recv %u nack sent %u rtp retransmit %u\n", __func__, port,
         rx->name, rx->stat_rtp_received, rx->stat_nack_sent,
         rx->stat_rtp_retransmit_succ);
  rx->stat_rtp_received = 0;
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
  if (!tx) {
    return;
  }

  mt_stat_unregister(tx->parent, rtcp_tx_stat, tx);

  if (tx->mbuf_ring) {
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
  rx->ssrc = ops->ssrc;
  strncpy(rx->name, ops->name, sizeof(rx->name) - 1);
  mtl_memcpy(&rx->udp_hdr, ops->udp_hdr, sizeof(rx->udp_hdr));

  MT_TAILQ_INIT(&rx->nack_list);

  mt_stat_register(impl, rtcp_rx_stat, rx, rx->name);

  info("%s(%d,%s), suss\n", __func__, rx->port, rx->name);

  return rx;
}

void mt_rtcp_rx_free(struct mt_rtcp_rx* rx) {
  if (!rx) {
    return;
  }

  mt_stat_unregister(rx->parent, rtcp_rx_stat, rx);

  mt_rte_free(rx);
}