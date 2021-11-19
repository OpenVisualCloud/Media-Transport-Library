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

#include <rte_alarm.h>
#include <rte_arp.h>
#include <rte_ethdev.h>
#include <rte_kni.h>
#include <rte_tm.h>
#include <rte_version.h>
#include <st_dpdk_api.h>
#include <sys/file.h>
#include <unistd.h>

#include "st_fmt.h"
#include "st_mem.h"
#include "st_pkt.h"
#include "st_platform.h"

#ifndef _ST_LIB_MAIN_HEAD_H_
#define _ST_LIB_MAIN_HEAD_H_

#define ST_MBUF_CACHE_SIZE (128)
#define ST_MBUF_SIZE (RTE_MBUF_DEFAULT_BUF_SIZE) /* 2176 (2048 + 128) */
#define ST_MBUF_POOL_SIZE_10M (4096)             /* 10M with 5 2M hugepage, 4096x2156 */

#define ST_MAX_NAME_LEN (32)

#define ST_MCAST_POOL_INC (32)

#define ST_MAX_SCH_NUM (3)          /* max 3 scheduler lcore */
#define ST_MAX_TASKLET_PER_SCH (16) /* max 8 tasklet in one scheduler lcore */

#define ST_SCH_MAX_TX_VIDEO_SESSIONS (32) /* max video tx sessions per sch lcore */
#define ST_SCH_MAX_RX_VIDEO_SESSIONS (32) /* max video rx sessions per sch lcore */
#define ST_SESSION_MAX_BULK (4)
/* number of tmstamp it will tracked for out of order pkts */
#define ST_VIDEO_RX_REC_NUM_OFO (2)

/* max tx audio(st_30) sessions */
#define ST_MAX_TX_AUDIO_SESSIONS (32)
#define ST_MAX_RX_AUDIO_SESSIONS (32)
/* max tx anc(st_40) sessions */
#define ST_MAX_TX_ANC_SESSIONS (32)
#define ST_MAX_RX_ANC_SESSIONS (32)

/* max RL items */
#define ST_MAX_RL_ITEMS (16)

#define ST_ARRAY_SIZE(arr) (sizeof(arr) / sizeof(arr[0]))

#define ST_MCAST_GROUP_MAX (32)

#define ST_IP_DONT_FRAGMENT_FLAG (0x0040)

/* Port supports Rx queue setup after device started. */
#define ST_IF_FEATURE_RUNTIME_RX_QUEUE (0x00000001)
/* Timesync enabled on the port */
#define ST_IF_FEATURE_TIMESYNC (0x00000002)
/* Port registers Rx timestamp in mbuf dynamic field */
#define ST_IF_FEATURE_RX_OFFLOAD_TIMESTAMP (0x00000004)

struct st_main_impl; /* foward declare */

struct st_muf_priv_data {
  uint64_t resv1[14];  /* dynamic fields are implemented after rte_mbuf */
  uint64_t idx;        /* index inside curren frame */
  uint64_t time_stamp; /* tsc time stamp of current mbuf */
};

struct st_ptp_clock_id {
  uint8_t id[8];
};

struct st_ptp_port_id {
  struct st_ptp_clock_id clock_identity;
  uint16_t port_number;
} __attribute__((packed));

struct st_ptp_ipv4_udp {
  struct rte_ipv4_hdr ip;
  struct rte_udp_hdr udp;
} __attribute__((__packed__));

enum st_port_type {
  ST_PORT_ERR = 0,
  ST_PORT_VF,
  ST_PORT_PF,
};

enum st_session_port {
  ST_SESSION_PORT_P = 0, /* primary session(logical) port */
  ST_SESSION_PORT_R,     /* redundant session(logical) port */
  ST_SESSION_PORT_MAX,
};

enum st_ptp_l_mode {
  ST_PTP_L2 = 0,
  ST_PTP_L4,
  ST_PTP_MAX_MODE,
};

enum st_ptp_addr_mode {
  ST_PTP_MULTICAST_ADDR = 0,
  ST_PTP_UNICAST_ADDR,
};

enum st21_tx_frame_status {
  ST21_TX_STAT_UNKNOWN = 0,
  ST21_TX_STAT_WAIT_FRAME,
  ST21_TX_STAT_SENDING_PKTS,
  ST21_TX_STAT_WAIT_PKTS,
};

enum st30_tx_frame_status {
  ST30_TX_STAT_UNKNOWN = 0,
  ST30_TX_STAT_WAIT_FRAME,
  ST30_TX_STAT_SENDING_PKTS,
};

enum st40_tx_frame_status {
  ST40_TX_STAT_UNKNOWN = 0,
  ST40_TX_STAT_WAIT_FRAME,
  ST40_TX_STAT_SENDING_PKTS,
};

enum st21_tx_pacing_way {
  ST21_TX_PACING_WAY_AUTO = 0,
  ST21_TX_PACING_WAY_RL,  /* rate limit based pacing */
  ST21_TX_PACING_WAY_TSC, /* tsc based pacing, fall back path if no rl */
  ST21_TX_PACING_WAY_MAX,
};

struct st_ptp_impl {
  struct st_main_impl* impl;
  enum st_port port;
  uint16_t port_id;
  uint16_t tx_queue_id;
  bool tx_queue_active;
  uint16_t rx_queue_id;
  bool rx_queue_active;
  struct rte_mempool* mbuf_pool;

