/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef _MT_LIB_ST_HEAD_H_
#define _MT_LIB_ST_HEAD_H_

#include <st20_api.h>
#include <st30_api.h>
#include <st40_api.h>
#include <st_pipeline_api.h>

#include "../mt_header.h"
#include "st_convert.h"
#include "st_fmt.h"
#include "st_pkt.h"

#define ST_MAX_NAME_LEN (32)

#define ST_PLUNGIN_MAX_PATH_LEN (128)

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
#define ST_TX_VIDEO_SESSIONS_RING_SIZE (512)

/* number of tmstamp it will tracked for out of order pkts */
#define ST_VIDEO_RX_REC_NUM_OFO (2)
/* number of slices it will tracked as out of order pkts */
#define ST_VIDEO_RX_SLICE_NUM (32)
/* sync to atomic if reach this threshold */
#define ST_VIDEO_STAT_UPDATE_INTERVAL (1000)
/* data size for each pkt in block packing mode */
#define ST_VIDEO_BPM_SIZE (1260)

/* max tx/rx audio(st_30) sessions */
#define ST_MAX_TX_AUDIO_SESSIONS (180)
#define ST_TX_AUDIO_SESSIONS_RING_SIZE (512)
#define ST_MAX_RX_AUDIO_SESSIONS (180)
/* max tx/rx anc(st_40) sessions */
#define ST_MAX_TX_ANC_SESSIONS (180)
#define ST_TX_ANC_SESSIONS_RING_SIZE (512)
#define ST_MAX_RX_ANC_SESSIONS (180)

/* max dl plugin lib number */
#define ST_MAX_DL_PLUGINS (8)
/* max encoder devices number */
#define ST_MAX_ENCODER_DEV (8)
/* max decoder devices number */
#define ST_MAX_DECODER_DEV (8)
/* max converter devices number */
#define ST_MAX_CONVERTER_DEV (8)
/* max sessions number per encoder */
#define ST_MAX_SESSIIONS_PER_ENCODER (16)
/* max sessions number per decoder */
#define ST_MAX_SESSIIONS_PER_DECODER (16)
/* max sessions number per converter */
#define ST_MAX_SESSIIONS_PER_CONVERTER (16)

#define ST_TX_DUMMY_PKT_IDX (0xFFFFFFFF)

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

struct st_tx_muf_priv_data {
  uint64_t tsc_time_stamp; /* tsc time stamp of current mbuf */
  uint64_t ptp_time_stamp; /* ptp time stamp of current mbuf */
  uint32_t idx;            /* index inside current frame */
};

/* info passing between dma and rx */
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

/* describe the frame used in transport(both tx and rx) */
struct st_frame_trans {
  /* todo: use struct st_frame as base */
  int idx;
  void* addr;            /* virtual address */
  rte_iova_t iova;       /* iova for hw */
  rte_atomic32_t refcnt; /* 0 means it's free */
  void* priv;            /* private data for lib */

  uint32_t flags;                          /* ST_FT_FLAG_* */
  struct rte_mbuf_ext_shared_info sh_info; /* for st20 tx ext shared */

  /* metadata */
  union {
    struct st20_tx_frame_meta tv_meta;
    struct st22_tx_frame_meta tx_st22_meta;
    struct st20_rx_frame_meta rv_meta; /* not use now */
    struct st30_tx_frame_meta ta_meta;
    struct st30_rx_frame_meta ra_meta; /* not use now */
    struct st40_tx_frame_meta tc_meta;
  };
};

/* timing for pacing */
struct st_tx_video_pacing {
  double trs;             /* in ns for of 2 consecutive packets, T-Frame / N-Packets */
  double tr_offset;       /* in ns, tr offset time of each frame */
  uint32_t tr_offset_vrx; /* packets unit, VRX start value of each frame */
  double frame_time;      /* time of the frame in nanoseconds */
  double frame_time_sampling; /* time of the frame in sampling(90k) */
  uint32_t warm_pkts;         /* pkts for RL pacing warm boot */
  float pad_interval;         /* padding pkt interval(pkts level) for RL pacing */

  uint64_t cur_epochs; /* epoch of current frame */
  /* timestamp for rtp header */
  uint32_t rtp_time_stamp;
  /* timestamp for pacing */
  uint32_t pacing_time_stamp;
  double tsc_time_cursor; /* in ns, tsc time cursor for packet pacing */
  double ptp_time_cursor; /* in ns, ptp time cursor for packet pacing */
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
  int st22_min_pkts;
};

