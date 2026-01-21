/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef _MT_LIB_ST_HEAD_H_
#define _MT_LIB_ST_HEAD_H_

#include "../mt_header.h"
#include "st20_api.h"
#include "st30_api.h"
#include "st40_api.h"
#include "st41_api.h"
#include "st_convert.h"
#include "st_fmt.h"
#include "st_pipeline_api.h"
#include "st_pkt.h"

#define ST_MAX_NAME_LEN (32)

#define ST_PLUGIN_MAX_PATH_LEN (128)

/* max 12 1080p tx session per sch lcore */
#define ST_QUOTA_TX1080P_PER_SCH (12)
/* max 8 1080p rtp tx session per sch lcore */
#define ST_QUOTA_TX1080P_RTP_PER_SCH (8)
/* max 12 1080p rx session per sch lcore */
#define ST_QUOTA_RX1080P_PER_SCH (12)
/* max 12 1080p rtp rx session per sch lcore, rtp unpack run on other threads */
#define ST_QUOTA_RX1080P_RTP_PER_SCH (12)
/* max 8 1080p rx(without DMA) session per sch lcore */
#define ST_QUOTA_RX1080P_NO_DMA_PER_SCH (8)

#define ST_SCH_MAX_TX_VIDEO_SESSIONS (60) /* max video tx sessions per sch lcore */
#define ST_SCH_MAX_RX_VIDEO_SESSIONS (60) /* max video rx sessions per sch lcore */
#define ST_SESSION_MAX_BULK (4)
#define ST_TX_VIDEO_SESSIONS_RING_SIZE (2048)

/* number of tmstamp it will tracked for out of order pkts */
#define ST_VIDEO_RX_REC_NUM_OFO (2)
/* number of slices it will tracked as out of order pkts */
#define ST_VIDEO_RX_SLICE_NUM (32)
/* sync to atomic if reach this threshold */
#define ST_VIDEO_STAT_UPDATE_INTERVAL (1000)
/* data size for each pkt in block packing mode */
#define ST_VIDEO_BPM_SIZE (1260)

/* max tx/rx audio(st30) sessions */
#define ST_SCH_MAX_TX_AUDIO_SESSIONS (512) /* max audio tx sessions per sch lcore */
#define ST_TX_AUDIO_SESSIONS_RING_SIZE (ST_SCH_MAX_TX_AUDIO_SESSIONS * 2)
#define ST_SCH_MAX_RX_AUDIO_SESSIONS (512 * 2) /* max audio rx sessions per sch lcore */

/* max tx/rx anc(st40) sessions */
#define ST_MAX_TX_ANC_SESSIONS (180)
#define ST_TX_ANC_SESSIONS_RING_SIZE (512)
#define ST_MAX_RX_ANC_SESSIONS (180)

/* max tx/rx fmd(st41) sessions */
#define ST_MAX_TX_FMD_SESSIONS (180)
#define ST_TX_FMD_SESSIONS_RING_SIZE (512)
#define ST_MAX_RX_FMD_SESSIONS (180)

/* max dl plugin lib number */
#define ST_MAX_DL_PLUGINS (8)
/* max encoder devices number */
#define ST_MAX_ENCODER_DEV (8)
/* max decoder devices number */
#define ST_MAX_DECODER_DEV (8)
/* max converter devices number */
#define ST_MAX_CONVERTER_DEV (8)
/* max sessions number per encoder */
#define ST_MAX_SESSIONS_PER_ENCODER (16)
/* max sessions number per decoder */
#define ST_MAX_SESSIONS_PER_DECODER (16)
/* max sessions number per converter */
#define ST_MAX_SESSIONS_PER_CONVERTER (16)

#define ST_TX_DUMMY_PKT_IDX (0xFFFFFFFF)

#define ST_SESSION_STAT_TIMEOUT_US (10)

#define ST_SESSION_REDUNDANT_ERROR_THRESHOLD (20)

#define ST_SESSION_STAT_INC(s, struct, stat) \
  do {                                       \
    (s)->stat++;                             \
    (s)->struct.stat++;                      \
  } while (0)

#define ST_SESSION_STAT_ADD(s, struct, stat, val) \
  do {                                            \
    (s)->stat += (val);                           \
    (s)->struct.stat += (val);                    \
  } while (0)

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

enum st41_tx_frame_status {
  ST41_TX_STAT_UNKNOWN = 0,
  ST41_TX_STAT_WAIT_FRAME,
  ST41_TX_STAT_SENDING_PKTS,
};

struct st_tx_muf_priv_data {
  uint64_t tsc_time_stamp; /* tsc time stamp of current mbuf */
  uint64_t ptp_time_stamp; /* ptp time stamp of current mbuf */
  void* priv;              /* private data to current frame */
  uint32_t idx;            /* index of packet in current frame */
};

struct st_rx_muf_priv_data {
  uint32_t offset;
  uint32_t len;
  uint32_t lender;
  uint32_t padding;
};

/* the frame is malloc by rte malloc, not ext or head split */
#define ST_FT_FLAG_RTE_MALLOC (MTL_BIT32(0))
/* ext frame by application */
#define ST_FT_FLAG_EXT (MTL_BIT32(1))
/* the frame is malloc by gpu zero-level api */
#define ST_FT_FLAG_GPU_MALLOC (MTL_BIT32(2))

/* IOVA mapping info of each page in frame, used for IOVA:PA mode */
struct st_page_info {
  rte_iova_t iova; /* page begin iova */
  void* addr;      /* page begin va */
  size_t len;      /* page length */
};

/* describe the frame used in transport(both tx and rx) */
struct st_frame_trans {
  int idx;
  void* addr;                      /* virtual address */
  rte_iova_t iova;                 /* iova for hw */
  struct st_page_info* page_table; /* page table for hw, used for IOVA:PA mode */
  uint16_t page_table_len;         /* page table len for hw, used for IOVA:PA mode */
  rte_atomic32_t refcnt;           /* 0 means it's free */
  void* priv;                      /* private data for lib */

  uint32_t flags;                          /* ST_FT_FLAG_* */
  struct rte_mbuf_ext_shared_info sh_info; /* for st20 tx ext shared */

  void* user_meta; /* the meta data from user */
  size_t user_meta_buffer_size;
  size_t user_meta_data_size;

  /* metadata */
  union {
    struct st20_tx_frame_meta tv_meta;
    struct st22_tx_frame_meta tx_st22_meta;
    struct st20_rx_frame_meta rv_meta; /* not use now */
    struct st30_tx_frame_meta ta_meta;
    struct st30_rx_frame_meta ra_meta; /* not use now */
    struct st40_tx_frame_meta tc_meta;
    struct st41_tx_frame_meta tf_meta;
  };
};

/* timing for pacing */
struct st_tx_video_pacing {
  double trs;         /* in ns for of 2 consecutive packets, T-Frame / N-Packets */
  double tr_offset;   /* in ns, tr offset time of each frame */
  uint32_t vrx;       /* packets unit, VRX start value of each frame */
  uint32_t warm_pkts; /* packets unit, pkts for RL pacing warm boot */
  double frame_time;  /* time of the frame in nanoseconds */
  double frame_time_sampling; /* time of the frame in sampling(90k) */
  /* in ns, idle time at the end of frame, frame_time - tr_offset - (trs * pkts) */
  double frame_idle_time;
  double reactive;
  float pad_interval; /* padding pkt interval(pkts level) for RL pacing */

  uint64_t cur_epochs; /* epoch of current frame */
  /* timestamp for rtp header */
  uint32_t rtp_time_stamp;
  uint64_t tsc_time_cursor; /* in ns, tsc time cursor for packet pacing */
  uint64_t ptp_time_cursor; /* in ns, ptp time cursor for packet pacing */
  /* ptp time may onward */
  uint32_t max_onward_epochs;
  uint64_t tsc_time_frame_start; /* start tsc time for frame start */
};

enum st20_packet_type {
  ST20_PKT_TYPE_NORMAL = 0,
  ST20_PKT_TYPE_EXTRA,
  ST20_PKT_TYPE_FRAME_TAIL,
  ST20_PKT_TYPE_LINE_TAIL,
  ST20_PKT_TYPE_MAX,
};

/* info of each type of packets */
struct st20_packet_group_info {
  uint32_t size;   /* size of packet including header for this type */
  uint32_t number; /* number of packets in frame for this type */
};

