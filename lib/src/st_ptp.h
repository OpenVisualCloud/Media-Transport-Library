/*
 * Copyright (C) 2021 Intel Corporation.
 *
 * This software and the related documents are Intel copyrighted materials,
 * and your use of them is governed by the express license under which they
 * were provided to you ("License").
 * Unless the License provides otherwise, you may not use, modify, copy,
 * publish, distribute, disclose or transmit this software or the related
 * documents without Intel's prior written permission.
 *
 * This software and the related documents are provided as is, with no
 * express or implied warranties, other than those that are expressly stated
 * in the License.
 *
 */

#ifndef _ST_LIB_PTP_HEAD_H_
#define _ST_LIB_PTP_HEAD_H_

#include "st_main.h"

#define ST_PTP_DELAY_REQ_US (50)
#define ST_PTP_DELAY_REQ_MONITOR_US (1000 * 1)
#define ST_PTP_DELAY_STEP_US (10)
#define ST_PTP_CLOCK_IDENTITY_MAGIC (0xfeff)

#define ST_PTP_STAT_INTERVAL_S (10) /* 10s */
#define ST_PTP_STAT_INTERVAL_US (ST_PTP_STAT_INTERVAL_S * US_PER_S)

#define ST_PTP_RX_BURST_SIZE (4)

enum st_ptp_msg {
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

enum st_ptp_udp_ports {
  ST_PTP_UDP_EVENT_PORT = 319,
  ST_PTP_UDP_GEN_PORT = 320,
  ST_PTP_UDP_MULTICAST_GEN_MSG_PORT = ST_PTP_UDP_GEN_PORT,
  ST_PTP_UDP_UNICAST_CLK_GEN_PORT = ST_PTP_UDP_GEN_PORT,
};

struct st_ptp_header {
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
  struct st_ptp_port_id source_port_identity;
  uint16_t sequence_id;
  uint8_t control_field;
  int8_t log_message_interval;
} __attribute__((packed));

struct st_ptp_tmstamp {
  uint16_t sec_msb;
  uint32_t sec_lsb;
  uint32_t ns;
} __attribute__((packed));

struct st_ptp_sync_msg {
  struct st_ptp_header hdr;
  struct st_ptp_tmstamp origin_timestamp;
} __attribute__((packed));

struct st_ptp_follow_up_msg {
  struct st_ptp_header hdr;
  struct st_ptp_tmstamp precise_origin_timestamp;
  uint8_t suffix[0];
} __attribute__((packed));

struct st_ptp_clock_quality {
  uint8_t clock_class;
  uint8_t clock_accuracy;
  uint16_t offset_scaled_log_variance;
} __attribute__((packed));

struct st_ptp_announce_msg {
  struct st_ptp_header hdr;
  struct st_ptp_tmstamp origin_timestamp;
  int16_t current_utc_offset;
  uint8_t reserved;
  uint8_t grandmaster_priority1;
  struct st_ptp_clock_quality grandmaster_clock_quality;
  uint8_t grandmaster_priority2;
  struct st_ptp_clock_id grandmaster_identity;
  uint16_t steps_removed;
  uint8_t time_source;
  uint8_t suffix[0];
} __attribute__((packed));

struct st_ptp_delay_resp_msg {
  struct st_ptp_header hdr;
  struct st_ptp_tmstamp receive_timestamp;
  struct st_ptp_port_id requesting_port_identity;
  uint8_t suffix[0];
} __attribute__((packed));

static inline struct st_ptp_impl* st_get_ptp(struct st_main_impl* impl,
                                             enum st_port port) {
  return &impl->ptp[port];
}

int st_ptp_init(struct st_main_impl* impl);
int st_ptp_uinit(struct st_main_impl* impl);

int st_ptp_parse(struct st_ptp_impl* ptp, struct st_ptp_header* hdr, bool vlan,
                 enum st_ptp_l_mode mode, uint16_t timesync,
                 struct st_ptp_ipv4_udp* ipv4_hdr);

void st_ptp_stat(struct st_main_impl* impl);

#endif