struct st_vsync_info {
  struct st10_vsync_meta meta;
  uint64_t next_epoch_tsc;
  bool init;
};

struct st_tx_video_session_impl {
  enum mtl_port port_maps[MTL_SESSION_PORT_MAX];
  struct rte_mempool* mbuf_mempool_hdr[MTL_SESSION_PORT_MAX];
  bool mbuf_mempool_reuse_rx[MTL_SESSION_PORT_MAX]; /* af_xdp zero copy */
  struct rte_mempool* mbuf_mempool_chain;
  bool tx_mono_pool; /* if reuse tx mono pool */
  /* if the eth dev support chain buff */
  bool eth_has_chain[MTL_SESSION_PORT_MAX];
  /* if the eth dev support ipv4 checksum offload */
  bool eth_ipv4_cksum_offload[MTL_SESSION_PORT_MAX];
  unsigned int ring_count;
  struct rte_ring* ring[MTL_SESSION_PORT_MAX];
  struct rte_ring* packet_ring; /* rtp ring */
  uint16_t port_id[MTL_SESSION_PORT_MAX];
  struct mt_tx_queue* queue[MTL_SESSION_PORT_MAX];
  int idx; /* index for current tx_session */
  uint64_t advice_sleep_us;

  struct st_tx_video_session_handle_impl* st20_handle;
  struct st22_tx_video_session_handle_impl* st22_handle;

  uint16_t st20_ipv4_packet_id;
  uint16_t st20_src_port[MTL_SESSION_PORT_MAX]; /* udp port */
  uint16_t st20_dst_port[MTL_SESSION_PORT_MAX]; /* udp port */
  struct st_rfc4175_video_hdr s_hdr[MTL_SESSION_PORT_MAX];

  struct st_tx_video_pacing pacing;
  enum st21_tx_pacing_way pacing_way[MTL_SESSION_PORT_MAX];
  int (*pacing_tasklet_func[MTL_SESSION_PORT_MAX])(struct mtl_main_impl* impl,
                                                   struct st_tx_video_session_impl* s,
                                                   enum mtl_session_port s_port);

  struct st_vsync_info vsync;

  struct st20_tx_ops ops;
  char ops_name[ST_MAX_NAME_LEN];
  enum mt_handle_type s_type; /* st22 or st20 */

  unsigned int bulk; /* Enqueue bulk objects on the ring */
  struct rte_mbuf* inflight[MTL_SESSION_PORT_MAX][ST_SESSION_MAX_BULK];
  bool has_inflight[MTL_SESSION_PORT_MAX];
  int inflight_cnt[MTL_SESSION_PORT_MAX]; /* for stats */

  /* info for transmitter */
  uint64_t trs_target_tsc[MTL_SESSION_PORT_MAX];
  struct rte_mbuf* trs_inflight[MTL_SESSION_PORT_MAX][ST_SESSION_MAX_BULK];
  unsigned int trs_inflight_num[MTL_SESSION_PORT_MAX];
  unsigned int trs_inflight_idx[MTL_SESSION_PORT_MAX];
  unsigned int trs_pad_inflight_num[MTL_SESSION_PORT_MAX]; /* inflight padding */
  int trs_inflight_cnt[MTL_SESSION_PORT_MAX];              /* for stats */
  struct rte_mbuf* trs_inflight2[MTL_SESSION_PORT_MAX][ST_SESSION_MAX_BULK];
  unsigned int trs_inflight_num2[MTL_SESSION_PORT_MAX];
  unsigned int trs_inflight_idx2[MTL_SESSION_PORT_MAX];
  int trs_inflight_cnt2[MTL_SESSION_PORT_MAX]; /* for stats */

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
  float cpu_busy_score;
  int pri_nic_burst_cnt; /* sync to atomic if this reach a threshold */
  int pri_nic_inflight_cnt;
  rte_atomic32_t nic_burst_cnt;
  rte_atomic32_t nic_inflight_cnt;

  /* info for st22 */
  struct st22_tx_video_info* st22_info;
  uint16_t st22_box_hdr_length;
  size_t st22_codestream_size;

  /* stat */
  rte_atomic32_t stat_frame_cnt;
  int stat_pkts_build;
  int stat_pkts_dummy;
  int stat_pkts_burst;
  int stat_pkts_burst_dummy;
  int stat_trs_ret_code[MTL_SESSION_PORT_MAX];
  int stat_build_ret_code;
  uint64_t stat_last_time;
  uint32_t stat_epoch_drop;
  uint32_t stat_error_user_timestamp;
  uint32_t stat_epoch_troffset_mismatch; /* pacing mismatch the epoch troffset */
  uint32_t stat_trans_troffset_mismatch; /* transmitter mismatch the epoch troffset */
  uint32_t stat_exceed_frame_time;
  bool stat_user_busy_first;
  uint32_t stat_user_busy;       /* get_next_frame or dequeue_bulk from rtp ring fail */
  uint32_t stat_lines_not_ready; /* query app lines not ready */
  uint32_t stat_vsync_mismatch;
};

