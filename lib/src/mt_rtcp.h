/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#ifndef _MT_LIB_RTCP_HEAD_H_
#define _MT_LIB_RTCP_HEAD_H_

#include "mt_main.h"

#define MT_RTCP_PTYPE_NACK (204)
#define MT_RTCP_MAX_NAME_LEN (24)

#define MT_RTCP_TX_RING_PREFIX "TRT_"

MTL_PACK(struct mt_rtcp_fci {
  uint16_t start;
  uint16_t follow;
});

MTL_PACK(struct mt_rtcp_hdr {
  uint8_t flags;
  uint8_t ptype;
  uint16_t len;
  uint32_t ssrc;
  uint8_t name[4];
  struct mt_rtcp_fci fci[0];
});

struct mt_rtcp_nack_item {
  uint16_t seq_id;
  uint16_t bulk;
  uint16_t retry_count;
  uint64_t expire_time;
  MT_TAILQ_ENTRY(mt_rtcp_nack_item) next;
};
MT_TAILQ_HEAD(mt_rtcp_nack_list, mt_rtcp_nack_item);

struct mt_rtcp_tx_ops {
  const char* name;           /* short and unique name for each session */
  struct mt_udp_hdr* udp_hdr; /* headers including eth, ipv4 and udp */
  uint32_t ssrc;              /* ssrc of rtp session */
  uint16_t buffer_size;       /* max number of buffered rtp packets */
  enum mtl_port port;         /* port of rtp session */
};

struct mt_rtcp_rx_ops {
  const char* name;              /* short and unique name for each session */
  struct mt_udp_hdr* udp_hdr;    /* headers including eth, ipv4 and udp */
  uint16_t max_retry;            /* max retry count for each nack item */
  uint64_t nacks_send_interval;  /* nack sending interval */
  uint64_t nack_expire_interval; /* nack expire time interval */
  enum mtl_port port;            /* port of rtp session */
};

struct mt_rtcp_tx {
  struct mtl_main_impl* parent;
  enum mtl_port port;
  struct rte_ring* mbuf_ring;
  struct mt_udp_hdr udp_hdr;
  char name[MT_RTCP_MAX_NAME_LEN];
  uint32_t ssrc;

  uint16_t ipv4_packet_id;

  /* stat */
  uint32_t stat_rtp_sent;
  uint32_t stat_rtp_retransmit_succ;
  uint32_t stat_rtp_retransmit_fail;
  uint32_t stat_rtp_retransmit_fail_nobuf;
  uint32_t stat_rtp_retransmit_fail_dequeue;
  uint32_t stat_rtp_retransmit_fail_obsolete;
  uint32_t stat_nack_received;
};

struct mt_rtcp_rx {
  struct mtl_main_impl* parent;
  enum mtl_port port;
  struct mt_rtcp_nack_list nack_list;
  uint16_t max_retry;
  uint16_t last_seq_id;
  struct mt_udp_hdr udp_hdr;
  char name[MT_RTCP_MAX_NAME_LEN];
  uint32_t ssrc;

  uint16_t ipv4_packet_id;
  uint64_t nacks_send_time;
  uint64_t nacks_send_interval;
  uint64_t nack_expire_interval;

  /* stat */
  uint32_t stat_rtp_received;
  uint32_t stat_rtp_lost_detected;
  uint32_t stat_rtp_retransmit_succ;
  uint32_t stat_nack_sent;
  uint32_t stat_nack_drop_expire;
  uint32_t stat_nack_drop_oor;
  uint32_t stat_nack_drop_exceed;
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