  uint8_t mcast_group_addr[ST_IP_ADDR_LEN]; /* 224.0.1.129 */
  bool master_initialized;
  struct st_ptp_port_id master_port_id;
  struct rte_ether_addr master_addr;
  struct st_ptp_port_id our_port_id;
  struct st_ptp_ipv4_udp dst_udp;   /* for l4 */
  uint8_t sip_addr[ST_IP_ADDR_LEN]; /* source IP */
  enum st_ptp_addr_mode master_addr_mode;
  int16_t master_utc_offset; /* offset to UTC of current master PTP */
  int64_t ptp_delta;         /* current delta for PTP */

  uint64_t t1;
  uint8_t t1_domain_number;
  uint64_t t2;
  bool t2_vlan;
  uint16_t t2_sequence_id;
  enum st_ptp_l_mode t2_mode;
  uint64_t t3;
  uint16_t t3_sequence_id;
  uint64_t t4;
  /* result */
  uint64_t delta_result_cnt;
  uint64_t delta_result_sum;
  uint64_t delta_result_err;

  /* status */
  int64_t stat_delta_min;
  int64_t stat_delta_max;
  int64_t stat_delta_cnt;
  int64_t stat_delta_sum;
  int64_t stat_rx_sync_err;
  int64_t stat_result_err;
};

struct st_cni_impl {
  uint16_t rx_q_id[ST_PORT_MAX]; /* cni rx queue id */
  bool rx_q_active[ST_PORT_MAX];
  pthread_t tid; /* thread id for rx */
  rte_atomic32_t stop_thread;
  bool lcore_tasklet;
  /* stat */
  int eth_rx_cnt[ST_PORT_MAX];
#ifdef ST_HAS_KNI
  bool has_kni_kmod;
  rte_atomic32_t if_up[ST_PORT_MAX];
  struct rte_kni_conf conf[ST_PORT_MAX];
  struct rte_kni* rkni[ST_PORT_MAX];
  pthread_t kni_bkg_tid; /* bkg thread id for kni */
  rte_atomic32_t stop_kni;
  uint16_t tx_q_id[ST_PORT_MAX]; /* cni tx queue id */
  bool tx_q_active[ST_PORT_MAX];
  int kni_rx_cnt[ST_PORT_MAX];
#endif
};

struct st_arp_impl {
  uint32_t ip[ST_PORT_MAX];
  struct rte_ether_addr ea[ST_PORT_MAX];
  rte_atomic32_t mac_ready[ST_PORT_MAX];
  uint16_t tx_q_id[ST_PORT_MAX]; /* arp tx queue id */
  bool tx_q_active[ST_PORT_MAX];
};

struct st_mcast_impl {
  pthread_mutex_t group_mutex[ST_PORT_MAX];
  uint32_t group_ip[ST_PORT_MAX][ST_MCAST_GROUP_MAX];
  uint16_t group_num[ST_PORT_MAX];
  uint16_t tx_q_id[ST_PORT_MAX]; /* mcast tx queue id */
  bool tx_q_active[ST_PORT_MAX];
};

/*
 * Tasklets share the time slot on a lcore,
 * only non-block method can be used in handler routine.
 */
struct st_sch_tasklet_ops {
  char* name;
  void* priv; /* private data to the callback */

  int (*pre_start)(void* priv);
  int (*start)(void* priv);
  int (*stop)(void* priv);
  int (*handler)(void* priv);
};

struct st_sch_tasklet_impl {
  struct st_sch_tasklet_ops ops;
  char name[ST_MAX_NAME_LEN];

  int idx;
};

enum st_session_type {
  ST_SESSION_TYPE_UNKNOWN = 0,
  ST_SESSION_TYPE_TX_VIDEO,
  ST_SESSION_TYPE_TX_AUDIO,
  ST_SESSION_TYPE_TX_ANC,
  ST22_SESSION_TYPE_TX_VIDEO,
  ST_SESSION_TYPE_RX_VIDEO,
  ST_SESSION_TYPE_RX_AUDIO,
  ST_SESSION_TYPE_RX_ANC,
  ST22_SESSION_TYPE_RX_VIDEO,
  ST_SESSION_TYPE_MAX,
};

/* timing for Packet Read Schedule */
struct st_tx_video_pacing {
  double trs;             /* in ns for of 2 consecutive packets, T-Frame / N-Packets */
  double tr_offset;       /* in ns, tr offset time of each frame */
  uint32_t tr_offset_vrx; /* packets unit, VRX start value of each frame */
  double frame_time;      /* time of the frame in nanoseconds */
  double frame_time_sampling; /* time of the frame in sampling(90k) */
  uint32_t warm_pkts;         /* pkts for RL pacing warm boot */
  float pad_interval;         /* padding pkt interval(pkts level) for RL pacing */

  uint64_t cur_epochs;     /* epoch of current frame */
  uint32_t cur_time_stamp; /* The Timestamp field shall contain the RTP Timestamp as
                              specified in SMPTE ST 2110-10, 90K sample */
  double tsc_time_cursor;  /* in ns, tsc time cursor for packet pacing */
};