/* len: 22(0x16) */
struct st22_jpvi {
  uint32_t lbox; /* box length */
  char tbox[4];  /* box type */
  uint32_t brat;
  uint32_t frat;
  uint16_t schar;
  uint32_t tcod;
} __attribute__((__packed__));

/* len: 12(0x0C) */
struct st22_jxpl {
  uint32_t lbox; /* box lenght */
  char tbox[4];  /* box type */
  uint16_t ppih;
  uint16_t plev;
} __attribute__((__packed__));

/* len: 42(0x2A) */
struct st22_jpvs {
  uint32_t lbox; /* box lenght */
  char tbox[4];  /* box type */
  struct st22_jpvi jpvi;
  struct st22_jxpl jxpl;
} __attribute__((__packed__));

/* len: 18(0x12) */
struct st22_colr {
  uint32_t lbox; /* box lenght */
  char tbox[4];  /* box type */
  uint8_t meth;
  uint8_t prec;
  uint8_t approx;
  uint8_t methdat[7];
} __attribute__((__packed__));

/* len: 60(0x3C) */
struct st22_boxes {
  struct st22_jpvs jpvs;
  struct st22_colr colr;
} __attribute__((__packed__));

struct st22_tx_video_info {
  /* app callback */
  int (*get_next_frame)(void* priv, uint16_t* next_frame_idx,
                        struct st22_tx_frame_meta* meta);
  int (*notify_frame_done)(void* priv, uint16_t frame_idx,
                           struct st22_tx_frame_meta* meta);

  struct st22_rfc9134_rtp_hdr rtp_hdr[MTL_SESSION_PORT_MAX];
  int pkt_idx;           /* for P&F counter*/
  size_t cur_frame_size; /* size per frame */
  int frame_idx;         /* The Frame (F) counter */

  struct st22_boxes st22_boxes;
  int st22_total_pkts;
};

struct st_vsync_info {
  struct st10_vsync_meta meta;
  uint64_t next_epoch_tsc;
  bool init;
};

struct st_tx_video_session_impl {
  struct mtl_main_impl* impl;
  struct st_tx_video_sessions_mgr* mgr;
  int socket_id;
  bool active;
  enum mtl_port port_maps[MTL_SESSION_PORT_MAX];
  struct rte_mempool* mbuf_mempool_hdr[MTL_SESSION_PORT_MAX];
  bool mbuf_mempool_reuse_rx[MTL_SESSION_PORT_MAX]; /* af_xdp zero copy */
  struct rte_mempool* mbuf_mempool_chain;
  struct rte_mempool* mbuf_mempool_copy_chain;
  bool tx_mono_pool;   /* if reuse tx mono pool */
  bool tx_no_chain;    /* if tx not use chain mbuf */
  bool multi_src_port; /* if tx use multiple src port */
  /* if the eth dev support chain buff */
  bool eth_has_chain[MTL_SESSION_PORT_MAX];
  /* if the eth dev support ipv4 checksum offload */
  bool eth_ipv4_cksum_offload[MTL_SESSION_PORT_MAX];
  unsigned int ring_count;
  struct rte_ring* ring[MTL_SESSION_PORT_MAX];
  struct rte_ring* packet_ring; /* rtp ring */
  struct mt_txq_entry* queue[MTL_SESSION_PORT_MAX];
  int idx; /* index for current tx_session */
  uint64_t advice_sleep_us;
  int recovery_idx;

  struct st_tx_video_session_handle_impl* st20_handle;
  struct st22_tx_video_session_handle_impl* st22_handle;

  uint16_t st20_src_port[MTL_SESSION_PORT_MAX]; /* udp port */
  uint16_t st20_dst_port[MTL_SESSION_PORT_MAX]; /* udp port */
  struct st_rfc4175_video_hdr s_hdr[MTL_SESSION_PORT_MAX];

  struct st_tx_video_pacing pacing;
  enum st21_tx_pacing_way pacing_way[MTL_SESSION_PORT_MAX];
  int (*pacing_tasklet_func[MTL_SESSION_PORT_MAX])(struct mtl_main_impl* impl,
                                                   struct st_tx_video_session_impl* s,
                                                   enum mtl_session_port s_port);

  struct st_vsync_info vsync;
  bool second_field;
  int usdt_frame_cnt;

  struct st20_tx_ops ops;
  char ops_name[ST_MAX_NAME_LEN];
  enum mt_handle_type s_type; /* st22 or st20 */

  unsigned int bulk; /* Enqueue bulk objects on the ring */
  struct rte_mbuf* inflight[MTL_SESSION_PORT_MAX][ST_SESSION_MAX_BULK];
  int inflight_cnt[MTL_SESSION_PORT_MAX]; /* for stats */

  /* info for transmitter */
  uint64_t trs_target_tsc[MTL_SESSION_PORT_MAX];
  uint64_t trs_target_ptp[MTL_SESSION_PORT_MAX];
  struct rte_mbuf* trs_inflight[MTL_SESSION_PORT_MAX][ST_SESSION_MAX_BULK];
  unsigned int trs_inflight_num[MTL_SESSION_PORT_MAX];
  unsigned int trs_inflight_idx[MTL_SESSION_PORT_MAX];
  unsigned int trs_pad_inflight_num[MTL_SESSION_PORT_MAX]; /* inflight padding */
  int trs_inflight_cnt[MTL_SESSION_PORT_MAX];              /* for stats */
  struct rte_mbuf* trs_inflight2[MTL_SESSION_PORT_MAX][ST_SESSION_MAX_BULK];
  unsigned int trs_inflight_num2[MTL_SESSION_PORT_MAX];
  unsigned int trs_inflight_idx2[MTL_SESSION_PORT_MAX];
  int trs_inflight_cnt2[MTL_SESSION_PORT_MAX]; /* for stats */

  /* the last burst succ time(tsc) */
  uint64_t last_burst_succ_time_tsc[MTL_SESSION_PORT_MAX];
  uint64_t tx_hang_detect_time_thresh;

  /* frame info */
  size_t st20_frame_size;   /* size per frame */
  size_t st20_fb_size;      /* frame buffer size, with lines' padding */
  size_t st20_linesize;     /* line size including padding bytes */
  uint16_t st20_frames_cnt; /* numbers of frames requested */
  struct st_frame_trans* st20_frames;

  uint16_t st20_frame_idx; /* current frame index */
  enum st21_tx_frame_status st20_frame_stat;
  uint16_t st20_frame_lines_ready;

  struct st20_pgroup st20_pg;
  struct st_fps_timing fps_tm;
  int st20_bytes_in_line;    /* number of bytes per each line, 4800 for 1080p */
  int st20_pkts_in_line;     /* number of packets per each line, 4 for 1080p */
  uint16_t st20_pkt_len;     /* data len(byte) for each pkt, 1200 */
  uint16_t st20_pkt_size;    /* size for each rtp which include all hdr */
  uint16_t rtp_pkt_max_size; /* max size for user rtp pkt */
  int st20_total_pkts;       /* total pkts in one frame, ex: 4320 for 1080p */
  int st20_pkt_idx;          /* pkt index in current frame, start from zero */
  uint32_t st20_seq_id;      /* seq id for each pkt */
  uint32_t st20_rtp_time;    /* keep track of rtp time */
  int st21_vrx_narrow;       /* pass criteria for narrow */
  int st21_vrx_wide;         /* pass criteria for wide */

  struct st20_packet_group_info st20_pkt_info[ST20_PKT_TYPE_MAX];
  struct rte_mbuf* pad[MTL_SESSION_PORT_MAX][ST20_PKT_TYPE_MAX];

  /* the cpu resource to handle tx, 0: full, 100: cpu is very busy */
  double cpu_busy_score;
  rte_atomic32_t cbs_build_timeout;

  /* info for st22 */
  struct st22_tx_video_info* st22_info;
  uint16_t st22_box_hdr_length;
  size_t st22_codestream_size;

  struct mt_rtcp_tx* rtcp_tx[MTL_SESSION_PORT_MAX];
  struct mt_rxq_entry* rtcp_q[MTL_SESSION_PORT_MAX];