struct st_tx_video_sessions_mgr {
  struct mtl_main_impl* parnet;
  int idx;     /* index for current session mgr */
  int max_idx; /* max session index */
  struct mt_sch_tasklet_impl* tasklet;

  struct st_tx_video_session_impl* sessions[ST_SCH_MAX_TX_VIDEO_SESSIONS];
  /* protect session, spin(fast) lock as it call from tasklet aslo */
  rte_spinlock_t mutex[ST_SCH_MAX_TX_VIDEO_SESSIONS];
};

struct st_video_transmitter_impl {
  struct mtl_main_impl* parnet;
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
  uint32_t tmstamp;
  uint16_t seq_id_base;     /* seq id for the first packt */
  uint32_t seq_id_base_u32; /* seq id for the first packt with u32 */
  bool seq_id_got;
  void* frame; /* only for frame type */
  rte_iova_t frame_iova;
  uint8_t* frame_bitmap;
  size_t frame_recv_size;           /* for frame type */
  size_t pkt_lcore_frame_recv_size; /* frame_recv_size for pkt lcore */
  uint32_t pkts_received;
  uint32_t pkts_redunant_received;
  struct st20_rx_frame_meta meta;      /* only for frame type */
  struct st22_rx_frame_meta st22_meta; /* only for st22 frame type */
  /* Second field type indicate */
  bool second_field;
  struct st_rx_video_slot_slice_info* slice_info; /* for ST20_TYPE_SLICE_LEVEL */
  /* payload len for codestream packetization mode */
  uint16_t st22_payload_length;
  uint16_t st22_box_hdr_length;
};

struct st_rx_video_ebu_info {
  double trs;        /* in ns for of 2 consecutive packets, T-Frame / N-Packets */
  double tr_offset;  /* in ns, tr offset time of each frame */
  double frame_time; /* time of the frame in nanoseconds */
  double frame_time_sampling; /* time of the frame in sampling(90k) */
  int dropped_results;        /* number of results to drop at the beginning */

  // pass criteria
  uint32_t c_max_narrow_pass;
  uint32_t c_max_wide_pass;
  uint32_t vrx_full_narrow_pass;
  uint32_t vrx_full_wide_pass;
  uint32_t rtp_offset_max_pass;

  bool init;
};

struct st_rx_video_ebu_stat {
  uint64_t cur_epochs; /* epoch of current frame */
  int frame_idx;
  bool compliant;
  bool compliant_narrow;

  /* Cinst, packet level check */
  uint64_t cinmtl_initial_time;
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

struct st_rx_video_ebu_result {
  int ebu_result_num;
  int cinst_pass_narrow;
  int cinst_pass_wide;
  int cinst_fail;
  int vrx_pass_narrow;
  int vrx_pass_wide;
  int vrx_fail;
  int latency_pass;
  int latency_fail;
  int rtp_offset_pass;
  int rtp_offset_fail;
  int rtp_ts_delta_pass;
  int rtp_ts_delta_fail;
  int fpt_pass;
  int fpt_fail;
  int compliance;
  int compliance_narrow;
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
  enum mtl_port port;
};

struct st_rx_video_session_impl {
  int idx; /* index for current session */
  struct st_rx_video_sessions_mgr* parnet;
  struct st_rx_session_priv priv[MTL_SESSION_PORT_MAX];

  struct st20_rx_ops ops;
  char ops_name[ST_MAX_NAME_LEN];
  uint64_t advice_sleep_us;

  enum mtl_port port_maps[MTL_SESSION_PORT_MAX];
  struct mt_rx_queue* queue[MTL_SESSION_PORT_MAX];
  struct mt_rss_entry* rss[MTL_SESSION_PORT_MAX];
  uint16_t port_id[MTL_SESSION_PORT_MAX];
  uint16_t st20_src_port[MTL_SESSION_PORT_MAX]; /* udp port */
  uint16_t st20_dst_port[MTL_SESSION_PORT_MAX]; /* udp port */

  struct st_rx_video_session_handle_impl* st20_handle;
  struct st22_rx_video_session_handle_impl* st22_handle;