struct st_tx_video_session_impl {
  enum st_port port_maps[ST_SESSION_PORT_MAX];
  struct rte_ring* ring[ST_SESSION_PORT_MAX];
  struct rte_ring* packet_ring;
  uint16_t port_id[ST_SESSION_PORT_MAX];
  uint16_t queue_id[ST_SESSION_PORT_MAX];
  uint16_t queue_active[ST_SESSION_PORT_MAX];
  int idx; /* index for current tx_session */

  uint16_t st20_ipv4_packet_id;
  uint16_t st20_src_port[ST_SESSION_PORT_MAX]; /* udp port */
  uint16_t st20_dst_port[ST_SESSION_PORT_MAX]; /* udp port */
  struct st_rfc4175_hdr_single s_hdr[ST_SESSION_PORT_MAX];

  struct st_tx_video_pacing pacing;

  struct st20_tx_ops ops;
  char ops_name[ST_MAX_NAME_LEN];

  unsigned int bulk; /* Enqueue bulk objects on the ring */
  struct rte_mbuf* inflight[ST_SESSION_PORT_MAX][ST_SESSION_MAX_BULK];
  bool has_inflight[ST_SESSION_PORT_MAX];
  int inflight_cnt[ST_SESSION_PORT_MAX]; /* for stats */

  /* info for transmitter */
  uint64_t trs_target_tsc[ST_SESSION_PORT_MAX];
  struct rte_mbuf* trs_inflight[ST_SESSION_PORT_MAX][ST_SESSION_MAX_BULK];
  unsigned int trs_inflight_num[ST_SESSION_PORT_MAX];
  unsigned int trs_inflight_idx[ST_SESSION_PORT_MAX];
  unsigned int trs_pad_inflight_num[ST_SESSION_PORT_MAX]; /* inflight padding */
  int trs_inflight_cnt[ST_SESSION_PORT_MAX];              /* for stats */
  struct rte_mbuf* trs_inflight2[ST_SESSION_PORT_MAX][ST_SESSION_MAX_BULK];
  unsigned int trs_inflight_num2[ST_SESSION_PORT_MAX];
  unsigned int trs_inflight_idx2[ST_SESSION_PORT_MAX];
  int trs_inflight_cnt2[ST_SESSION_PORT_MAX]; /* for stats */

  /* st20 frame info */
  size_t st20_frame_size;   /* size per frame */
  uint16_t st20_frames_cnt; /* numbers of frames requested */
  void** st20_frames;
  struct rte_mbuf_ext_shared_info** st20_frames_sh_info;
  rte_iova_t* st20_frames_iova;
  uint16_t st20_frame_idx; /* current frame index */
  enum st21_tx_frame_status st20_frame_stat;

  struct st20_pgroup st20_pg;
  struct st21_timing st21_tm;
  struct st_fps_timing fps_tm;
  int st20_pkts_in_line;      /* number of packets per each line, 4 for 1080p */
  uint32_t st20_pkt_len;      /* data len(byte) for each pkt */
  uint32_t st20_pkt_size;     /* size for each pkt which include the header */
  uint32_t st20_avg_pkt_size; /* avg size for pkts of frame */
  int st20_total_pkts;        /* total pkts in one frame, ex: 4320 for 1080p */
  int st20_pkt_idx;           /* pkt index in current frame */
  uint32_t st20_seq_id;       /* seq id for each pkt */
  uint32_t st20_rtp_time;     /* keep track of rtp time */
  int st21_vrx_narrow;        /* pass criteria for narrow */
  int st21_vrx_wide;          /* pass criteria for wide */

  struct rte_mbuf* pad[ST_SESSION_PORT_MAX];

  /* stat */
  int st20_stat_frame_cnt;
  int st20_stat_pkts_build;
  int st20_stat_pkts_burst;
  uint64_t st20_stat_last_time;
  uint32_t st20_epoch_mismatch;    /* mismatch the epoch */
  uint32_t st20_troffset_mismatch; /* troffset mismatch */
};

struct st_tx_video_sessions_mgr {
  struct st_main_impl* parnet;
  int idx; /* index for current session mgr */

  struct st_tx_video_session_impl sessions[ST_SCH_MAX_TX_VIDEO_SESSIONS];
  bool active[ST_SCH_MAX_TX_VIDEO_SESSIONS]; /* active map for sessions */
};

struct st_video_transmitter_impl {
  struct st_main_impl* parnet;
  struct st_tx_video_sessions_mgr* mgr;
  int idx; /* index for current transmitter */
};

struct st_rx_video_slot_impl {
  int idx;
  uint32_t tmstamp;
  uint16_t seq_id_base; /* seq id for the first packt */
  bool seq_id_got;
  void* frame; /* only for frame type */
  uint8_t* frame_bitmap;
  size_t frame_recv_size; /* only for frame type */
};

struct st_rx_video_ebu_info {
  double trs;        /* in ns for of 2 consecutive packets, T-Frame / N-Packets */
  double tr_offset;  /* in ns, tr offset time of each frame */
  double frame_time; /* time of the frame in nanoseconds */
  double frame_time_sampling; /* time of the frame in sampling(90k) */

  // pass criteria
  uint32_t c_max_narrow_pass;
  uint32_t c_max_wide_pass;
  uint32_t vrx_full_narrow_pass;
  uint32_t vrx_full_wide_pass;
  uint32_t rtp_offset_max_pass;
};

struct st_rx_video_ebu_stat {
  uint64_t cur_epochs; /* epoch of current frame */
  int frame_idx;