  /* stat */
  rte_atomic32_t stat_frame_cnt;
  int stat_pkts_build[MTL_SESSION_PORT_MAX];
  int stat_pkts_dummy;
  int stat_pkts_burst;
  int stat_pkts_burst_dummy;
  int stat_pkts_chain_realloc_fail;
  int stat_trs_ret_code[MTL_SESSION_PORT_MAX];
  int stat_build_ret_code;
  uint64_t stat_last_time;
  uint32_t stat_epoch_drop;
  uint32_t stat_epoch_onward;
  uint32_t stat_error_user_timestamp;
  uint32_t stat_epoch_troffset_mismatch; /* pacing mismatch the epoch troffset */
  uint32_t stat_trans_troffset_mismatch; /* transmitter mismatch the epoch troffset */
  uint32_t stat_trans_recalculate_warmup;
  uint32_t stat_exceed_frame_time;
  bool stat_user_busy_first;
  uint32_t stat_user_busy;       /* get_next_frame or dequeue_bulk from rtp ring fail */
  uint32_t stat_lines_not_ready; /* query app lines not ready */
  uint32_t stat_vsync_mismatch;
  uint64_t stat_bytes_tx[MTL_SESSION_PORT_MAX];
  uint32_t stat_user_meta_cnt;
  uint32_t stat_user_meta_pkt_cnt;
  uint32_t stat_max_next_frame_us;
  uint32_t stat_max_notify_frame_us;
  uint32_t stat_unrecoverable_error;
  uint32_t stat_recoverable_error;
  /* interlace */
  uint32_t stat_interlace_first_field;
  uint32_t stat_interlace_second_field;
  /* for display */
  double stat_cpu_busy_score;
  /* for tasklet session time measure */
  struct mt_stat_u64 stat_time;
  struct st20_tx_user_stats port_user_stats;
};

struct st_tx_video_sessions_mgr {
  struct mtl_main_impl* parent;
  int idx;     /* index for current session mgr */
  int max_idx; /* max session index */
  struct mt_sch_tasklet_impl* tasklet;

  struct st_tx_video_session_impl* sessions[ST_SCH_MAX_TX_VIDEO_SESSIONS];
  /* protect session, spin(fast) lock as it call from tasklet aslo */
  rte_spinlock_t mutex[ST_SCH_MAX_TX_VIDEO_SESSIONS];
};

struct st_video_transmitter_impl {
  struct mtl_main_impl* parent;
  struct st_tx_video_sessions_mgr* mgr;
  struct mt_sch_tasklet_impl* tasklet;
  int idx; /* index for current transmitter */
};

struct st_rx_video_slot_slice {
  uint32_t offset;
  uint32_t size;
};

struct st_rx_video_slot_slice_info {
  struct st_rx_video_slot_slice slices[ST_VIDEO_RX_SLICE_NUM];
  uint32_t ready_slices;
  uint32_t extra_slices;
};

struct st_rx_video_slot_impl {
  int idx;
  int64_t tmstamp;
  uint16_t seq_id_base;     /* seq id for the first packet */
  uint32_t seq_id_base_u32; /* seq id for the first packet with u32 */
  bool seq_id_got;
  struct st_frame_trans* frame; /* only for frame type */
  uint8_t* frame_bitmap;
  size_t frame_recv_size;           /* for frame type */
  size_t pkt_lcore_frame_recv_size; /* frame_recv_size for pkt lcore */
  /* the total packets received, not include the redundant packets */
  uint32_t pkts_received;
  uint32_t pkts_recv_per_port[MTL_SESSION_PORT_MAX];
  struct st20_rx_frame_meta meta;      /* only for frame type */
  struct st22_rx_frame_meta st22_meta; /* only for st22 frame type */
  /* Second field type indicate */
  bool second_field;
  struct st_rx_video_slot_slice_info* slice_info; /* for ST20_TYPE_SLICE_LEVEL */
  /* payload len for codestream packetization mode */
  uint16_t st22_payload_length;
  uint16_t st22_box_hdr_length;
  /* timestamp(ST10_TIMESTAMP_FMT_TAI, PTP) value for the first pkt */
  uint64_t timestamp_first_pkt;
  int last_pkt_idx;
};

enum st20_detect_status {
  ST20_DETECT_STAT_DISABLED = 0,
  ST20_DETECT_STAT_DETECTING,
  ST20_DETECT_STAT_SUCCESS,
  ST20_DETECT_STAT_FAIL,
};

struct st_rx_video_detector {
  enum st20_detect_status status;
  bool bpm;
  uint32_t rtp_tm[3];
  int pkt_num[3];
  int frame_num;
  bool single_line;
  int pkt_per_frame;

  /* detect result */
  struct st20_detect_meta meta;
};

struct st22_rx_video_info {
  /* app callback */
  int (*notify_frame_ready)(void* priv, void* frame, struct st22_rx_frame_meta* meta);

  struct st22_rx_frame_meta meta;
  size_t cur_frame_size; /* size per frame */
};

struct st_rx_video_hdr_split_info {
  void* frames;
  size_t frames_size;
  rte_iova_t frames_iova;
  uint32_t mbuf_alloc_idx;
  uint32_t mbufs_per_frame;
  uint32_t mbufs_total;
  bool mbuf_pool_ready;

  /* base frame add for current frame */
  void* cur_frame_addr;
  /* mbuf idx for current frame */
  uint32_t cur_frame_mbuf_idx;
};

struct st_rx_video_sessions_mgr; /* forward declare */

struct st_rx_session_priv {
  void* session;
  struct mtl_main_impl* impl;
  enum mtl_session_port s_port;
};

struct st_rv_tp_slot {
  /* epoch of current slot */
  uint64_t cur_epochs;

  struct st20_rx_tp_meta meta;

  uint32_t rtp_tmstamp;
  uint64_t first_pkt_time; /* ns */
  uint64_t prev_pkt_time;  /* ns */
  /* Cinst, packet level check */
  int64_t cinst_sum;
  /* vrx, packet level check */
  int64_t vrx_sum;
  /* Inter-packet time(ns), packet level check */
  int64_t ipt_sum;
};

struct st_rv_tp_stat {
  /* for the status */
  struct st_rv_tp_slot slot;
  uint32_t stat_frame_cnt;

  int32_t stat_fpt_min;
  int32_t stat_fpt_max;
  float stat_fpt_sum;
  int32_t stat_latency_min;
  int32_t stat_latency_max;
  float stat_latency_sum;
  int32_t stat_rtp_offset_min;
  int32_t stat_rtp_offset_max;
  float stat_rtp_offset_sum;
  int32_t stat_rtp_ts_delta_min;
  int32_t stat_rtp_ts_delta_max;
  float stat_rtp_ts_delta_sum;
  uint32_t stat_compliant_result[ST_RX_TP_COMPLIANT_MAX];
};

struct st_rx_video_tp {
  /* in ns for of 2 consecutive packets, T-Frame / N-Packets */
  double trs;
  /* pass criteria */
  struct st20_rx_tp_pass pass;

  /* timing info for each slot */
  struct st_rv_tp_slot slots[ST_VIDEO_RX_REC_NUM_OFO][MTL_SESSION_PORT_MAX];
  uint32_t pre_rtp_tmstamp[MTL_SESSION_PORT_MAX];

  /* for the status */
  struct st_rv_tp_stat stat[MTL_SESSION_PORT_MAX];
  uint32_t stat_untrusted_pkts;
};

struct st_rx_video_session_impl {
  struct mtl_main_impl* impl;
  int idx; /* index for current session */
  int socket_id;
  bool attached;
  struct st_rx_video_sessions_mgr* parent;
  struct st_rx_session_priv priv[MTL_SESSION_PORT_MAX];
  uint16_t rx_burst_size;
  uint16_t cur_succ_burst_cnt;
  bool in_continuous_burst[MTL_SESSION_PORT_MAX];

  struct st20_rx_ops ops;
  char ops_name[ST_MAX_NAME_LEN];
  uint64_t advice_sleep_us;

  enum mtl_port port_maps[MTL_SESSION_PORT_MAX];
  struct mt_rxq_entry* rxq[MTL_SESSION_PORT_MAX];

  uint16_t st20_dst_port[MTL_SESSION_PORT_MAX]; /* udp port */
  bool mcast_joined[MTL_SESSION_PORT_MAX];

  struct st_rx_video_session_handle_impl* st20_handle;
  struct st22_rx_video_session_handle_impl* st22_handle;

  struct st22_rx_video_info* st22_info;

  bool is_hdr_split;
  struct st_rx_video_hdr_split_info hdr_split_info[MTL_SESSION_PORT_MAX];

  /* st20 detector info */
  struct st_rx_video_detector detector;

  struct st_vsync_info vsync;
  int usdt_frame_cnt;