  struct st22_rx_video_info* st22_info;

  bool is_hdr_split;
  struct st_rx_video_hdr_split_info hdr_split_info[MTL_SESSION_PORT_MAX];

  /* st20 detector info */
  struct st_rx_video_detector detector;

  struct st_vsync_info vsync;

  /* frames info */
  size_t st20_frame_size;        /* size per frame, without padding */
  size_t st20_fb_size;           /* frame buffer size, with lines' padding */
  size_t st20_linesize;          /* line size including padding bytes */
  size_t st20_bytes_in_line;     /* bytes per line not including padding */
  size_t st20_frame_bitmap_size; /* bitmap size per frame */
  int st20_frames_cnt;           /* numbers of frames requested */
  struct st_frame_trans* st20_frames;
  struct st20_pgroup st20_pg;

  size_t st20_uframe_size; /* size per user frame */
  struct st20_rx_uframe_pg_meta pg_meta;

  /* rtp info */
  struct rte_ring* rtps_ring;

  /* record two frames in case pkts out of order within marker */
  struct st_rx_video_slot_impl slots[ST_VIDEO_RX_REC_NUM_OFO];
  int slot_idx;
  int slot_max;

  /* slice info */
  uint32_t slice_lines;
  size_t slice_size;
  struct st20_rx_slice_meta slice_meta;

  /* dma dev */
  bool dma_copy;
  struct mtl_dma_lender_dev* dma_dev;
  uint16_t dma_nb_desc;
  struct st_rx_video_slot_impl* dma_slot;
#ifdef ST_PCAPNG_ENABLED
  /* pcap dumper */
  uint32_t pcapng_dumped_pkts;
  uint32_t pcapng_dropped_pkts;
  uint32_t pcapng_max_pkts;
  struct rte_pcapng* pcapng;
  struct rte_mempool* pcapng_pool;
#endif
  /* additional lcore for pkt handling */
  unsigned int pkt_lcore;
  bool has_pkt_lcore;
  struct rte_ring* pkt_lcore_ring;
  rte_atomic32_t pkt_lcore_active;
  rte_atomic32_t pkt_lcore_stopped;

  /* the cpu resource to handle rx, 0: full, 100: cpu is very busy */
  float cpu_busy_score;
  float dma_busy_score;
  int pri_nic_burst_cnt; /* sync to atomic if this reach a threshold */
  int pri_nic_inflight_cnt;
  rte_atomic32_t nic_burst_cnt;
  rte_atomic32_t nic_inflight_cnt;
  rte_atomic32_t dma_previous_busy_cnt;
  rte_atomic32_t cbs_frame_slot_cnt;
  rte_atomic32_t cbs_incomplete_frame_cnt;

  /* status */
  int stat_pkts_idx_dropped;
  int stat_pkts_idx_oo_bitmap;
  int stat_pkts_enqueue_fallback; /* for pkt lcore */
  int stat_pkts_offset_dropped;
  int stat_pkts_redunant_dropped;
  int stat_pkts_wrong_hdr_dropped;
  int stat_pkts_received;
  int stat_pkts_multi_segments_received;
  int stat_pkts_dma;
  int stat_pkts_rtp_ring_full;
  int stat_pkts_no_slot;
  int stat_pkts_not_bpm;
  int stat_pkts_copy_hdr_split;
  int stat_pkts_wrong_payload_hdr_split;
  int stat_mismatch_hdr_split_frame;
  int stat_frames_dropped;
  rte_atomic32_t stat_frames_received;
  int stat_slices_received;
  int stat_pkts_slice_fail;
  int stat_pkts_slice_merged;
  uint64_t stat_last_time;
  uint32_t stat_vsync_mismatch;
  uint32_t stat_slot_get_frame_fail;
  uint32_t stat_slot_query_ext_fail;

  struct st_rx_video_ebu_info ebu_info;
  struct st_rx_video_ebu_stat ebu;
  struct st_rx_video_ebu_result ebu_result;

  /* ret > 0 if it's handled by DMA */
  int (*pkt_handler)(struct st_rx_video_session_impl* s, struct rte_mbuf* mbuf,
                     enum mtl_session_port s_port, bool ctrl_thread);
};

struct st_rx_video_sessions_mgr {
  struct mtl_main_impl* parnet;
  int idx;     /* index for current session mgr */
  int max_idx; /* max session index */
  struct mt_sch_tasklet_impl* tasklet;