  /* Cinst, packet level check */
  uint64_t cinst_initial_time;
  int32_t cinst_max;
  int32_t cinst_min;
  uint32_t cinst_cnt;
  int64_t cinst_sum;
  float cinst_avg;

  /* vrx, packet level check */
  int32_t vrx_drained_prev;
  int32_t vrx_prev;
  int32_t vrx_max;
  int32_t vrx_min;
  uint32_t vrx_cnt;
  int64_t vrx_sum;
  float vrx_avg;

  /* fpt(first packet timestamp), frame level check, pass criteria < TR_OFFSET */
  int32_t fpt_max;
  int32_t fpt_min;
  uint32_t fpt_cnt;
  int64_t fpt_sum;
  float fpt_avg;

  /* latency(== fpt), frame level check */
  int32_t latency_max;
  int32_t latency_min;
  uint32_t latency_cnt;
  int64_t latency_sum;
  float latency_avg;

  /* rtp offset, frame level check */
  int32_t rtp_offset_max;
  int32_t rtp_offset_min;
  uint32_t rtp_offset_cnt;
  int64_t rtp_offset_sum;
  float rtp_offset_avg;

  /* Inter-frame RTP TS Delta, frame level check */
  uint32_t prev_rtp_ts;
  int32_t rtp_ts_delta_max;
  int32_t rtp_ts_delta_min;
  uint32_t rtp_ts_delta_cnt;
  int64_t rtp_ts_delta_sum;
  float rtp_ts_delta_avg;

  /* Inter-packet time(ns), packet level check */
  uint64_t prev_rtp_ipt_ts;
  int32_t rtp_ipt_max;
  int32_t rtp_ipt_min;
  uint32_t rtp_ipt_cnt;
  int64_t rtp_ipt_sum;
  float rtp_ipt_avg;
};

struct st_rx_video_session_impl {
  int idx; /* index for current session */
  struct st20_rx_ops ops;
  char ops_name[ST_MAX_NAME_LEN];

  enum st_port port_maps[ST_SESSION_PORT_MAX];
  uint16_t queue_id[ST_SESSION_PORT_MAX]; /* queue id for the session */
  bool queue_active[ST_SESSION_PORT_MAX];
  uint16_t port_id[ST_SESSION_PORT_MAX];
  uint16_t st20_src_port[ST_SESSION_PORT_MAX]; /* udp port */
  uint16_t st20_dst_port[ST_SESSION_PORT_MAX]; /* udp port */

  /* st21 frames info */
  size_t st20_frame_size;        /* size per frame */
  size_t st20_frame_bitmap_size; /* bitmap size per frame */
  int st20_frames_cnt;           /* numbers of frames requested */
  void** st20_frames;
  rte_atomic32_t* st20_frames_refcnt;
  struct st20_pgroup st20_pg;

  /* st21 rtp info */
  struct rte_ring* st20_rtps_ring;

  /* record two frames in case pkts out of order within marker */
  struct st_rx_video_slot_impl slots[ST_VIDEO_RX_REC_NUM_OFO];
  int slot_idx;

  /* status */
  int st20_stat_pkts_idx_dropped;
  int st20_stat_pkts_offset_dropped;
  int st20_stat_pkts_redunant_dropped;
  int st20_stat_pkts_received;
  int st20_stat_pkts_rtp_ring_full;
  int st20_stat_frames_dropped;
  int st20_stat_frames_received;
  uint64_t st20_stat_last_time;
  struct st_rx_video_ebu_info ebu_info;
  struct st_rx_video_ebu_stat ebu;
};

struct st_rx_video_sessions_mgr {
  struct st_main_impl* parnet;
  int idx; /* index for current session mgr */

  struct st_rx_video_session_impl sessions[ST_SCH_MAX_RX_VIDEO_SESSIONS];
  bool active[ST_SCH_MAX_RX_VIDEO_SESSIONS]; /* active map for sessions */
};

struct st_sch_impl {
  struct st_sch_tasklet_impl tasklet[ST_MAX_TASKLET_PER_SCH];
  int num_tasklet;
  unsigned int lcore;

  int data_quota_mbs_total; /* total data quota(mb/s) for current sch */
  int data_quota_mbs_limit; /* limit data quota(mb/s) for current sch */

  struct st_main_impl* parnet;
  int idx; /* index for current sch */
  rte_atomic32_t started;
  rte_atomic32_t request_stop;
  rte_atomic32_t stopped;
  bool active; /* if this sch is active */
  rte_atomic32_t ref_cnt;

  /* one video transmitter for one sch */
  struct st_video_transmitter_impl video_transmitter;
  /* one tx sessions mgr for one sch */
  struct st_tx_video_sessions_mgr tx_video_mgr;
  bool tx_video_init;

  /* one rx video sessions mgr for one sch */
  struct st_rx_video_sessions_mgr rx_video_mgr;
  bool rx_video_init;
};

struct st_tx_audio_session_pacing {
  double trs;                 /* in ns for of 2 consecutive packets */
  double frame_time;          /* time of the frame in nanoseconds */
  double frame_time_sampling; /* time of the frame in sampling(48k) */
  uint64_t cur_epochs;        /* epoch of current frame */
  uint32_t cur_time_stamp;    /* RTP Timestamp as specified in SMPTE PCM audio signals */
  uint64_t tsc_time_cursor;   /* in ns, tsc time cursor for packet pacing */
};