  /* frames info */
  size_t st20_frame_size;        /* size per frame, without padding */
  size_t st20_fb_size;           /* frame buffer size, with lines' padding */
  size_t st20_linesize;          /* line size including padding bytes */
  size_t st20_bytes_in_line;     /* bytes per line not including padding */
  size_t st20_frame_bitmap_size; /* bitmap size per frame */
  int st20_frames_cnt;           /* numbers of frames requested */
  struct st_frame_trans* st20_frames;
  struct st20_pgroup st20_pg;
  double frame_time;          /* time of the frame in nanoseconds */
  double frame_time_sampling; /* time of the frame in sampling(90k) */
  /* in ns for of 2 consecutive packets, T-Frame / N-Packets */
  double trs;

  size_t st20_uframe_size; /* size per user frame */
  struct st20_rx_uframe_pg_meta pg_meta;

  uint32_t st22_ops_flags;       /* copy of st22_rx_ops->flags */
  size_t st22_expect_frame_size; /* total frame size calculated from marker */
  /* expect for each frame, st22_expect_frame_size is clear to zero in the init slot of
   * each frame, in case we don't get maker, it can use previous frame size */
  size_t st22_expect_size_per_frame;

  /* rtp info */
  struct rte_ring* rtps_ring;

  /* Redundant packet threshold guard: Accept packets after error threshold
   * to prevent deadlock when streams reset or have large timestamp jumps.
   * Handles edge case of 2^31 timestamp wraparound (highly unlikely). */
  int redundant_error_cnt[MTL_SESSION_PORT_MAX];

  /* record two frames in case pkts out of order within marker */
  struct st_rx_video_slot_impl slots[ST_VIDEO_RX_REC_NUM_OFO];
  int slot_idx;
  int slot_max;

  /* slice info */
  uint32_t slice_lines;
  size_t slice_size;
  struct st20_rx_slice_meta slice_meta;

  /* dma dev */
  struct mtl_dma_lender_dev* dma_dev;
  uint16_t dma_nb_desc;
  struct st_rx_video_slot_impl* dma_slot;
  bool dma_copy;

  /* pcap dumper */
  struct mt_rx_pcap pcap[MTL_SESSION_PORT_MAX];

  /* additional lcore for pkt handling */
  unsigned int pkt_lcore;
  bool has_pkt_lcore;
  struct rte_ring* pkt_lcore_ring;
  rte_atomic32_t pkt_lcore_active;
  rte_atomic32_t pkt_lcore_stopped;

  /* the cpu resource to handle rx, 0: full, 100: cpu is very busy */
  double cpu_busy_score;
  double dma_busy_score;
  double imiss_busy_score;
  rte_atomic32_t dma_previous_busy_cnt;
  rte_atomic32_t cbs_incomplete_frame_cnt;

  struct mt_rtcp_rx* rtcp_rx[MTL_SESSION_PORT_MAX];
  uint16_t burst_loss_max;
  float sim_loss_rate;
  uint16_t burst_loss_cnt;

  int (*pkt_handler)(struct st_rx_video_session_impl* s, struct rte_mbuf* mbuf,
                     enum mtl_session_port s_port, bool ctrl_thread);

  /* if enable the parser for the st2110-21 timing */
  bool enable_timing_parser;
  bool enable_timing_parser_stat;
  bool enable_timing_parser_meta;
  struct st_rx_video_tp* tp;

  /* status */
  int stat_pkts_idx_dropped;
  int stat_pkts_idx_oo_bitmap;
  int stat_pkts_enqueue_fallback; /* for pkt lcore */
  int stat_pkts_offset_dropped;
  int stat_pkts_out_of_order;
  int stat_pkts_out_of_order_per_port[MTL_SESSION_PORT_MAX];
  int stat_pkts_redundant_dropped;
  int stat_pkts_wrong_pt_dropped;
  int stat_pkts_wrong_ssrc_dropped;
  int stat_pkts_wrong_kmod_dropped; /* for st22 */
  int stat_pkts_wrong_interlace_dropped;
  int stat_pkts_wrong_len_dropped;
  int stat_pkts_received;
  int stat_pkts_retransmit;
  int stat_pkts_multi_segments_received;
  int stat_pkts_dma;
  int stat_pkts_rtp_ring_full;
  int stat_pkts_no_slot;
  int stat_pkts_not_bpm;
  int stat_pkts_copy_hdr_split;
  int stat_pkts_wrong_payload_hdr_split;
  int stat_pkts_simulate_loss;
  int stat_mismatch_hdr_split_frame;
  int stat_frames_dropped;
  int stat_frames_pks_missed;
  rte_atomic32_t stat_frames_received;
  int stat_slices_received;
  int stat_pkts_slice_fail;
  int stat_pkts_slice_merged;
  int stat_pkts_user_meta;
  int stat_pkts_user_meta_err;
  uint64_t stat_last_time;
  uint32_t stat_vsync_mismatch;
  uint32_t stat_slot_get_frame_fail;
  uint32_t stat_slot_query_ext_fail;
  uint64_t stat_bytes_received;
  uint32_t stat_max_notify_frame_us;
  /* for interlace */
  uint32_t stat_interlace_first_field;
  uint32_t stat_interlace_second_field;
  /* for st22 */
  uint32_t stat_st22_boxes;
  /* for stat display */
  double stat_cpu_busy_score;
  /* for tasklet session time measure */
  struct mt_stat_u64 stat_time;
  /* for rx burst */
  int stat_burst_succ_cnt;
  uint16_t stat_burst_pkts_max;
  uint64_t stat_burst_pkts_sum;
  struct st20_rx_user_stats port_user_stats;
};

struct st_rx_video_sessions_mgr {
  struct mtl_main_impl* parent;
  int idx;     /* index for current session mgr */
  int max_idx; /* max session index */
  /* pkt rx task */
  struct mt_sch_tasklet_impl* pkt_rx_tasklet;

  struct st_rx_video_session_impl* sessions[ST_SCH_MAX_RX_VIDEO_SESSIONS];
  /* protect session, spin(fast) lock as it call from tasklet aslo */
  rte_spinlock_t mutex[ST_SCH_MAX_RX_VIDEO_SESSIONS];
};

struct st_tx_audio_session_pacing {
  double trs;               /* in ns for of 2 consecutive packets */
  double pkt_time_sampling; /* time of each pkt in sampling */
  uint64_t cur_epochs;      /* epoch of current pkt */
  /* timestamp for rtp header */
  uint32_t rtp_time_stamp;
  uint64_t ptp_time_cursor;
  /* in ns, tsc time cursor for packet pacing */
  uint64_t tsc_time_cursor;
  /* ptp time may onward */
  uint32_t max_onward_epochs;
  /* sometimes it may reach tx_audio_session_sync_pacing in a late time */
  uint32_t max_late_epochs;
};

#define ST30_TX_RL_QUEUES_USED (2)

enum st30_tx_rl_stage {
  ST30_TX_RL_STAGE_PREPARE,
  ST30_TX_RL_STAGE_TRANS,
  ST30_TX_RL_STAGE_WARM_UP,
};

struct st_tx_audio_session_rl_port {
  /* two queues used per port */
  struct mt_txq_entry* queue[ST30_TX_RL_QUEUES_USED];
  struct rte_mbuf* pad; /* pkt with pad_pkt_size len */
  struct rte_mbuf* pkt; /* pkt with st30_pkt_size len */
  int cur_queue;
  int cur_pkt_idx;
  uint64_t trs_target_tsc;
  /* inflight padding */
  uint32_t trs_pad_inflight_num;
  bool force_sync_first_tsc;

  uint32_t stat_pkts_burst;
  uint32_t stat_pad_pkts_burst;
  uint32_t stat_warmup_pkts_burst;
  uint32_t stat_mismatch_sync_point;
  uint32_t stat_recalculate_warmup;
  uint32_t stat_hit_backup_cp;
};

struct st_tx_audio_session_rl_info {
  /* size for padding pkt which include the header */
  uint32_t pad_pkt_size;
  int pads_per_st30_pkt;
  int pkts_per_sync;
  int pkts_prepare_warmup;
  uint32_t required_accuracy_ns;
  uint32_t max_warmup_trs;
  /* info per port */
  struct st_tx_audio_session_rl_port port_info[MTL_SESSION_PORT_MAX];
};

struct st_tx_audio_session_impl {
  int idx; /* index for current session */
  int socket_id;
  struct st30_tx_ops ops;
  char ops_name[ST_MAX_NAME_LEN];
  int recovery_idx;
  bool active;
  struct st_tx_audio_sessions_mgr* mgr;

