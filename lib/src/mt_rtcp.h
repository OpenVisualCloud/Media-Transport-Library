/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#ifndef _MT_LIB_RTCP_HEAD_H_
#define _MT_LIB_RTCP_HEAD_H_

#include "mt_main.h"

#define MT_RTCP_PTYPE_NACK (204)
#define MT_RTCP_MAX_NAME_LEN (24)
#define MT_RTCP_MAX_FCIS (256)

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

enum mt_rtp_payload_format {
  MT_RTP_PAYLOAD_FORMAT_RAW = 0,
  MT_RTP_PAYLOAD_FORMAT_RFC4175,
  MT_RTP_PAYLOAD_FORMAT_RFC9134,
};

struct mt_rtcp_tx_ops {
  const char *name;                          /* short and unique name for each session */
  struct mt_udp_hdr *udp_hdr;                /* headers including eth, ipv4 and udp */
  uint32_t ssrc;                             /* ssrc of rtp session */
  uint16_t buffer_size;                      /* max number of buffered rtp packets */
  enum mtl_port port;                        /* port of rtp session */
  enum mt_rtp_payload_format payload_format; /* payload format */
};

struct mt_rtcp_rx_ops {
  const char *name;             /* short and unique name for each session */
  struct mt_udp_hdr *udp_hdr;   /* headers including eth, ipv4 and udp */
  uint64_t nacks_send_interval; /* nack sending interval */
  enum mtl_port port;           /* port of rtp session */
  uint16_t seq_bitmap_size;     /* bitmap size of detecting window, can hold n * 8 seq */
  uint16_t seq_skip_window;     /* skip some seq to handle out of order while
                                   detecting */
};

struct mt_rtcp_tx {
  struct mtl_main_impl *parent;
  enum mtl_port port;
  struct mt_u64_fifo *mbuf_ring;
  struct rte_mempool *mbuf_pool;
  struct mt_txq_entry *mbuf_queue;
  struct mt_udp_hdr udp_hdr;
  char name[MT_RTCP_MAX_NAME_LEN];
  uint32_t ssrc;
  bool active;
  enum mt_rtp_payload_format payload_format;

  uint16_t last_seq_num;

  /* stat */
  uint32_t stat_rtp_sent;
  uint32_t stat_rtp_retransmit_succ;
  uint32_t stat_rtp_retransmit_fail;
  uint32_t stat_rtp_retransmit_fail_nobuf;
  uint32_t stat_rtp_retransmit_fail_read;
  uint32_t stat_rtp_retransmit_fail_obsolete;
  uint32_t stat_rtp_retransmit_fail_burst;
  uint32_t stat_nack_received;
};

struct mt_rtcp_rx {
  struct mtl_main_impl *parent;
  enum mtl_port port;
  struct mt_udp_hdr udp_hdr;
  char name[MT_RTCP_MAX_NAME_LEN];
  uint64_t nacks_send_interval;
  uint32_t ssrc;
  bool active;

  uint64_t nacks_send_time;

  uint16_t last_seq;
  uint16_t last_cont;
  uint8_t *seq_bitmap;
  uint16_t seq_window_size;
  uint16_t seq_skip_window;

  /* stat */
  uint32_t stat_rtp_received;
  uint32_t stat_rtp_lost_detected;
  uint32_t stat_nack_sent;
  uint32_t stat_nack_drop_exceed;
};

struct mt_rtcp_tx *mt_rtcp_tx_create(struct mtl_main_impl *mtl,
                                     struct mt_rtcp_tx_ops *ops);
void mt_rtcp_tx_free(struct mt_rtcp_tx *tx);

struct mt_rtcp_rx *mt_rtcp_rx_create(struct mtl_main_impl *mtl,
                                     struct mt_rtcp_rx_ops *ops);
void mt_rtcp_rx_free(struct mt_rtcp_rx *rx);

int mt_rtcp_tx_buffer_rtp_packets(struct mt_rtcp_tx *tx, struct rte_mbuf **mbufs,
                                  unsigned int bulk);
int mt_rtcp_tx_parse_rtcp_packet(struct mt_rtcp_tx *tx, struct mt_rtcp_hdr *rtcp);

int mt_rtcp_rx_parse_rtp_packet(struct mt_rtcp_rx *rx, struct st_rfc3550_rtp_hdr *rtp);
int mt_rtcp_rx_send_nack_packet(struct mt_rtcp_rx *rx);

#endif