struct st_tx_audio_session_impl {
  int idx; /* index for current session */
  struct st30_tx_ops ops;
  char ops_name[ST_MAX_NAME_LEN];
  enum st_port port_maps[ST_SESSION_PORT_MAX];
  struct rte_mbuf* inflight[ST_SESSION_PORT_MAX];
  bool has_inflight[ST_SESSION_PORT_MAX];
  int inflight_cnt[ST_SESSION_PORT_MAX]; /* for stats */
  struct rte_ring* packet_ring;

  void* st30_frames;
  int st30_frame_size;     /* size per frame*/
  uint16_t st30_frame_idx; /* current frame index */
  enum st30_tx_frame_status st30_frame_stat;

  uint16_t st30_ipv4_packet_id;
  uint16_t st30_src_port[ST_SESSION_PORT_MAX]; /* udp port */
  uint16_t st30_dst_port[ST_SESSION_PORT_MAX]; /* udp port */
  struct st_rfc3550_audio_hdr hdr[ST_SESSION_PORT_MAX];

  struct st_tx_audio_session_pacing pacing;

  uint32_t pkt_len;       /* data len(byte) for each pkt */
  uint32_t st30_pkt_size; /* size for each pkt which include the header */
  int st30_total_pkts;    /* total pkts in one frame */
  int st30_pkt_idx;       /* pkt index in current frame */
  uint16_t st30_seq_id;   /* seq id for each pkt */
  int st30_rtp_time;      /* record rtp time */

  /* stat */
  int st30_stat_frame_cnt;
  int st30_stat_pkt_cnt;
  uint32_t st30_epoch_mismatch; /* count of frame lower than pacing */
};

struct st_tx_audio_sessions_mgr {
  struct st_main_impl* parnet;
  int idx; /* index for current sessions mgr */
  /* all audio sessions share same ring/queue */
  struct rte_ring* ring[ST_PORT_MAX];
  uint16_t port_id[ST_PORT_MAX];
  uint16_t queue_id[ST_PORT_MAX];
  uint16_t queue_active[ST_PORT_MAX];
  struct st_tx_audio_session_impl sessions[ST_MAX_TX_AUDIO_SESSIONS];
  bool active[ST_MAX_TX_AUDIO_SESSIONS]; /* active map for sessions */

  /* status */
  int st30_stat_pkts_burst;
};

struct st_audio_transmitter_impl {
  struct st_main_impl* parnet;
  struct st_tx_audio_sessions_mgr* mgr;
  int idx; /* index for current transmitter */

  struct rte_mbuf* inflight[ST_PORT_MAX]; /* inflight mbuf */
  int inflight_cnt[ST_PORT_MAX];          /* for stats */
};

struct st_rx_audio_session_impl {
  int idx; /* index for current session */
  struct st30_rx_ops ops;
  char ops_name[ST_MAX_NAME_LEN];

  enum st_port port_maps[ST_SESSION_PORT_MAX];
  uint16_t queue_id[ST_SESSION_PORT_MAX]; /* queue id for the session */
  bool queue_active[ST_SESSION_PORT_MAX];
  uint16_t port_id[ST_SESSION_PORT_MAX];

  uint16_t st30_src_port[ST_SESSION_PORT_MAX]; /* udp port */
  uint16_t st30_dst_port[ST_SESSION_PORT_MAX]; /* udp port */

  void** st30_frames;
  void* st30_frame;
  int st30_frames_cnt; /* numbers of frames requested */
  rte_atomic32_t* st30_frames_refcnt;
  size_t st30_frame_size;

  uint32_t pkt_len;       /* data len(byte) for each pkt */
  uint32_t st30_pkt_size; /* size for each pkt which include the header */
  int st30_total_pkts;    /* total pkts in one frame */
  int st30_pkt_idx;       /* pkt index in current frame */
  uint16_t st30_seq_id;   /* seq id for each pkt */

  uint32_t tmstamp;
  size_t frame_recv_size;

  /* st30 rtp info */
  struct rte_ring* st30_rtps_ring;

  /* status */
  int st30_stat_pkts_dropped;
  int st30_stat_pkts_received;
  int st30_stat_frames_dropped;
  int st30_stat_frames_received;
  int st30_stat_pkts_rtp_ring_full;
  uint64_t st30_stat_last_time;
};

struct st_rx_audio_sessions_mgr {
  struct st_main_impl* parnet;
  int idx; /* index for current session mgr */

  struct st_rx_audio_session_impl sessions[ST_MAX_RX_AUDIO_SESSIONS];
  bool active[ST_MAX_RX_AUDIO_SESSIONS]; /* active map for sessions */
};

struct st_tx_ancillary_session_pacing {
  double frame_time;          /* time of the frame in nanoseconds */
  double frame_time_sampling; /* time of the frame in sampling(90k) */
  uint64_t cur_epochs;        /* epoch of current frame */
  uint32_t cur_time_stamp;    /* The Timestamp field shall contain the RTP Timestamp as
                                 specified in SMPTE ST 2110-10, 90K sample */
  double tsc_time_cursor;     /* in ns, tsc time cursor for packet pacing */
};

struct st_tx_ancillary_session_impl {
  int idx; /* index for current session */
  struct st40_tx_ops ops;
  char ops_name[ST_MAX_NAME_LEN];