  enum mtl_port port_maps[MTL_SESSION_PORT_MAX];
  struct rte_mempool* mbuf_mempool_hdr[MTL_SESSION_PORT_MAX];
  struct rte_mempool* mbuf_mempool_chain;
  bool tx_mono_pool; /* if reuse tx mono pool */
  bool tx_no_chain;  /* if tx not use chain mbuf */
  /* if the eth dev support chain buff */
  bool eth_has_chain[MTL_SESSION_PORT_MAX];
  /* if the eth dev support ipv4 checksum offload */
  bool eth_ipv4_cksum_offload[MTL_SESSION_PORT_MAX];
  struct rte_mbuf* inflight[MTL_SESSION_PORT_MAX];
  int inflight_cnt[MTL_SESSION_PORT_MAX]; /* for stats */
  struct mt_u64_fifo* trans_ring[MTL_SESSION_PORT_MAX];
  uint16_t trans_ring_thresh;
  struct rte_mbuf* trans_ring_inflight[MTL_SESSION_PORT_MAX];
  struct rte_ring* packet_ring;
  bool pacing_in_build; /* if control pacing in the build stage */
  /* dedicated queue tx mode */
  struct mt_txq_entry* queue[MTL_SESSION_PORT_MAX];
  bool shared_queue;

  struct st30_tx_user_stats port_user_stats;

  enum st30_tx_pacing_way tx_pacing_way;
  /* for rl based pacing */
  struct st_tx_audio_session_rl_info rl;

  uint16_t st30_frames_cnt; /* numbers of frames requested */
  struct st_frame_trans* st30_frames;
  uint32_t st30_frame_size; /* size per frame*/
  uint16_t st30_frame_idx;  /* current frame index */
  enum st30_tx_frame_status st30_frame_stat;
  int frames_per_sec;

  /* usdt dump */
  int usdt_dump_fd;
  char usdt_dump_path[64];
  int usdt_dumped_frames;

  uint16_t st30_src_port[MTL_SESSION_PORT_MAX]; /* udp port */
  uint16_t st30_dst_port[MTL_SESSION_PORT_MAX]; /* udp port */
  struct st_rfc3550_audio_hdr hdr[MTL_SESSION_PORT_MAX];

  struct st_tx_audio_session_pacing pacing;
  bool calculate_time_cursor;
  bool check_frame_done_time;

  uint16_t sample_size;
  uint16_t sample_num;
  uint32_t pkt_len;           /* data len(byte) for each pkt */
  uint32_t st30_pkt_size;     /* size for each pkt which include the header */
  int st30_total_pkts;        /* total pkts in one frame */
  int st30_pkt_idx;           /* pkt index in current frame */
  uint16_t st30_seq_id;       /* seq id for each pkt */
  uint32_t st30_rtp_time_app; /* record rtp time from app */
  uint32_t st30_rtp_time;     /* record rtp time */

  int stat_build_ret_code;
  int stat_transmit_ret_code;

  struct mt_rtcp_tx* rtcp_tx[MTL_SESSION_PORT_MAX];

  /* stat */
  rte_atomic32_t stat_frame_cnt;
  int stat_pkt_cnt[MTL_SESSION_PORT_MAX];
  /* count of frame not match the epoch */
  uint32_t stat_epoch_mismatch;
  uint32_t stat_epoch_drop;
  uint32_t stat_epoch_onward;
  uint32_t stat_epoch_late;
  uint32_t stat_error_user_timestamp;
  uint32_t stat_exceed_frame_time;
  uint64_t stat_last_time;
  uint32_t stat_max_next_frame_us;
  uint32_t stat_max_notify_frame_us;
  uint32_t stat_unrecoverable_error;
  uint32_t stat_recoverable_error;
  /* for tasklet session time measure */
  struct mt_stat_u64 stat_time;
  struct mt_stat_u64 stat_tx_delta;
};

struct st_tx_audio_sessions_mgr {
  struct mtl_main_impl* parent;
  int socket_id;
  int idx;     /* index for current sessions mgr */
  int max_idx; /* max session index */
  struct mt_sch_tasklet_impl* tasklet;

  /* all audio sessions share same ring/queue */
  struct rte_ring* ring[MTL_PORT_MAX];
  struct mt_txq_entry* queue[MTL_PORT_MAX];
  /* the last burst succ time(tsc) */
  uint64_t last_burst_succ_time_tsc[MTL_PORT_MAX];
  uint64_t tx_hang_detect_time_thresh;

  struct st_tx_audio_session_impl* sessions[ST_SCH_MAX_TX_AUDIO_SESSIONS];
  /* protect session, spin(fast) lock as it call from tasklet aslo */
  rte_spinlock_t mutex[ST_SCH_MAX_TX_AUDIO_SESSIONS]; /* protect session */

  rte_atomic32_t transmitter_started;
  rte_atomic32_t transmitter_clients;

  /* status */
  int stat_pkts_burst;
  int stat_trs_ret_code[MTL_PORT_MAX];
  uint32_t stat_unrecoverable_error;
  uint32_t stat_recoverable_error;
};

struct st_audio_transmitter_impl {
  struct mtl_main_impl* parent;
  struct st_tx_audio_sessions_mgr* mgr;
  struct mt_sch_tasklet_impl* tasklet;
  int idx; /* index for current transmitter */

  struct rte_mbuf* inflight[MTL_PORT_MAX]; /* inflight mbuf */
  int inflight_cnt[MTL_PORT_MAX];          /* for stats */
};

/* tp for every 200ms */
struct st_ra_tp_slot {
  struct st30_rx_tp_meta meta;

  int32_t dpvr_first;
  int64_t dpvr_sum;
  /* Inter-packet time(ns), packet level check */
  int64_t ipt_sum;
};

struct st_ra_tp_stat {
  uint32_t stat_compliant_result[ST_RX_TP_COMPLIANT_MAX];
  struct st_ra_tp_slot slot;
  int32_t tsdf_min;
  int32_t tsdf_max;
  int64_t tsdf_sum;
  uint32_t tsdf_cnt;
};

struct st_rx_audio_tp {
  /* time of the packet in nanoseconds */
  double pkt_time;
  /* time of the packet in sampling */
  double pkt_time_sampling;
  /* Pass Criteria*/
  /* Maximum Delta Packet vs RTP */
  /* in us, 3(1 packetization + 1 transit + 1 jitter) pkt time */
  int32_t dpvr_max_pass_narrow;
  /* in us, 19(1 packetization + 1 transit + 17 jitter) pkt tim */
  int32_t dpvr_max_pass_wide;
  /* Maximum Timestamped Delay Factor */
  int32_t tsdf_max_pass_narrow; /* in us */
  int32_t tsdf_max_pass_wide;   /* in us */

  uint64_t prev_pkt_time[MTL_SESSION_PORT_MAX]; /* ns */

  /* timing info for each frame */
  struct st_ra_tp_slot slot[MTL_SESSION_PORT_MAX];
  /* for the status */
  struct st_ra_tp_stat stat[MTL_SESSION_PORT_MAX];
  uint32_t stat_bursted_cnt[MTL_SESSION_PORT_MAX];

  uint64_t last_parse_time;
};

struct st_rx_audio_session_impl {
  int idx; /* index for current session */
  struct st_rx_audio_sessions_mgr* mgr;
  int socket_id;
  bool attached;
  struct st30_rx_ops ops;
  char ops_name[ST_MAX_NAME_LEN];
  struct st_rx_session_priv priv[MTL_SESSION_PORT_MAX];
  struct st_rx_audio_session_handle_impl* st30_handle;

  bool enable_timing_parser;
  bool enable_timing_parser_stat;
  bool enable_timing_parser_meta;
  struct st_rx_audio_tp* tp;

  enum mtl_port port_maps[MTL_SESSION_PORT_MAX];
  struct mt_rxq_entry* rxq[MTL_SESSION_PORT_MAX];

  uint16_t st30_dst_port[MTL_SESSION_PORT_MAX]; /* udp port */
  bool mcast_joined[MTL_SESSION_PORT_MAX];

  struct st_frame_trans* st30_frames;
  int st30_frames_cnt; /* numbers of frames requested */
  size_t st30_frame_size;
  struct st_frame_trans* st30_cur_frame; /* pointer to current frame */
  int frames_per_sec;

  /* pcap dumper */
  struct mt_rx_pcap pcap[MTL_SESSION_PORT_MAX];

  /* usdt dump */
  int usdt_dump_fd;
  char usdt_dump_path[64];
  int usdt_dumped_frames;

