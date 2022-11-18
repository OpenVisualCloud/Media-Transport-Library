/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef _MT_LIB_PTP_HEAD_H_
#define _MT_LIB_PTP_HEAD_H_

#include "mt_main.h"

#define MT_PTP_DELAY_REQ_US (50)
#define MT_PTP_DELAY_REQ_MONITOR_US (1000 * 1)
#define MT_PTP_DELAY_STEP_US (10)
#define MT_PTP_CLOCK_IDENTITY_MAGIC (0xfeff)

#define MT_PTP_STAT_INTERVAL_S (10) /* 10s */
#define MT_PTP_STAT_INTERVAL_US (MT_PTP_STAT_INTERVAL_S * US_PER_S)

#define MT_PTP_RX_BURST_SIZE (4)

enum mt_ptp_msg {
  PTP_SYNC = 0,
  PTP_DELAY_REQ = 1,
  PTP_PDELAY_REQ = 2,
  PTP_PDELAY_RESP = 3,
  PTP_FOLLOW_UP = 8,
  PTP_DELAY_RESP = 9,
  PTP_PDELAY_RESP_FOLLOW_UP = 10,
  PTP_ANNOUNCE = 11,
  PTP_SIGNALING = 12,
  PTP_MANAGEMENT = 13,
};

enum mt_ptp_udp_ports {
  MT_PTP_UDP_EVENT_PORT = 319,
  MT_PTP_UDP_GEN_PORT = 320,
  MT_PTP_UDP_MULTICAST_GEN_MSG_PORT = MT_PTP_UDP_GEN_PORT,
  MT_PTP_UDP_UNICAST_CLK_GEN_PORT = MT_PTP_UDP_GEN_PORT,
};

struct mt_ptp_header {
  struct {
    uint8_t message_type : 4;
    uint8_t transport_specific : 4;
  };
  struct {
    uint8_t version : 4;
    uint8_t reserved0 : 4;
  };
  uint16_t message_length;
  uint8_t domain_number;
  uint8_t reserved1;
  uint16_t flag_field;
  int64_t correction_field;
  uint32_t reserved2;
  struct mt_ptp_port_id source_port_identity;
  uint16_t sequence_id;
  uint8_t control_field;
  int8_t log_message_interval;
} __attribute__((packed));

struct mt_ptp_tmstamp {
  uint16_t sec_msb;
  uint32_t sec_lsb;
  uint32_t ns;
} __attribute__((packed));

struct mt_ptp_sync_msg {
  struct mt_ptp_header hdr;
  struct mt_ptp_tmstamp origin_timestamp;
} __attribute__((packed));

struct mt_ptp_follow_up_msg {
  struct mt_ptp_header hdr;
  struct mt_ptp_tmstamp precise_origin_timestamp;
  uint8_t suffix[0];
} __attribute__((packed));

struct mt_ptp_clock_quality {
  uint8_t clock_class;
  uint8_t clock_accuracy;
  uint16_t offset_scaled_log_variance;
} __attribute__((packed));

struct mt_ptp_announce_msg {
  struct mt_ptp_header hdr;
  struct mt_ptp_tmstamp origin_timestamp;
  int16_t current_utc_offset;
  uint8_t reserved;
  uint8_t grandmaster_priority1;
  struct mt_ptp_clock_quality grandmaster_clock_quality;
  uint8_t grandmaster_priority2;
  struct mt_ptp_clock_id grandmaster_identity;
  uint16_t steps_removed;
  uint8_t time_source;
  uint8_t suffix[0];
} __attribute__((packed));

struct mt_ptp_delay_resp_msg {
  struct mt_ptp_header hdr;
  struct mt_ptp_tmstamp receive_timestamp;
  struct mt_ptp_port_id requesting_port_identity;
  uint8_t suffix[0];
} __attribute__((packed));

static inline struct mt_ptp_impl* mt_get_ptp(struct mtl_main_impl* impl,
                                             enum mtl_port port) {
  return &impl->ptp[port];
}

int mt_ptp_init(struct mtl_main_impl* impl);
int mt_ptp_uinit(struct mtl_main_impl* impl);

int mt_ptp_parse(struct mt_ptp_impl* ptp, struct mt_ptp_header* hdr, bool vlan,
                 enum mt_ptp_l_mode mode, uint16_t timesync,
                 struct mt_ptp_ipv4_udp* ipv4_hdr);

void mt_ptp_stat(struct mtl_main_impl* impl);

#endif