  enum st_port port_maps[ST_SESSION_PORT_MAX];
  struct rte_mbuf* inflight[ST_SESSION_PORT_MAX];
  bool has_inflight[ST_SESSION_PORT_MAX];
  int inflight_cnt[ST_SESSION_PORT_MAX]; /* for stats */
  struct rte_ring* packet_ring;

  void* st40_frames;
  uint16_t st40_frame_idx; /* current frame index */
  enum st40_tx_frame_status st40_frame_stat;

  uint16_t st40_ipv4_packet_id;
  uint16_t st40_src_port[ST_SESSION_PORT_MAX]; /* udp port */
  uint16_t st40_dst_port[ST_SESSION_PORT_MAX]; /* udp port */
  struct st_rfc8331_anc_hdr hdr[ST_SESSION_PORT_MAX];

  struct st_tx_ancillary_session_pacing pacing;
  struct st_fps_timing fps_tm;

  uint16_t st40_seq_id;     /* seq id for each pkt */
  uint16_t st40_ext_seq_id; /* ext seq id for each pkt */
  int st40_total_pkts;      /* total pkts in one frame */
  int st40_pkt_idx;         /* pkt index in current frame */
  int st40_rtp_time;        /* record rtp time */

  /* stat */
  int st40_stat_frame_cnt;
  int st40_stat_pkt_cnt;
  uint32_t st40_epoch_mismatch; /* count of frame lower than pacing */
};

struct st_tx_ancillary_sessions_mgr {
  struct st_main_impl* parnet;
  int idx; /* index for current sessions mgr */
  /* all anc sessions share same ring/queue */
  struct rte_ring* ring[ST_PORT_MAX];
  uint16_t port_id[ST_PORT_MAX];
  uint16_t queue_id[ST_PORT_MAX];
  uint16_t queue_active[ST_PORT_MAX];

  struct st_tx_ancillary_session_impl sessions[ST_MAX_TX_ANC_SESSIONS];
  bool active[ST_MAX_TX_ANC_SESSIONS]; /* active map for sessions */

  /* status */
  int st40_stat_pkts_burst;
};

struct st_rx_ancillary_session_impl {
  int idx; /* index for current session */
  struct st40_rx_ops ops;
  char ops_name[ST_MAX_NAME_LEN];

  enum st_port port_maps[ST_SESSION_PORT_MAX];
  uint16_t queue_id[ST_SESSION_PORT_MAX]; /* queue id for the session */
  bool queue_active[ST_SESSION_PORT_MAX];
  uint16_t port_id[ST_SESSION_PORT_MAX];
  struct rte_ring* packet_ring;

  uint16_t st40_src_port[ST_SESSION_PORT_MAX]; /* udp port */
  uint16_t st40_dst_port[ST_SESSION_PORT_MAX]; /* udp port */

  int st40_pkt_idx;
  uint16_t st40_seq_id; /* seq id for each pkt */

  uint32_t tmstamp;
  /* status */
  int st40_stat_frames_received;
  int st40_stat_pkts_dropped;
  int st40_stat_pkts_received;
  uint64_t st40_stat_last_time;
};

struct st_rx_ancillary_sessions_mgr {
  struct st_main_impl* parnet;
  int idx; /* index for current session mgr */

  struct st_rx_ancillary_session_impl sessions[ST_MAX_RX_ANC_SESSIONS];
  bool active[ST_MAX_RX_ANC_SESSIONS]; /* active map for sessions */
};

struct st_ancillary_transmitter_impl {
  struct st_main_impl* parnet;
  struct st_tx_ancillary_sessions_mgr* mgr;
  int idx; /* index for current transmitter */

  struct rte_mbuf* inflight[ST_PORT_MAX]; /* inflight mbuf */
  int inflight_cnt[ST_PORT_MAX];          /* for stats */
};

struct st_pacing_train_result {
  uint64_t rl_bps;           /* input, byte per sec */
  float pacing_pad_interval; /* result */
};

struct st_rl_shaper {
  uint64_t rl_bps; /* input, byte per sec */
  uint32_t shaper_profile_id;
  int idx;
};

struct st_interface {
  enum st_port port;
  uint16_t port_id;
  enum st_port_type port_type;
  int socket_id;                          /* socket id for the port */
  uint32_t feature;                       /* ST_IF_FEATURE_* */
  uint32_t link_speed;                    /**< ETH_SPEED_NUM_ */
  struct rte_ether_addr* mcast_mac_lists; /**< pool of multicast mac addrs */
  uint32_t mcast_nb;                      /**< number of address */

  struct rte_mempool* mbuf_pool;
  uint16_t nb_tx_desc;
  uint16_t nb_rx_desc;

  /* tx queue resources */
  int max_tx_queues;
  bool* tx_queues_active;
  /* bytes per sec for rate limit */
  uint64_t* tx_queues_bps;
  /* tx rl info */
  int* tx_rl_shapers_mapping; /* map to tx_rl_shapers */
  struct st_rl_shaper tx_rl_shapers[ST_MAX_RL_ITEMS];
  bool tx_rl_root_active;
  /* video rl pacing train result */
  struct st_pacing_train_result pt_results[ST_MAX_RL_ITEMS];

  /* rx queue resources */
  int max_rx_queues;
  bool* rx_queues_active;
  struct rte_flow** rx_queues_flow;
  struct st_dev_flow* rx_dev_flows;