  uint32_t pkt_len;       /* data len(byte) for each pkt */
  uint32_t st30_pkt_size; /* size for each pkt which include the header */
  int st30_total_pkts;    /* total pkts in one frame */
  int st30_pkt_idx;       /* pkt index in current frame */
  int session_seq_id;     /* global session seq id to track continuity across redundant */
  int latest_seq_id[MTL_SESSION_PORT_MAX]; /* latest seq id */

  /* Redundant packet threshold guard: Accept packets after error threshold
   * to prevent deadlock when streams reset or have large timestamp jumps.
   * Handles edge case of 2^31 timestamp wraparound (highly unlikely). */
  int redundant_error_cnt[MTL_SESSION_PORT_MAX];

  uint32_t first_pkt_rtp_ts; /* rtp time stamp for the first pkt */
  uint64_t first_pkt_ptp_ts; /* PTP time stamp for the first pkt */
  int64_t tmstamp;
  size_t frame_recv_size;

  /* st30 rtp info */
  struct rte_ring* st30_rtps_ring;

  struct st30_rx_frame_meta meta; /* only for frame type */

  struct mt_rtcp_rx* rtcp_rx[MTL_SESSION_PORT_MAX];

  /* status */
  int stat_pkts_dropped;
  int stat_pkts_redundant;
  int stat_pkts_out_of_order;
  int stat_pkts_out_of_order_per_port[MTL_SESSION_PORT_MAX];
  int stat_slot_get_frame_fail;
  int stat_pkts_wrong_pt_dropped;
  int stat_pkts_wrong_ssrc_dropped;
  int stat_pkts_len_mismatch_dropped;
  int stat_pkts_received;
  rte_atomic32_t stat_frames_received;
  uint64_t stat_last_time;
  uint32_t stat_max_notify_frame_us;
  struct st30_rx_user_stats port_user_stats;
  /* for tasklet session time measure */
  struct mt_stat_u64 stat_time;
};

struct st_rx_audio_sessions_mgr {
  struct mtl_main_impl* parent;
  int idx;     /* index for current session mgr */
  int max_idx; /* max session index */
  struct mt_sch_tasklet_impl* tasklet;

  struct st_rx_audio_session_impl* sessions[ST_SCH_MAX_RX_AUDIO_SESSIONS];
  /* protect session, spin(fast) lock as it call from tasklet aslo */
  rte_spinlock_t mutex[ST_SCH_MAX_RX_AUDIO_SESSIONS];
};

struct st_tx_ancillary_session_pacing {
  double frame_time;          /* time of the frame in nanoseconds */
  double frame_time_sampling; /* time of the frame in sampling(90k) */
  uint64_t cur_epochs;        /* epoch of current frame */
  /* timestamp for rtp header */
  uint32_t rtp_time_stamp;
  uint64_t ptp_time_cursor;
  double tsc_time_cursor; /* in ns, tsc time cursor for packet pacing */
  /* ptp time may onward */
  uint32_t max_onward_epochs;
};

struct st_tx_ancillary_session_impl {
  int idx; /* index for current session */
  int socket_id;
  struct st_tx_ancillary_sessions_mgr* mgr;
  struct st40_tx_ops ops;
  char ops_name[ST_MAX_NAME_LEN];

  enum mtl_port port_maps[MTL_SESSION_PORT_MAX];
  struct rte_mempool* mbuf_mempool_hdr[MTL_SESSION_PORT_MAX];
  struct rte_mempool* mbuf_mempool_chain;
  bool tx_mono_pool; /* if reuse tx mono pool */
  bool tx_no_chain;  /* if tx not use chain mbuf */
  /* if the eth dev support chain buff */
  bool eth_has_chain[MTL_SESSION_PORT_MAX];
  /* if the eth dev support ipv4 checksum offload */
  bool eth_ipv4_cksum_offload[MTL_SESSION_PORT_MAX];
  struct rte_mbuf* inflight[MTL_SESSION_PORT_MAX];
  int inflight_cnt[MTL_SESSION_PORT_MAX]; /* for stats */
  struct rte_ring* packet_ring;
  bool second_field;

  /* dedicated queue tx mode */
  struct mt_txq_entry* queue[MTL_SESSION_PORT_MAX];
  bool shared_queue;

  uint32_t max_pkt_len; /* max data len(byte) for each pkt */

  uint16_t st40_frames_cnt; /* numbers of frames requested */
  struct st_frame_trans* st40_frames;
  uint16_t st40_frame_idx; /* current frame index */
  enum st40_tx_frame_status st40_frame_stat;

  uint16_t st40_src_port[MTL_SESSION_PORT_MAX]; /* udp port */
  uint16_t st40_dst_port[MTL_SESSION_PORT_MAX]; /* udp port */
  struct st_rfc8331_anc_hdr hdr[MTL_SESSION_PORT_MAX];

  struct st_tx_ancillary_session_pacing pacing;
  bool calculate_time_cursor;
  bool check_frame_done_time;
  struct st_fps_timing fps_tm;

  uint16_t st40_seq_id;     /* seq id for each pkt */
  uint16_t st40_ext_seq_id; /* ext seq id for each pkt */
  int st40_total_pkts;      /* total pkts in one frame */
  int st40_pkt_idx;         /* pkt index in current frame */
  int st40_rtp_time;        /* record rtp time */

  int stat_build_ret_code;

  struct mt_rtcp_tx* rtcp_tx[MTL_SESSION_PORT_MAX];

  /* stat */
  rte_atomic32_t stat_frame_cnt;
  int stat_pkt_cnt[MTL_SESSION_PORT_MAX];
  /* count of frame not match the epoch */
  uint32_t stat_epoch_mismatch;
  uint32_t stat_epoch_drop;
  uint32_t stat_epoch_onward;
  uint32_t stat_error_user_timestamp;
  uint32_t stat_exceed_frame_time;
  uint64_t stat_last_time;
  uint32_t stat_max_next_frame_us;
  uint32_t stat_max_notify_frame_us;
  /* for tasklet session time measure */
  struct mt_stat_u64 stat_time;
  /* interlace */
  uint32_t stat_interlace_first_field;
  uint32_t stat_interlace_second_field;
  struct st40_tx_user_stats port_user_stats;
};

struct st_tx_ancillary_sessions_mgr {
  struct mtl_main_impl* parent;
  int socket_id;
  int idx;     /* index for current sessions mgr */
  int max_idx; /* max session index */
  struct mt_sch_tasklet_impl* tasklet;

  /* all anc sessions share same ring/queue */
  struct rte_ring* ring[MTL_PORT_MAX];
  struct mt_txq_entry* queue[MTL_PORT_MAX];

  struct st_tx_ancillary_session_impl* sessions[ST_MAX_TX_ANC_SESSIONS];
  /* protect session, spin(fast) lock as it call from tasklet aslo */
  rte_spinlock_t mutex[ST_MAX_TX_ANC_SESSIONS];

  rte_atomic32_t transmitter_started;
  rte_atomic32_t transmitter_clients;

  /* status */
  int stat_pkts_burst;

  int stat_trs_ret_code[MTL_PORT_MAX];
};

struct st_rx_ancillary_session_impl {
  int idx; /* index for current session */
  int socket_id;
  struct st_rx_ancillary_sessions_mgr* mgr;
  bool attached;
  struct st40_rx_ops ops;
  char ops_name[ST_MAX_NAME_LEN];
  struct st_rx_session_priv priv[MTL_SESSION_PORT_MAX];
  struct st_rx_ancillary_session_handle_impl* st40_handle;

  enum mtl_port port_maps[MTL_SESSION_PORT_MAX];
  struct mt_rxq_entry* rxq[MTL_SESSION_PORT_MAX];
  struct rte_ring* packet_ring;

  uint16_t st40_dst_port[MTL_SESSION_PORT_MAX]; /* udp port */
  bool mcast_joined[MTL_SESSION_PORT_MAX];
  int session_seq_id; /* global session seq id to track continuity across redundant */
  int latest_seq_id[MTL_SESSION_PORT_MAX]; /* latest seq id */

  /* Redundant packet threshold guard: Accept packets after error threshold
   * to prevent deadlock when streams reset or have large timestamp or seq_id jumps.
   * Handles edge case of 2^31 timestamp wraparound (highly unlikely)
   * and 2^15 seq_id wraparound (unlikely). */
  int redundant_error_cnt[MTL_SESSION_PORT_MAX];

