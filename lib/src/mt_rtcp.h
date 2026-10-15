/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#ifndef _MT_LIB_RTCP_HEAD_H_
#define _MT_LIB_RTCP_HEAD_H_

#include "mt_main.h"

#define MT_RTCP_PTYPE_NACK (204)
#define MT_RTCP_MAX_NAME_LEN (24)
#define MT_RTCP_MAX_FCIS (256)
/* max mbufs of one retransmit chunk, sizes the on-stack arrays */
#define MT_RTCP_RETRANSMIT_BULK (32)
/* dbg logs 1 in this many invalid rtcp packets of each drop reason */
#define MT_RTCP_DROP_SAMPLE (100)

enum mt_rtcp_drop_reason {
  MT_RTCP_DROP_SHORT = 0, /* shorter than the rtcp header */
  MT_RTCP_DROP_FLAGS,     /* flags are not 0x80 */
  MT_RTCP_DROP_NAME,      /* nack name is not IMTL */
  MT_RTCP_DROP_LEN,       /* len field under the header or past the received bytes */
  MT_RTCP_DROP_SSRC,      /* nack ssrc does not match the session ssrc (RFC4585) */
  MT_RTCP_DROP_MAX,
};

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
  const char* name;                          /* short and unique name for each session */
  struct mt_udp_hdr* udp_hdr;                /* headers including eth, ipv4 and udp */
  uint32_t ssrc;                             /* ssrc of rtp session */
  uint16_t buffer_size;                      /* max number of buffered rtp packets */
  enum mtl_port port;                        /* port of rtp session */
  enum mt_rtp_payload_format payload_format; /* payload format */
  bool ssrc_check; /* drop a nack whose ssrc does not match ssrc (RFC4585) */
};

struct mt_rtcp_rx_ops {
  const char* name;             /* short and unique name for each session */
  struct mt_udp_hdr* udp_hdr;   /* headers including eth, ipv4 and udp */
  uint64_t nacks_send_interval; /* nack sending interval */
  enum mtl_port port;           /* port of rtp session */
  uint16_t seq_bitmap_size;     /* bitmap size of detecting window, can hold n * 8 seq */
  uint16_t seq_skip_window;     /* skip some seq to handle out of order while detecting */
};

struct mt_rtcp_tx {
  struct mtl_main_impl* parent;
  enum mtl_port port;
  struct mt_u64_fifo* mbuf_ring;
  struct rte_mempool* mbuf_pool;
  struct mt_txq_entry* mbuf_queue;
  struct mt_udp_hdr udp_hdr;
  char name[MT_RTCP_MAX_NAME_LEN];
  uint32_t ssrc;
  bool active;
  bool ssrc_check; /* drop a nack whose ssrc does not match ssrc (RFC4585) */
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
  uint32_t stat_nack_drop_invalid;
  /* invalid rtcp per reason, dbg logs 1 in MT_RTCP_DROP_SAMPLE of each reason */
  uint32_t stat_nack_drop_reason[MT_RTCP_DROP_MAX]; /* reset by rtcp_tx_stat() */
  uint32_t nack_drop_seen[MT_RTCP_DROP_MAX];        /* since create */
  bool nack_drop_sampled;                           /* the last parse logged a sample */
  /* cumulative since create, not reset by rtcp_tx_stat(), read by the public
   * st20_tx_get_session_stats() through mt_rtcp_tx_read_stats() */
  uint32_t nack_recv_seen;       /* nacks that passed every guard */
  uint32_t retransmit_succ_seen; /* rtp packets retransmitted ok */
};

/* Cumulative tx rtcp counters since create. The windowed stat_* fields reset
 * every rtcp_tx_stat(), so a stats getter reads these instead. */
struct mt_rtcp_tx_stats {
  uint64_t nack_received;     /* nacks that passed every guard */
  uint64_t nack_drop_invalid; /* nacks dropped, all reasons */
  uint64_t nack_drop_ssrc;    /* nacks dropped by the RFC4585 ssrc check */
  uint64_t retransmit;        /* rtp packets retransmitted ok */
};

struct mt_rtcp_rx {
  struct mtl_main_impl* parent;
  enum mtl_port port;
  struct mt_udp_hdr udp_hdr;
  char name[MT_RTCP_MAX_NAME_LEN];
  uint64_t nacks_send_interval;
  uint32_t ssrc;
  bool active;

  uint64_t nacks_send_time;

  uint16_t last_seq;
  uint16_t last_cont;
  uint8_t* seq_bitmap;
  uint16_t seq_bitmap_size; /* length of seq_bitmap in bytes */
  uint16_t seq_window_size;
  uint16_t seq_skip_window;

  /* stat */
  uint32_t stat_rtp_received;
  uint32_t stat_rtp_lost_detected;
  uint32_t stat_nack_sent;
  uint32_t stat_nack_drop_exceed;
};

struct mt_rtcp_tx* mt_rtcp_tx_create(struct mtl_main_impl* mtl,
                                     struct mt_rtcp_tx_ops* ops);
void mt_rtcp_tx_free(struct mt_rtcp_tx* tx);
void mt_rtcp_tx_read_stats(struct mt_rtcp_tx* tx, struct mt_rtcp_tx_stats* stats);

struct mt_rtcp_rx* mt_rtcp_rx_create(struct mtl_main_impl* mtl,
                                     struct mt_rtcp_rx_ops* ops);
void mt_rtcp_rx_free(struct mt_rtcp_rx* rx);

int mt_rtcp_tx_buffer_rtp_packets(struct mt_rtcp_tx* tx, struct rte_mbuf** mbufs,
                                  unsigned int bulk);
int mt_rtcp_tx_parse_rtcp_packet(struct mt_rtcp_tx* tx, struct mt_rtcp_hdr* rtcp,
                                 size_t len);

int mt_rtcp_rx_parse_rtp_packet(struct mt_rtcp_rx* rx, struct st_rfc3550_rtp_hdr* rtp);
int mt_rtcp_rx_send_nack_packet(struct mt_rtcp_rx* rx);

#endif