  struct st_rx_video_session_impl* sessions[ST_SCH_MAX_RX_VIDEO_SESSIONS];
  /* protect session, spin(fast) lock as it call from tasklet aslo */
  rte_spinlock_t mutex[ST_SCH_MAX_RX_VIDEO_SESSIONS];
};

struct st_tx_audio_session_pacing {
  double trs;                 /* in ns for of 2 consecutive packets */
  double frame_time;          /* time of the frame in nanoseconds */
  double frame_time_sampling; /* time of the frame in sampling(48k) */
  uint64_t cur_epochs;        /* epoch of current frame */
  /* timestamp for rtp header */
  uint32_t rtp_time_stamp;
  /* timestamp for pacing */
  uint32_t pacing_time_stamp;
  uint64_t tsc_time_cursor; /* in ns, tsc time cursor for packet pacing */
};

struct st_tx_audio_session_impl {
  int idx; /* index for current session */
  struct st30_tx_ops ops;
  char ops_name[ST_MAX_NAME_LEN];

  enum mtl_port port_maps[MTL_SESSION_PORT_MAX];
  struct rte_mempool* mbuf_mempool_hdr[MTL_SESSION_PORT_MAX];
  struct rte_mempool* mbuf_mempool_chain;
  bool tx_mono_pool; /* if reuse tx mono pool */
  /* if the eth dev support chain buff */
  bool eth_has_chain[MTL_SESSION_PORT_MAX];
  /* if the eth dev support ipv4 checksum offload */
  bool eth_ipv4_cksum_offload[MTL_SESSION_PORT_MAX];
  struct rte_mbuf* inflight[MTL_SESSION_PORT_MAX];
  bool has_inflight[MTL_SESSION_PORT_MAX];
  int inflight_cnt[MTL_SESSION_PORT_MAX]; /* for stats */
  struct rte_ring* packet_ring;

  uint16_t st30_frames_cnt; /* numbers of frames requested */
  struct st_frame_trans* st30_frames;
  uint32_t st30_frame_size; /* size per frame*/
  uint16_t st30_frame_idx;  /* current frame index */
  enum st30_tx_frame_status st30_frame_stat;

  uint16_t st30_ipv4_packet_id;
  uint16_t st30_src_port[MTL_SESSION_PORT_MAX]; /* udp port */
  uint16_t st30_dst_port[MTL_SESSION_PORT_MAX]; /* udp port */
  struct st_rfc3550_audio_hdr hdr[MTL_SESSION_PORT_MAX];

  struct st_tx_audio_session_pacing pacing;

  uint32_t pkt_len;           /* data len(byte) for each pkt */
  uint32_t st30_pkt_size;     /* size for each pkt which include the header */
  int st30_total_pkts;        /* total pkts in one frame */
  int st30_pkt_idx;           /* pkt index in current frame */
  uint16_t st30_seq_id;       /* seq id for each pkt */
  uint32_t st30_rtp_time_app; /* record rtp time from app */
  uint32_t st30_rtp_time;     /* record rtp time */

  int stat_build_ret_code;

  /* stat */
  rte_atomic32_t st30_stat_frame_cnt;
  int st30_stat_pkt_cnt;
  uint32_t st30_epoch_mismatch; /* count of frame lower than pacing */
  uint32_t stat_error_user_timestamp;
};

struct st_tx_audio_sessions_mgr {
  struct mtl_main_impl* parnet;
  int idx;     /* index for current sessions mgr */
  int max_idx; /* max session index */
  struct mt_sch_tasklet_impl* tasklet;

  /* all audio sessions share same ring/queue */
  struct rte_ring* ring[MTL_PORT_MAX];
  uint16_t port_id[MTL_PORT_MAX];
  struct mt_tx_queue* queue[MTL_PORT_MAX];

  struct st_tx_audio_session_impl* sessions[ST_MAX_TX_AUDIO_SESSIONS];
  /* protect session, spin(fast) lock as it call from tasklet aslo */
  rte_spinlock_t mutex[ST_MAX_TX_AUDIO_SESSIONS]; /* protect session */

  rte_atomic32_t transmitter_started;

  /* status */
  int st30_stat_pkts_burst;

  int stat_trs_ret_code[MTL_PORT_MAX];
};

struct st_audio_transmitter_impl {
  struct mtl_main_impl* parnet;
  struct st_tx_audio_sessions_mgr* mgr;
  struct mt_sch_tasklet_impl* tasklet;
  int idx; /* index for current transmitter */

  struct rte_mbuf* inflight[MTL_PORT_MAX]; /* inflight mbuf */
  int inflight_cnt[MTL_PORT_MAX];          /* for stats */
};

struct st_rx_audio_ebu_info {
  double frame_time;          /* time of the frame in nanoseconds */
  double frame_time_sampling; /* time of the frame in sampling */
  int dropped_results;        /* number of results to drop at the beginning */