  /* function ops per interface(pf/vf) */
  uint64_t (*ptp_get_time_fn)(struct st_main_impl* impl, enum st_port port);
};

struct st_lcore_shm {
  int used;                          /* number of used lcores */
  bool lcores_active[RTE_MAX_LCORE]; /* lcores active map */
};

struct st_main_impl {
  struct st_interface inf[ST_PORT_MAX];

  struct st_init_params user_para;
  uint64_t tsc_hz;
  pthread_t tsc_cal_tid;

  /* stat */
  pthread_t stat_tid;
  pthread_cond_t stat_wake_cond;
  pthread_mutex_t stat_wake_mutex;
  rte_atomic32_t stat_stop;

  /* dev context */
  rte_atomic32_t started;       /* if st dev is started */
  rte_atomic32_t dev_in_reset;  /* if dev is in reset */
  rte_atomic32_t request_exit;  /* request exit, in case for ctrl-c from app */
  struct st_sch_impl* main_sch; /* system sch */

  /* cni context */
  struct st_cni_impl cni;

  /* ptp context */
  struct st_ptp_impl ptp[ST_PORT_MAX];

  /* arp context */
  struct st_arp_impl arp;

  /* mcast context */
  struct st_mcast_impl mcast;

  /* sch context */
  struct st_sch_impl sch[ST_MAX_SCH_NUM];

  /* audio(st_30) context */
  struct st_tx_audio_sessions_mgr tx_a_mgr;
  struct st_audio_transmitter_impl a_trs;
  struct st_rx_audio_sessions_mgr rx_a_mgr;
  bool tx_a_init;
  bool rx_a_init;

  /* ancillary(st_40) context */
  struct st_tx_ancillary_sessions_mgr tx_anc_mgr;
  struct st_ancillary_transmitter_impl anc_trs;
  struct st_rx_ancillary_sessions_mgr rx_anc_mgr;
  bool tx_anc_init;
  bool rx_anc_init;

  /* max sessions supported */
  uint16_t tx_sessions_cnt_max;
  uint16_t rx_sessions_cnt_max;
  /* cnt for open sessions */
  rte_atomic32_t st20_tx_sessions_cnt;
  rte_atomic32_t st22_tx_sessions_cnt;
  rte_atomic32_t st30_tx_sessions_cnt;
  rte_atomic32_t st40_tx_sessions_cnt;
  rte_atomic32_t st20_rx_sessions_cnt;
  rte_atomic32_t st22_rx_sessions_cnt;
  rte_atomic32_t st30_rx_sessions_cnt;
  rte_atomic32_t st40_rx_sessions_cnt;
  /* active sch cnt */
  rte_atomic32_t sch_cnt;
  /* active lcore cnt */
  rte_atomic32_t lcore_cnt;

  enum st21_tx_pacing_way tx_pacing_way;

  struct st_lcore_shm* lcore_shm;
  int lcore_shm_id;
  int lcore_lock_fd;

  /* rx timestamp register */
  int dynfield_offset;
};

struct st_tx_video_session_handle_impl {
  struct st_main_impl* parnet;
  enum st_session_type type;
  struct st_sch_impl* sch; /* the sch this session attached */
  int quota_mbs;           /* data quota for this session */
  struct st_tx_video_session_impl* impl;
};

struct st22_tx_video_session_handle_impl {
  struct st_main_impl* parnet;
  enum st_session_type type;
  struct st_sch_impl* sch; /* the sch this session attached */
  int quota_mbs;           /* data quota for this session */
  struct st_tx_video_session_impl* impl;
};

struct st_tx_audio_session_handle_impl {
  struct st_main_impl* parnet;
  enum st_session_type type;
  struct st_tx_audio_session_impl* impl;
};

struct st_tx_ancillary_session_handle_impl {
  struct st_main_impl* parnet;
  enum st_session_type type;
  struct st_tx_ancillary_session_impl* impl;
};

struct st_rx_video_session_handle_impl {
  struct st_main_impl* parnet;
  enum st_session_type type;
  struct st_sch_impl* sch; /* the sch this session attached */
  int quota_mbs;           /* data quota for this session */
  struct st_rx_video_session_impl* impl;
};

struct st22_rx_video_session_handle_impl {
  struct st_main_impl* parnet;
  enum st_session_type type;
  struct st_sch_impl* sch; /* the sch this session attached */
  int quota_mbs;           /* data quota for this session */
  struct st_rx_video_session_impl* impl;
};

struct st_rx_audio_session_handle_impl {
  struct st_main_impl* parnet;
  enum st_session_type type;
  struct st_rx_audio_session_impl* impl;
};

struct st_rx_ancillary_session_handle_impl {
  struct st_main_impl* parnet;
  enum st_session_type type;
  struct st_rx_ancillary_session_impl* impl;
};

static inline struct st_init_params* st_get_user_params(struct st_main_impl* impl) {
  return &impl->user_para;
}

static inline struct st_interface* st_if(struct st_main_impl* impl, enum st_port port) {
  return &impl->inf[port];
}

static inline uint16_t st_port_id(struct st_main_impl* impl, enum st_port port) {
  return st_if(impl, port)->port_id;
}

static inline enum st_port_type st_port_type(struct st_main_impl* impl,
                                             enum st_port port) {
  return st_if(impl, port)->port_type;
}

enum st_port st_port_by_id(struct st_main_impl* impl, uint16_t port_id);