  struct mt_rtcp_rx* rtcp_rx[MTL_SESSION_PORT_MAX];

  int64_t tmstamp;
  /* status */
  rte_atomic32_t stat_frames_received;
  int stat_pkts_dropped;
  int stat_pkts_redundant;
  int stat_pkts_out_of_order;
  int stat_pkts_out_of_order_per_port[MTL_SESSION_PORT_MAX];
  int stat_pkts_enqueue_fail;
  int stat_pkts_wrong_pt_dropped;
  int stat_pkts_wrong_ssrc_dropped;
  int stat_pkts_received;
  uint64_t stat_last_time;
  uint32_t stat_max_notify_rtp_us;
  /* for tasklet session time measure */
  struct mt_stat_u64 stat_time;
  /* for interlace */
  uint32_t stat_interlace_first_field;
  uint32_t stat_interlace_second_field;
  int stat_pkts_wrong_interlace_dropped;
  struct st40_rx_user_stats port_user_stats;
};

struct st_rx_ancillary_sessions_mgr {
  struct mtl_main_impl* parent;
  int idx;     /* index for current session mgr */
  int max_idx; /* max session index */
  struct mt_sch_tasklet_impl* tasklet;

  struct st_rx_ancillary_session_impl* sessions[ST_MAX_RX_ANC_SESSIONS];
  /* protect session, spin(fast) lock as it call from tasklet aslo */
  rte_spinlock_t mutex[ST_MAX_RX_ANC_SESSIONS];
};

struct st_ancillary_transmitter_impl {
  struct mtl_main_impl* parent;
  struct st_tx_ancillary_sessions_mgr* mgr;
  struct mt_sch_tasklet_impl* tasklet;
  int idx; /* index for current transmitter */

  struct rte_mbuf* inflight[MTL_PORT_MAX]; /* inflight mbuf */
  int inflight_cnt[MTL_PORT_MAX];          /* for stats */
};

struct st_tx_fastmetadata_session_pacing {
  double frame_time;          /* time of the frame in nanoseconds */
  double frame_time_sampling; /* time of the frame in sampling(90k) */
  uint64_t cur_epochs;        /* epoch of current frame */
  /* timestamp for rtp header */
  uint32_t rtp_time_stamp;
  /* timestamp for pacing */
  uint32_t pacing_time_stamp;
  uint64_t ptp_time_cursor;
  double tsc_time_cursor; /* in ns, tsc time cursor for packet pacing */
  /* ptp time may onward */
  uint32_t max_onward_epochs;
};

struct st_tx_fastmetadata_session_impl {
  int idx; /* index for current session */
  int socket_id;
  struct st_tx_fastmetadata_sessions_mgr* mgr;
  struct st41_tx_ops ops;
  char ops_name[ST_MAX_NAME_LEN];

  enum mtl_port port_maps[MTL_SESSION_PORT_MAX];
  struct rte_mempool* mbuf_mempool_hdr[MTL_SESSION_PORT_MAX];
  struct rte_mempool* mbuf_mempool_chain;
  bool tx_mono_pool; /* if reuse tx mono pool */
  bool tx_no_chain;  /* if tx not use chain mbuf */
  /* if the eth dev support chain buff */
  bool eth_has_chain[MTL_SESSION_PORT_MAX];
  /* if the eth dev support ipv4 checksum offload */
  bool eth_ipv4_cksum_offload[MTL_SESSION_PORT_MAX];
  struct rte_mbuf* inflight[MTL_SESSION_PORT_MAX];
  int inflight_cnt[MTL_SESSION_PORT_MAX]; /* for stats */
  struct rte_ring* packet_ring;
  bool second_field;

  struct st41_tx_user_stats port_user_stats;

  /* dedicated queue tx mode */
  struct mt_txq_entry* queue[MTL_SESSION_PORT_MAX];
  bool shared_queue;

  uint32_t max_pkt_len; /* max data len(byte) for each pkt */

  uint16_t st41_frames_cnt; /* numbers of frames requested */
  struct st_frame_trans* st41_frames;
  uint16_t st41_frame_idx; /* current frame index */
  enum st41_tx_frame_status st41_frame_stat;

  uint16_t st41_src_port[MTL_SESSION_PORT_MAX]; /* udp port */
  uint16_t st41_dst_port[MTL_SESSION_PORT_MAX]; /* udp port */
  struct st41_fmd_hdr hdr[MTL_SESSION_PORT_MAX];

  struct st_tx_fastmetadata_session_pacing pacing;
  bool calculate_time_cursor;
  bool check_frame_done_time;
  struct st_fps_timing fps_tm;

  uint16_t st41_seq_id; /* seq id for each pkt */
  int st41_total_pkts;  /* total pkts in one frame */
  int st41_pkt_idx;     /* pkt index in current frame */
  int st41_rtp_time;    /* record rtp time */

  int stat_build_ret_code;

  struct mt_rtcp_tx* rtcp_tx[MTL_SESSION_PORT_MAX];

  /* stat */
  rte_atomic32_t stat_frame_cnt;
  int stat_pkt_cnt[MTL_SESSION_PORT_MAX];
  /* count of frame not match the epoch */
  uint32_t stat_epoch_mismatch;
  uint32_t stat_epoch_drop;
  uint32_t stat_epoch_onward;
  uint32_t stat_error_user_timestamp;
  uint32_t stat_exceed_frame_time;
  uint64_t stat_last_time;
  uint32_t stat_max_next_frame_us;
  uint32_t stat_max_notify_frame_us;
  /* for tasklet session time measure */
  struct mt_stat_u64 stat_time;
  /* interlace */
  uint32_t stat_interlace_first_field;
  uint32_t stat_interlace_second_field;
};

struct st_tx_fastmetadata_sessions_mgr {
  struct mtl_main_impl* parent;
  int socket_id;
  int idx;     /* index for current sessions mgr */
  int max_idx; /* max session index */
  struct mt_sch_tasklet_impl* tasklet;

  /* all fmd sessions share same ring/queue */
  struct rte_ring* ring[MTL_PORT_MAX];
  struct mt_txq_entry* queue[MTL_PORT_MAX];

  struct st_tx_fastmetadata_session_impl* sessions[ST_MAX_TX_FMD_SESSIONS];
  /* protect session, spin(fast) lock as it call from tasklet aslo */
  rte_spinlock_t mutex[ST_MAX_TX_FMD_SESSIONS];

  rte_atomic32_t transmitter_started;
  rte_atomic32_t transmitter_clients;

  /* status */
  int stat_pkts_burst;

  int stat_trs_ret_code[MTL_PORT_MAX];
};

struct st_rx_fastmetadata_session_impl {
  int idx; /* index for current session */
  int socket_id;
  struct st_rx_fastmetadata_sessions_mgr* mgr;
  bool attached;
  struct st41_rx_ops ops;
  char ops_name[ST_MAX_NAME_LEN];
  struct st_rx_session_priv priv[MTL_SESSION_PORT_MAX];
  struct st_rx_fastmetadata_session_handle_impl* st41_handle;

  enum mtl_port port_maps[MTL_SESSION_PORT_MAX];
  struct mt_rxq_entry* rxq[MTL_SESSION_PORT_MAX];
  struct rte_ring* packet_ring;

  uint16_t st41_dst_port[MTL_SESSION_PORT_MAX]; /* udp port */
  bool mcast_joined[MTL_SESSION_PORT_MAX];
  int session_seq_id; /* global session seq id to track continuity across redundant */
  int latest_seq_id[MTL_SESSION_PORT_MAX]; /* latest seq id */

  /* Redundant packet threshold guard: Accept packets after error threshold
   * to prevent deadlock when streams reset or have large timestamp or seq_id jumps.
   * Handles edge case of 2^31 timestamp wraparound (highly unlikely)
   * and 2^15 seq_id wraparound (unlikely). */
  int redundant_error_cnt[MTL_SESSION_PORT_MAX];

  struct mt_rtcp_rx* rtcp_rx[MTL_SESSION_PORT_MAX];

  /* the timestamp */
  int64_t tmstamp;
  /* status */
  rte_atomic32_t stat_frames_received;
  int stat_pkts_redundant;
  int stat_pkts_out_of_order;
  int stat_pkts_out_of_order_per_port[MTL_SESSION_PORT_MAX];
  int stat_pkts_enqueue_fail;
  int stat_pkts_wrong_pt_dropped;
  int stat_pkts_wrong_ssrc_dropped;
  int stat_pkts_received;
  uint64_t stat_last_time;
  uint32_t stat_max_notify_rtp_us;
  /* for tasklet session time measure */
  struct mt_stat_u64 stat_time;
  /* for interlace */
  uint32_t stat_interlace_first_field;
  uint32_t stat_interlace_second_field;
  int stat_pkts_wrong_interlace_dropped;
  struct st41_rx_user_stats port_user_stats;
};

