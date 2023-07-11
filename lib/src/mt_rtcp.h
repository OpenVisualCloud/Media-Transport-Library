/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#ifndef _MT_LIB_RTCP_HEAD_H_
#define _MT_LIB_RTCP_HEAD_H_

#include "mt_main.h"

#define MT_RTCP_PTYPE_NACK (204)

MTL_PACK(struct mt_rtcp_hdr {
  uint8_t flags;
  uint8_t ptype;
  uint16_t len;
  uint32_t ssrc;
  uint8_t name[4];
});

MTL_PACK(struct mt_rtcp_fci {
  uint16_t start;
  uint16_t follow;
});

struct mt_rtcp_nack_item {
  uint16_t seq_id;
  uint16_t bulk;
  uint16_t retry_count;
  /* expire time? */
  MT_TAILQ_ENTRY(mt_rtcp_nack_item) next;
};
MT_TAILQ_HEAD(mt_rtcp_nack_list, mt_rtcp_nack_item);

struct mt_rtcp_tx_ops {
  const char* name;
  struct mt_udp_hdr* udp_hdr;
  uint32_t ssrc;
  uint16_t buffer_size;
  enum mtl_port port;
};

struct mt_rtcp_rx_ops {
  const char* name;
  struct mt_udp_hdr* udp_hdr;
  uint16_t max_idx;
  uint16_t max_retry;
  enum mtl_port port;
};

struct mt_rtcp_tx {
  struct mtl_main_impl* parent;
  enum mtl_port port;
  struct rte_ring* mbuf_ring;
  uint16_t ring_first_idx;
  struct mt_udp_hdr udp_hdr;
  char name[32];
  uint32_t ssrc;

  uint16_t ipv4_packet_id;

  /* stat */
  uint32_t stat_rtp_sent;
  uint32_t stat_rtp_retransmit_succ;
  uint32_t stat_rtp_retransmit_drop;
  uint32_t stat_nack_received;
};

struct mt_rtcp_rx {
  struct mtl_main_impl* parent;
  enum mtl_port port;
  struct mt_rtcp_nack_list nack_list;
  uint16_t max_retry;
  uint16_t last_seq_id;
  struct mt_udp_hdr udp_hdr;
  char name[32];
  uint32_t ssrc;

  uint16_t ipv4_packet_id;

  /* stat */
  uint32_t stat_rtp_received;
  uint32_t stat_rtp_lost_detected;
  uint32_t stat_rtp_retransmit_succ;
  uint32_t stat_nack_sent;
};

struct mt_rtcp_tx* mt_rtcp_tx_create(struct mtl_main_impl* mtl,
                                     struct mt_rtcp_tx_ops* ops);
void mt_rtcp_tx_free(struct mt_rtcp_tx* tx);

struct mt_rtcp_rx* mt_rtcp_rx_create(struct mtl_main_impl* mtl,
                                     struct mt_rtcp_rx_ops* ops);
void mt_rtcp_rx_free(struct mt_rtcp_rx* rx);

int mt_rtcp_tx_buffer_rtp_packets(struct mt_rtcp_tx* tx, struct rte_mbuf** mbufs,
                                  unsigned int bulk);
int mt_rtcp_tx_parse_nack_packet(struct mt_rtcp_tx* tx, struct mt_rtcp_hdr* rtcp);

int mt_rtcp_rx_parse_rtp_packet(struct mt_rtcp_rx* rx, struct st_rfc3550_rtp_hdr* rtp);
int mt_rtcp_rx_send_nack_packet(struct mt_rtcp_rx* rx);

#endif