  /* Pass Criteria */
  int32_t dpvr_max_pass_narrow;
  int32_t dpvr_max_pass_wide;
  float dpvr_avg_pass_wide;
  int32_t tsdf_max_pass;
};

struct st_rx_audio_ebu_stat {
  uint32_t pkt_num;
  bool compliant;

  /* Delta Packet vs RTP */
  int64_t dpvr_max;
  int64_t dpvr_min;
  uint32_t dpvr_cnt;
  uint64_t dpvr_sum;
  float dpvr_avg;
  int64_t dpvr_first;

  /* Maximum Timestamped Delay Factor */
  int64_t tsdf_max;
};

struct st_rx_audio_ebu_result {
  int ebu_result_num;
  int dpvr_pass_narrow;
  int dpvr_pass_wide;
  int dpvr_fail;
  int tsdf_pass;
  int tsdf_fail;
  int compliance;
};

struct st_rx_audio_session_impl {
  int idx; /* index for current session */
  struct st30_rx_ops ops;
  char ops_name[ST_MAX_NAME_LEN];
  struct st_rx_session_priv priv[MTL_SESSION_PORT_MAX];

  enum mtl_port port_maps[MTL_SESSION_PORT_MAX];
  struct mt_rx_queue* queue[MTL_SESSION_PORT_MAX];
  struct mt_rss_entry* rss[MTL_SESSION_PORT_MAX];
  uint16_t port_id[MTL_SESSION_PORT_MAX];

  uint16_t st30_src_port[MTL_SESSION_PORT_MAX]; /* udp port */
  uint16_t st30_dst_port[MTL_SESSION_PORT_MAX]; /* udp port */

  struct st_frame_trans* st30_frames;
  int st30_frames_cnt; /* numbers of frames requested */
  size_t st30_frame_size;
  void* st30_cur_frame; /* pointer to current frame */

  uint32_t pkt_len;       /* data len(byte) for each pkt */
  uint32_t st30_pkt_size; /* size for each pkt which include the header */
  int st30_total_pkts;    /* total pkts in one frame */
  int st30_pkt_idx;       /* pkt index in current frame */
  int st30_seq_id;        /* seq id for each pkt */

  uint32_t tmstamp;
  size_t frame_recv_size;

  /* st30 rtp info */
  struct rte_ring* st30_rtps_ring;

  struct st30_rx_frame_meta meta; /* only for frame type */

  /* status */
  int st30_stat_pkts_dropped;
  int st30_stat_pkts_wrong_hdr_dropped;
  int st30_stat_pkts_received;
  int st30_stat_frames_dropped;
  rte_atomic32_t st30_stat_frames_received;
  int st30_stat_pkts_rtp_ring_full;
  uint64_t st30_stat_last_time;
  struct st_rx_audio_ebu_info ebu_info;
  struct st_rx_audio_ebu_stat ebu;
  struct st_rx_audio_ebu_result ebu_result;
};

struct st_rx_audio_sessions_mgr {
  struct mtl_main_impl* parnet;
  int idx;     /* index for current session mgr */
  int max_idx; /* max session index */
  struct mt_sch_tasklet_impl* tasklet;

  struct st_rx_audio_session_impl* sessions[ST_MAX_RX_AUDIO_SESSIONS];
  /* protect session, spin(fast) lock as it call from tasklet aslo */
  rte_spinlock_t mutex[ST_MAX_RX_AUDIO_SESSIONS];
};

struct st_tx_ancillary_session_pacing {
  double frame_time;          /* time of the frame in nanoseconds */
  double frame_time_sampling; /* time of the frame in sampling(90k) */
  uint64_t cur_epochs;        /* epoch of current frame */
  /* timestamp for rtp header */
  uint32_t rtp_time_stamp;
  /* timestamp for pacing */
  uint32_t pacing_time_stamp;
  double tsc_time_cursor; /* in ns, tsc time cursor for packet pacing */
};

struct st_tx_ancillary_session_impl {
  int idx; /* index for current session */
  struct st40_tx_ops ops;
  char ops_name[ST_MAX_NAME_LEN];

