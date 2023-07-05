/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#include "mt_rtcp.h"

#include "mt_dev.h"
#include "mt_log.h"

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
  tx->udp_hdr = *ops->rtp_udp_hdr;
  uint16_t udp_port = ntohs(ops->rtp_udp_hdr->udp.dst_port) + 1;
  tx->udp_hdr.udp.dst_port = htons(udp_port);
  tx->mbuf_ring = ring;
  tx->ipv4_packet_id = 0;

  return tx;
}

void mt_rtcp_tx_free(struct mt_rtcp_tx* tx) {
  if (!tx) {
    return;
  }

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
  rx->udp_hdr = *ops->rtp_udp_hdr;
  uint16_t udp_port = ntohs(ops->rtp_udp_hdr->udp.dst_port) + 1;
  rx->udp_hdr.udp.dst_port = htons(udp_port);
  rx->max_retry = ops->max_retry;
  rx->ipv4_packet_id = 0;

  MT_TAILQ_INIT(&rx->nack_list);

  return rx;
}

void mt_rtcp_rx_free(struct mt_rtcp_rx* rx) {
  if (!rx) {
    return;
  }

  mt_rte_free(rx);
}

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

  return send;
}

/* where to receive rtcp packet? add flow or rss? */
int mt_rtcp_tx_parse_nack_packet(struct mt_rtcp_tx* tx, struct mt_rtcp_hdr* rtcp) {
  if (!rtcp) {
    err("%s, no packet\n", __func__);
    return -EIO;
  }

  if (rtcp->flags != 0x80) {
    err("%s, wrong rtcp flags %u\n", __func__, rtcp->flags);
    return -EIO;
  }

  if (rtcp->ptype == MT_RTCP_PTYPE_NACK) {
    if (memcmp(rtcp->name, "IMTL", 4) != 0) {
      err("%s, not IMTL RTCP packet\n", __func__);
      return -EIO;
    }
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

int mt_rtcp_rx_parse_rtp_packet(struct mt_rtcp_rx* rx, struct st_rfc3550_rtp_hdr* rtp) {
  if (!rtp) {
    err("%s, invalid packet\n", __func__);
    return -EINVAL;
  }

  // uint16_t seq_id = ntohs(rtp->seq_number);

  /* TODO: update nack list
   * 1. remove received item from nack list
   * 2. calculate missing seq_id(s)
   * 3. make nack item
   * 4. insert to nack list
   **/

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
    return -EIO;
  }

  return 0;
}