static inline uint8_t* st_sip_addr(struct st_main_impl* impl, enum st_port port) {
  return st_get_user_params(impl)->sip_addr[port];
}

static inline int st_num_ports(struct st_main_impl* impl) {
  return st_get_user_params(impl)->num_ports;
}

static inline int st_has_ptp_service(struct st_main_impl* impl) {
  if (st_get_user_params(impl)->flags & ST_FLAG_PTP_ENABLE)
    return true;
  else
    return false;
}

static inline int st_has_user_ptp(struct st_main_impl* impl) {
  if (st_get_user_params(impl)->ptp_get_time_fn)
    return true;
  else
    return false;
}

static inline int st_if_has_timesync(struct st_main_impl* impl, enum st_port port) {
  if (st_if(impl, port)->feature & ST_IF_FEATURE_TIMESYNC)
    return true;
  else
    return false;
}

static inline int st_socket_id(struct st_main_impl* impl, enum st_port port) {
  return st_if(impl, port)->socket_id;
}

static inline struct rte_mempool* st_get_mempool(struct st_main_impl* impl,
                                                 enum st_port port) {
  return st_if(impl, port)->mbuf_pool;
}

static inline void st_sleep_ms(unsigned int ms) { return rte_delay_us_sleep(ms * 1000); }

static inline void st_delay_us(unsigned int us) { return rte_delay_us_block(us); }

static inline void st_tx_burst_busy(uint16_t port_id, uint16_t queue_id,
                                    struct rte_mbuf** tx_pkts, uint16_t nb_pkts) {
  uint32_t sent = 0;

  /* Send this vector with busy looping */
  while (sent < nb_pkts) {
    sent += rte_eth_tx_burst(port_id, queue_id, &tx_pkts[sent], nb_pkts - sent);
  }
}

static inline void st_free_mbufs(struct rte_mbuf** pkts, int num) {
  for (int i = 0; i < num; i++) {
    rte_pktmbuf_free(pkts[i]);
    pkts[i] = NULL;
  }
}

static inline void st_mbuf_init_ipv4(struct rte_mbuf* pkt) {
  pkt->l2_len = sizeof(struct rte_ether_hdr); /* 14 */
  pkt->l3_len = sizeof(struct rte_ipv4_hdr);  /* 20 */
  pkt->ol_flags |= PKT_TX_IPV4 | PKT_TX_IP_CKSUM;
}

static inline uint64_t st_timespec_to_ns(const struct timespec* ts) {
  return ((uint64_t)ts->tv_sec * NS_PER_S) + ts->tv_nsec;
}

static inline void st_ns_to_timespec(uint64_t ns, struct timespec* ts) {
  ts->tv_sec = ns / NS_PER_S;
  ts->tv_nsec = ns % NS_PER_S;
}

/* Return relative TSC time in nanoseconds */
static inline uint64_t st_get_tsc(struct st_main_impl* impl) {
  double tsc = rte_get_tsc_cycles();
  double tsc_hz = impl->tsc_hz;
  double time_nano = tsc / (tsc_hz / ((double)NS_PER_S));
  return time_nano;
}

/* busy loop until target time reach */
static inline void st_tsc_delay_to(struct st_main_impl* impl, uint64_t target) {
  while (st_get_tsc(impl) < target) {
  }
}

/* Monotonic time (in nanoseconds) since some unspecified starting point. */
static inline uint64_t st_get_monotonic_time() {
  struct timespec ts;

  clock_gettime(ST_CLOCK_MONOTONIC_ID, &ts);
  return ((uint64_t)ts.tv_sec * NS_PER_S) + ts.tv_nsec;
}

static inline void st_mbuf_set_time_stamp(struct rte_mbuf* mbuf, uint64_t time_stamp) {
  struct st_muf_priv_data* priv = rte_mbuf_to_priv(mbuf);
  priv->time_stamp = time_stamp;
}

static inline uint64_t st_mbuf_get_time_stamp(struct rte_mbuf* mbuf) {
  struct st_muf_priv_data* priv = rte_mbuf_to_priv(mbuf);
  return priv->time_stamp;
}

static inline uint64_t st_mbuf_get_hw_time_stamp(struct st_main_impl* impl,
                                                 struct rte_mbuf* mbuf) {
  struct st_ptp_impl* ptp = impl->ptp;
  struct timespec spec;
  uint64_t time_stamp =
      *RTE_MBUF_DYNFIELD(mbuf, impl->dynfield_offset, rte_mbuf_timestamp_t*);
  st_ns_to_timespec(time_stamp, &spec);
  spec.tv_sec -= ptp->master_utc_offset;
  time_stamp = st_timespec_to_ns(&spec) + ptp->ptp_delta;
  return time_stamp;
}

static inline void st_mbuf_set_idx(struct rte_mbuf* mbuf, uint64_t idx) {
  struct st_muf_priv_data* priv = rte_mbuf_to_priv(mbuf);
  priv->idx = idx;
}

static inline uint64_t st_mbuf_get_idx(struct rte_mbuf* mbuf) {
  struct st_muf_priv_data* priv = rte_mbuf_to_priv(mbuf);
  return priv->idx;
}

static inline uint64_t st_get_ptp_time(struct st_main_impl* impl, enum st_port port) {
  return st_if(impl, port)->ptp_get_time_fn(impl, port);
}

#endif