  enum mtl_port port_maps[MTL_SESSION_PORT_MAX];
  struct rte_mempool* mbuf_mempool_hdr[MTL_SESSION_PORT_MAX];
  struct rte_mempool* mbuf_mempool_chain;
  bool tx_mono_pool; /* if reuse tx mono pool */
  /* if the eth dev support chain buff */
  bool eth_has_chain[MTL_SESSION_PORT_MAX];
  /* if the eth dev support ipv4 checksum offload */
  bool eth_ipv4_cksum_offload[MTL_SESSION_PORT_MAX];
  struct rte_mbuf* inflight[MTL_SESSION_PORT_MAX];
  bool has_inflight[MTL_SESSION_PORT_MAX];
  int inflight_cnt[MTL_SESSION_PORT_MAX]; /* for stats */
  struct rte_ring* packet_ring;

  uint32_t max_pkt_len; /* max data len(byte) for each pkt */

  uint16_t st40_frames_cnt; /* numbers of frames requested */
  struct st_frame_trans* st40_frames;
  uint16_t st40_frame_idx; /* current frame index */
  enum st40_tx_frame_status st40_frame_stat;

  uint16_t st40_ipv4_packet_id;
  uint16_t st40_src_port[MTL_SESSION_PORT_MAX]; /* udp port */
  uint16_t st40_dst_port[MTL_SESSION_PORT_MAX]; /* udp port */
  struct st_rfc8331_anc_hdr hdr[MTL_SESSION_PORT_MAX];

  struct st_tx_ancillary_session_pacing pacing;
  struct st_fps_timing fps_tm;

  uint16_t st40_seq_id;     /* seq id for each pkt */
  uint16_t st40_ext_seq_id; /* ext seq id for each pkt */
  int st40_total_pkts;      /* total pkts in one frame */
  int st40_pkt_idx;         /* pkt index in current frame */
  int st40_rtp_time;        /* record rtp time */

  int stat_build_ret_code;

  /* stat */
  rte_atomic32_t st40_stat_frame_cnt;
  int st40_stat_pkt_cnt;
  uint32_t st40_epoch_mismatch; /* count of frame lower than pacing */
  uint32_t stat_error_user_timestamp;
};

struct st_tx_ancillary_sessions_mgr {
  struct mtl_main_impl* parnet;
  int idx;     /* index for current sessions mgr */
  int max_idx; /* max session index */
  struct mt_sch_tasklet_impl* tasklet;

  /* all anc sessions share same ring/queue */
  struct rte_ring* ring[MTL_PORT_MAX];
  uint16_t port_id[MTL_PORT_MAX];
  struct mt_tx_queue* queue[MTL_PORT_MAX];

  struct st_tx_ancillary_session_impl* sessions[ST_MAX_TX_ANC_SESSIONS];
  /* protect session, spin(fast) lock as it call from tasklet aslo */
  rte_spinlock_t mutex[ST_MAX_TX_ANC_SESSIONS];

  rte_atomic32_t transmitter_started;

  /* status */
  int st40_stat_pkts_burst;

  int stat_trs_ret_code[MTL_PORT_MAX];
};

struct st_rx_ancillary_session_impl {
  int idx; /* index for current session */
  struct st40_rx_ops ops;
  char ops_name[ST_MAX_NAME_LEN];
  struct st_rx_session_priv priv[MTL_SESSION_PORT_MAX];

  enum mtl_port port_maps[MTL_SESSION_PORT_MAX];
  struct mt_rx_queue* queue[MTL_SESSION_PORT_MAX];
  struct mt_rss_entry* rss[MTL_SESSION_PORT_MAX];
  uint16_t port_id[MTL_SESSION_PORT_MAX];
  struct rte_ring* packet_ring;

  uint16_t st40_src_port[MTL_SESSION_PORT_MAX]; /* udp port */
  uint16_t st40_dst_port[MTL_SESSION_PORT_MAX]; /* udp port */

  int st40_seq_id; /* seq id for each pkt */

  uint32_t tmstamp;
  /* status */
  rte_atomic32_t st40_stat_frames_received;
  int st40_stat_pkts_dropped;
  int st40_stat_pkts_wrong_hdr_dropped;
  int st40_stat_pkts_received;
  uint64_t st40_stat_last_time;
};

struct st_rx_ancillary_sessions_mgr {
  struct mtl_main_impl* parnet;
  int idx;     /* index for current session mgr */
  int max_idx; /* max session index */
  struct mt_sch_tasklet_impl* tasklet;