struct st_rx_fastmetadata_sessions_mgr {
  struct mtl_main_impl* parent;
  int idx;     /* index for current session mgr */
  int max_idx; /* max session index */
  struct mt_sch_tasklet_impl* tasklet;

  struct st_rx_fastmetadata_session_impl* sessions[ST_MAX_RX_FMD_SESSIONS];
  /* protect session, spin(fast) lock as it call from tasklet aslo */
  rte_spinlock_t mutex[ST_MAX_RX_FMD_SESSIONS];
};

struct st_fastmetadata_transmitter_impl {
  struct mtl_main_impl* parent;
  struct st_tx_fastmetadata_sessions_mgr* mgr;
  struct mt_sch_tasklet_impl* tasklet;
  int idx; /* index for current transmitter */

  struct rte_mbuf* inflight[MTL_PORT_MAX]; /* inflight mbuf */
  int inflight_cnt[MTL_PORT_MAX];          /* for stats */
};

struct st22_get_encoder_request {
  enum st_plugin_device device;
  struct st22_encoder_create_req req;

  void* priv;
  struct st22_encode_frame_meta* (*get_frame)(void* priv);
  int (*wake_block)(void* priv);
  int (*set_block_timeout)(void* priv, uint64_t timedwait_ns);
  int (*put_frame)(void* priv, struct st22_encode_frame_meta* frame, int result);
  int (*dump)(void* priv);
};

struct st22_get_decoder_request {
  enum st_plugin_device device;
  struct st22_decoder_create_req req;

  void* priv;
  struct st22_decode_frame_meta* (*get_frame)(void* priv);
  int (*wake_block)(void* priv);
  int (*set_block_timeout)(void* priv, uint64_t timedwait_ns);
  int (*put_frame)(void* priv, struct st22_decode_frame_meta* frame, int result);
  int (*dump)(void* priv);
};

struct st20_get_converter_request {
  enum st_plugin_device device;
  struct st20_converter_create_req req;

  void* priv;
  struct st20_convert_frame_meta* (*get_frame)(void* priv);
  int (*put_frame)(void* priv, struct st20_convert_frame_meta* frame, int result);
  int (*dump)(void* priv);
};

struct st22_encode_session_impl {
  int idx;
  void* parent; /* point to struct st22_encode_dev_impl */
  st22_encode_priv session;
  enum mt_handle_type type; /* for sanity check */

  size_t codestream_max_size;

  struct st22_get_encoder_request req;
};

struct st22_encode_dev_impl {
  enum mt_handle_type type; /* for sanity check */
  struct mtl_main_impl* parent;
  int idx;
  char name[ST_MAX_NAME_LEN];
  struct st22_encoder_dev dev;
  rte_atomic32_t ref_cnt;
  struct st22_encode_session_impl sessions[ST_MAX_SESSIONS_PER_ENCODER];
};

struct st22_decode_session_impl {
  int idx;
  void* parent; /* point to struct st22_decode_dev_impl */
  st22_decode_priv session;
  enum mt_handle_type type; /* for sanity check */

  struct st22_get_decoder_request req;
};

struct st22_decode_dev_impl {
  enum mt_handle_type type; /* for sanity check */
  struct mtl_main_impl* parent;
  int idx;
  char name[ST_MAX_NAME_LEN];
  struct st22_decoder_dev dev;
  rte_atomic32_t ref_cnt;
  struct st22_decode_session_impl sessions[ST_MAX_SESSIONS_PER_DECODER];
};

struct st20_convert_session_impl {
  int idx;
  void* parent; /* point to struct st20_convert_dev_impl */
  st20_convert_priv session;
  enum mt_handle_type type; /* for sanity check */

  struct st20_get_converter_request req;
};

struct st20_convert_dev_impl {
  enum mt_handle_type type; /* for sanity check */
  struct mtl_main_impl* parent;
  int idx;
  char name[ST_MAX_NAME_LEN];
  struct st20_converter_dev dev;
  rte_atomic32_t ref_cnt;
  struct st20_convert_session_impl sessions[ST_MAX_SESSIONS_PER_CONVERTER];
};

struct st_dl_plugin_impl {
  int idx;
  char path[ST_PLUGIN_MAX_PATH_LEN];
  void* dl_handle;
  st_plugin_create_fn create;
  st_plugin_priv handle;
  st_plugin_free_fn free;
  struct st_plugin_meta meta;
};

struct st_plugin_mgr {
  pthread_mutex_t lock; /* lock for encode_devs/decode_devs */
  struct st22_encode_dev_impl* encode_devs[ST_MAX_ENCODER_DEV];
  struct st22_decode_dev_impl* decode_devs[ST_MAX_DECODER_DEV];
  struct st20_convert_dev_impl* convert_devs[ST_MAX_CONVERTER_DEV];
  pthread_mutex_t plugins_lock; /* lock for plugins */
  struct st_dl_plugin_impl* plugins[ST_MAX_DL_PLUGINS];
  int plugins_nb;
};

struct st_tx_video_session_handle_impl {
  struct mtl_main_impl* parent;
  enum mt_handle_type type;
  struct mtl_sch_impl* sch; /* the sch this session attached */
  int quota_mbs;            /* data quota for this session */
  struct st_tx_video_session_impl* impl;
};

struct st22_tx_video_session_handle_impl {
  struct mtl_main_impl* parent;
  enum mt_handle_type type;
  struct mtl_sch_impl* sch; /* the sch this session attached */
  int quota_mbs;            /* data quota for this session */
  struct st_tx_video_session_impl* impl;
};

struct st_tx_audio_session_handle_impl {
  struct mtl_main_impl* parent;
  enum mt_handle_type type;
  struct mtl_sch_impl* sch; /* the sch this session attached */
  int quota_mbs;            /* data quota for this session */
  struct st_tx_audio_session_impl* impl;
};

struct st_tx_ancillary_session_handle_impl {
  struct mtl_main_impl* parent;
  enum mt_handle_type type;
  struct mtl_sch_impl* sch; /* the sch this session attached */
  int quota_mbs;            /* data quota for this session */
  struct st_tx_ancillary_session_impl* impl;
};

struct st_tx_fastmetadata_session_handle_impl {
  struct mtl_main_impl* parent;
  enum mt_handle_type type;
  struct mtl_sch_impl* sch; /* the sch this session attached */
  int quota_mbs;            /* data quota for this session */
  struct st_tx_fastmetadata_session_impl* impl;
};

struct st_rx_video_session_handle_impl {
  struct mtl_main_impl* parent;
  enum mt_handle_type type;
  struct mtl_sch_impl* sch; /* the sch this session attached */
  int quota_mbs;            /* data quota for this session */
  struct st_rx_video_session_impl* impl;
};

struct st22_rx_video_session_handle_impl {
  struct mtl_main_impl* parent;
  enum mt_handle_type type;
  struct mtl_sch_impl* sch; /* the sch this session attached */
  int quota_mbs;            /* data quota for this session */
  struct st_rx_video_session_impl* impl;
};

struct st_rx_audio_session_handle_impl {
  struct mtl_main_impl* parent;
  enum mt_handle_type type;
  struct mtl_sch_impl* sch; /* the sch this session attached */
  int quota_mbs;            /* data quota for this session */
  struct st_rx_audio_session_impl* impl;
};

struct st_rx_ancillary_session_handle_impl {
  struct mtl_main_impl* parent;
  enum mt_handle_type type;
  struct mtl_sch_impl* sch; /* the sch this session attached */
  int quota_mbs;            /* data quota for this session */
  struct st_rx_ancillary_session_impl* impl;
};

struct st_rx_fastmetadata_session_handle_impl {
  struct mtl_main_impl* parent;
  enum mt_handle_type type;
  struct mtl_sch_impl* sch; /* the sch this session attached */
  int quota_mbs;            /* data quota for this session */
  struct st_rx_fastmetadata_session_impl* impl;
};

static inline bool st20_is_frame_type(enum st20_type type) {
  if ((type == ST20_TYPE_FRAME_LEVEL) || (type == ST20_TYPE_SLICE_LEVEL))
    return true;
  else
    return false;
}

#endif