  struct st_rx_ancillary_session_impl* sessions[ST_MAX_RX_ANC_SESSIONS];
  /* protect session, spin(fast) lock as it call from tasklet aslo */
  rte_spinlock_t mutex[ST_MAX_RX_ANC_SESSIONS];
};

struct st_ancillary_transmitter_impl {
  struct mtl_main_impl* parnet;
  struct st_tx_ancillary_sessions_mgr* mgr;
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
  int (*put_frame)(void* priv, struct st22_encode_frame_meta* frame, int result);
  int (*dump)(void* priv);
};

struct st22_get_decoder_request {
  enum st_plugin_device device;
  struct st22_decoder_create_req req;

  void* priv;
  struct st22_decode_frame_meta* (*get_frame)(void* priv);
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
  void* parnet; /* point to struct st22_encode_dev_impl */
  st22_encode_priv session;
  enum mt_handle_type type; /* for sanity check */

  size_t codestream_max_size;

  struct st22_get_encoder_request req;
};

struct st22_encode_dev_impl {
  enum mt_handle_type type; /* for sanity check */
  struct mtl_main_impl* parnet;
  int idx;
  char name[ST_MAX_NAME_LEN];
  struct st22_encoder_dev dev;
  rte_atomic32_t ref_cnt;
  struct st22_encode_session_impl sessions[ST_MAX_SESSIIONS_PER_ENCODER];
};

struct st22_decode_session_impl {
  int idx;
  void* parnet; /* point to struct st22_decode_dev_impl */
  st22_decode_priv session;
  enum mt_handle_type type; /* for sanity check */

  struct st22_get_decoder_request req;
};

struct st22_decode_dev_impl {
  enum mt_handle_type type; /* for sanity check */
  struct mtl_main_impl* parnet;
  int idx;
  char name[ST_MAX_NAME_LEN];
  struct st22_decoder_dev dev;
  rte_atomic32_t ref_cnt;
  struct st22_decode_session_impl sessions[ST_MAX_SESSIIONS_PER_DECODER];
};

struct st20_convert_session_impl {
  int idx;
  void* parnet; /* point to struct st20_convert_dev_impl */
  st20_convert_priv session;
  enum mt_handle_type type; /* for sanity check */

  struct st20_get_converter_request req;
};

struct st20_convert_dev_impl {
  enum mt_handle_type type; /* for sanity check */
  struct mtl_main_impl* parnet;
  int idx;
  char name[ST_MAX_NAME_LEN];
  struct st20_converter_dev dev;
  rte_atomic32_t ref_cnt;
  struct st20_convert_session_impl sessions[ST_MAX_SESSIIONS_PER_CONVERTER];
};

struct st_dl_plugin_impl {
  int idx;
  char path[ST_PLUNGIN_MAX_PATH_LEN];
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
  struct mtl_main_impl* parnet;
  enum mt_handle_type type;
  struct mt_sch_impl* sch; /* the sch this session attached */
  int quota_mbs;           /* data quota for this session */
  struct st_tx_video_session_impl* impl;
};

struct st22_tx_video_session_handle_impl {
  struct mtl_main_impl* parnet;
  enum mt_handle_type type;
  struct mt_sch_impl* sch; /* the sch this session attached */
  int quota_mbs;           /* data quota for this session */
  struct st_tx_video_session_impl* impl;
};

struct st_tx_audio_session_handle_impl {
  struct mtl_main_impl* parnet;
  enum mt_handle_type type;
  struct st_tx_audio_session_impl* impl;
};

struct st_tx_ancillary_session_handle_impl {
  struct mtl_main_impl* parnet;
  enum mt_handle_type type;
  struct st_tx_ancillary_session_impl* impl;
};

struct st_rx_video_session_handle_impl {
  struct mtl_main_impl* parnet;
  enum mt_handle_type type;
  struct mt_sch_impl* sch; /* the sch this session attached */
  int quota_mbs;           /* data quota for this session */
  struct st_rx_video_session_impl* impl;
};

struct st22_rx_video_session_handle_impl {
  struct mtl_main_impl* parnet;
  enum mt_handle_type type;
  struct mt_sch_impl* sch; /* the sch this session attached */
  int quota_mbs;           /* data quota for this session */
  struct st_rx_video_session_impl* impl;
};

struct st_rx_audio_session_handle_impl {
  struct mtl_main_impl* parnet;
  enum mt_handle_type type;
  struct st_rx_audio_session_impl* impl;
};

struct st_rx_ancillary_session_handle_impl {
  struct mtl_main_impl* parnet;
  enum mt_handle_type type;
  struct st_rx_ancillary_session_impl* impl;
};

static inline bool st20_is_frame_type(enum st20_type type) {
  if ((type == ST20_TYPE_FRAME_LEVEL) || (type == ST20_TYPE_SLICE_LEVEL))
    return true;
  else
    return false;
}

#endif
