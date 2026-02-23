/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#define _GNU_SOURCE
#include <json-c/json.h>
#include <math.h>
#include <rte_alarm.h>
#include <rte_arp.h>
#include <rte_cpuflags.h>
#include <rte_errno.h>
#include <rte_ethdev.h>
#include <rte_thash.h>
#include <rte_tm.h>
#include <rte_version.h>
#include <rte_vfio.h>
#include <sys/file.h>
#include <sys/queue.h>
#include <unistd.h>

#include "mt_mem.h"
#include "mt_platform.h"
#include "mt_quirk.h"
#include "mtl_sch_api.h"
#include "st2110/st_header.h"

#ifndef _MT_LIB_MAIN_HEAD_H_
#define _MT_LIB_MAIN_HEAD_H_

/* Macros compatible with system's sys/queue.h */
#define MT_TAILQ_HEAD(name, type) RTE_TAILQ_HEAD(name, type)
#define MT_TAILQ_ENTRY(type) RTE_TAILQ_ENTRY(type)
#define MT_TAILQ_FOREACH(var, head, field) RTE_TAILQ_FOREACH(var, head, field)
#define MT_TAILQ_FIRST(head) RTE_TAILQ_FIRST(head)
#define MT_TAILQ_NEXT(elem, field) RTE_TAILQ_NEXT(elem, field)

#define MT_TAILQ_INSERT_TAIL(head, elem, filed) TAILQ_INSERT_TAIL(head, elem, filed)
#define MT_TAILQ_INSERT_HEAD(head, elem, filed) TAILQ_INSERT_HEAD(head, elem, filed)
#define MT_TAILQ_REMOVE(head, elem, filed) TAILQ_REMOVE(head, elem, filed)
#define MT_TAILQ_INIT(head) TAILQ_INIT(head)

#define MT_STAILQ_HEAD(name, type) RTE_STAILQ_HEAD(name, type)
#define MT_STAILQ_ENTRY(type) RTE_STAILQ_ENTRY(type)

#define MT_MBUF_CACHE_SIZE (128)
#define MT_MBUF_HEADROOM_SIZE (RTE_PKTMBUF_HEADROOM)          /* 128 */
#define MT_MBUF_DEFAULT_DATA_SIZE (RTE_MBUF_DEFAULT_DATAROOM) /* 2048 */

#define MT_MAX_SCH_NUM (18) /* max 18 scheduler lcore */

/* max RL items */
#define MT_MAX_RL_ITEMS (128)

#define MT_ARP_ENTRY_MAX (60)

#define MT_MCAST_GROUP_MAX (60)

#define MT_DMA_MAX_SESSIONS (16)
/* if use rte ring for dma enqueue/dequeue */
#define MT_DMA_RTE_RING (1)

#define MT_MAP_MAX_ITEMS (256)

#define MT_IP_DONT_FRAGMENT_FLAG (0x0040)

/* Port supports Rx queue setup after device started. */
#define MT_IF_FEATURE_RUNTIME_RX_QUEUE (MTL_BIT32(0))
/* Timesync enabled on the port */
#define MT_IF_FEATURE_TIMESYNC (MTL_BIT32(1))
/* Port registers Rx timestamp in mbuf dynamic field */
#define MT_IF_FEATURE_RX_OFFLOAD_TIMESTAMP (MTL_BIT32(2))
/* Multi segment tx, chain buffer */
#define MT_IF_FEATURE_TX_MULTI_SEGS (MTL_BIT32(4))
/* tx ip hdr checksum offload */
#define MT_IF_FEATURE_TX_OFFLOAD_IPV4_CKSUM (MTL_BIT32(5))
/* Rx queue support hdr split */
#define MT_IF_FEATURE_RXQ_OFFLOAD_BUFFER_SPLIT (MTL_BIT32(6))
/* LaunchTime Tx */
#define MT_IF_FEATURE_TX_OFFLOAD_SEND_ON_TIMESTAMP (MTL_BIT32(7))

#define MT_IF_STAT_PORT_CONFIGURED (MTL_BIT32(0))
#define MT_IF_STAT_PORT_STARTED (MTL_BIT32(1))
#define MT_IF_STAT_PORT_DOWN (MTL_BIT32(2))

#define MT_DPDK_AF_XDP_START_QUEUE (1)

#define NS_PER_MS (1000 * 1000)
#define NS_PER_US (1000)
#define US_PER_MS (1000)

#define MT_TIMEOUT_INFINITE (INT_MAX)
#define MT_TIMEOUT_ZERO (0)

#define MT_SAFE_FREE(obj, free_fn) \
  do {                             \
    if (obj) {                     \
      free_fn(obj);                \
      obj = NULL;                  \
    }                              \
  } while (0)

struct mtl_main_impl; /* forward declare */

/* dynamic fields are implemented after rte_mbuf */
struct mt_muf_priv_data {
  union {
    struct st_tx_muf_priv_data tx_priv;
    struct st_rx_muf_priv_data rx_priv;
  };
};

struct mt_ptp_clock_id {
  uint8_t id[8];
};

struct mt_ptp_port_id {
  struct mt_ptp_clock_id clock_identity;
  uint16_t port_number;
} __attribute__((packed));

struct mt_ipv4_udp {
  struct rte_ipv4_hdr ip;
  struct rte_udp_hdr udp;
} __attribute__((__packed__)) __attribute__((__aligned__(2)));

enum mt_port_type {
  MT_PORT_ERR = 0,
  MT_PORT_VF,
  MT_PORT_PF,
  MT_PORT_DPDK_AF_XDP,
  MT_PORT_DPDK_AF_PKT,
  MT_PORT_KERNEL_SOCKET,
  MT_PORT_NATIVE_AF_XDP
};

enum mt_rl_type {
  MT_RL_TYPE_NONE = 0,
  /* RL based on RTE Generic Traffic Manager */
  MT_RL_TYPE_TM,
  /* XDP based on queue sysfs */
  MT_RL_TYPE_XDP_QUEUE_SYSFS,
};

enum mt_driver_type {
  MT_DRV_DEFAULT = 0,
  MT_DRV_ICE,   /* ice pf, net_ice */
  MT_DRV_IXGBE, /* ixgbe pf, net_ixgbe */
  MT_DRV_I40E,  /* flv pf, net_i40e */
  MT_DRV_IAVF,  /* IA vf, net_iavf */
  /* dpdk af xdp, net_af_xdp */
  MT_DRV_DPDK_AF_XDP,
  MT_DRV_E1000_IGB, /* e1000 igb, net_e1000_igb */
  MT_DRV_IGC,       /* igc, net_igc */
  MT_DRV_ENA,       /* aws ena, net_ena */
  MT_DRV_MLX5,      /* mlx, mlx5_pci */
  /* dpdk af packet, net_af_packet */
  MT_DRV_DPDK_AF_PKT,
  /* kernel based socket */
  MT_DRV_KERNEL_SOCKET,
  /* native af xdp */
  MT_DRV_NATIVE_AF_XDP
};

enum mt_flow_type {
  MT_FLOW_ALL,   /* full feature on rte flow */
  MT_FLOW_NO_IP, /* no ip on rte flow, port is supported */
  MT_FLOW_NONE,  /* no rte flow */
};

enum mt_ptp_l_mode {
  MT_PTP_L2 = 0,
  MT_PTP_L4,
  MT_PTP_MAX_MODE,
};

enum mt_ptp_addr_mode {
  MT_PTP_MULTICAST_ADDR = 0,
  MT_PTP_UNICAST_ADDR,
};

struct mt_pi_servo {
  double offset[2];
  double local[2];
  double drift;
  int count;
};

struct mt_phc2sys_impl {
  struct mt_pi_servo servo; /* PI for phc2sys */
  long realtime_hz;
  long realtime_nominal_tick;
  int64_t stat_delta_max;
  bool locked;
  uint16_t stat_sync_keep;
};

struct mt_ptp_impl {
  struct mtl_main_impl* impl;
  enum mtl_port port;
  uint16_t port_id;
  bool active; /* if the ptp stack is running */
  bool no_timesync;
  /*
   * The flag indicates Qbv (IEEE 802.1Qbv) traffic shaper
   * enable.
   *
   * The IEEE 802.1Qbv is designed to separate traffics
   * transmission into different time slices to prevent
   * traffics transmission interfering.
   */
  bool qbv_enabled;
  int64_t no_timesync_delta;

  /* for no cni case */
  struct mt_rxq_entry* gen_rxq;   /* for MT_PTP_UDP_GEN_PORT */
  struct mt_rxq_entry* event_rxq; /* for MT_PTP_UDP_EVENT_PORT */
  struct mt_sch_tasklet_impl* rxq_tasklet;

  struct mt_phc2sys_impl phc2sys;
  bool phc2sys_active;
  struct mt_pi_servo servo; /* PI for PTP */

  struct rte_mempool* mbuf_pool;

  uint8_t mcast_group_addr[MTL_IP_ADDR_LEN]; /* 224.0.1.129 */
  bool master_initialized;
  struct mt_ptp_port_id master_port_id;
  struct rte_ether_addr master_addr;
  struct mt_ptp_port_id our_port_id;
  struct mt_ipv4_udp dst_udp;        /* for l4 */
  uint8_t sip_addr[MTL_IP_ADDR_LEN]; /* source IP */
  enum mt_ptp_addr_mode master_addr_mode;
  int16_t master_utc_offset; /* offset to UTC of current master PTP */
  int64_t ptp_delta;         /* current delta for PTP */

  uint64_t t1;
  uint8_t t1_domain_number;
  uint64_t t2;
  bool t2_vlan;
  uint16_t t2_sequence_id;
  enum mt_ptp_l_mode t2_mode;
  uint64_t t3;
  uint16_t t3_sequence_id;
  uint64_t t4;

  bool calibrate_t2_t3; /* for no_timesync case which the t2 and t3 get from tsc */

  bool locked;
  bool connected;

  /* result */
  uint64_t delta_result_cnt;
  uint64_t delta_result_sum;
  uint64_t delta_result_err;
  /* expect result */
  int32_t expect_result_cnt;
  int32_t expect_result_sum;
  int32_t expect_result_avg;
  int32_t expect_correct_result_sum;
  int32_t expect_correct_result_avg;
  int32_t expect_t2_t1_delta_sum;
  int32_t expect_t2_t1_delta_avg;
  int t2_t1_delta_continuous_err;
  int32_t expect_t4_t3_delta_sum;
  int32_t expect_t4_t3_delta_avg;
  int t4_t3_delta_continuous_err;
  uint64_t expect_result_start_ns;
  uint64_t expect_result_period_ns;

  /* calculate sw frequency */
  uint64_t last_sync_ts;
  double coefficient;
  double coefficient_result_sum;
  double coefficient_result_min;
  double coefficient_result_max;
  int32_t coefficient_result_cnt;

  /* pi controller */
  bool use_pi;        /* use pi controller */
  double kp;          /* proportional gain */
  double ki;          /* integral gain */
  double integral;    /* integral value */
  int64_t prev_error; /* previous error (correct_delta) */

  /* status */
  int64_t stat_delta_min;
  int64_t stat_delta_max;
  int32_t stat_delta_cnt;
  int64_t stat_delta_sum;
  int64_t stat_correct_delta_min;
  int64_t stat_correct_delta_max;
  int32_t stat_correct_delta_cnt;
  int64_t stat_correct_delta_sum;
  int64_t stat_path_delay_min;
  int64_t stat_path_delay_max;
  int32_t stat_path_delay_cnt;
  int64_t stat_path_delay_sum;
  int32_t stat_rx_sync_err;
  int32_t stat_tx_sync_err;
  int32_t stat_result_err;
  int32_t stat_sync_timeout_err;
  int32_t stat_t3_sequence_id_mismatch;
  int32_t stat_sync_cnt;
  int32_t stat_t2_t1_delta_calibrate;
  int32_t stat_t4_t3_delta_calibrate;
  uint16_t stat_sync_keep;
};

/* used for cni sys queue */
#define MT_RXQ_FLOW_F_SYS_QUEUE (MTL_BIT32(0))
/* no ip flow, only use port flow, for udp transport */
#define MT_RXQ_FLOW_F_NO_IP (MTL_BIT32(1))
/* if apply destination port flow or not */
#define MT_RXQ_FLOW_F_NO_PORT (MTL_BIT32(2))
/* child of cni to save queue usage */
#define MT_RXQ_FLOW_F_FORCE_CNI (MTL_BIT32(3))
/* if request hdr split */
#define MT_RXQ_FLOW_F_HDR_SPLIT (MTL_BIT32(4))
/* force to use socket, only for MT_DRV_F_KERNEL_BASED */
#define MT_RXQ_FLOW_F_FORCE_SOCKET (MTL_BIT32(5))

/* request of rx queue flow */
struct mt_rxq_flow {
  /* mandatory if not no_ip_flow */
  uint8_t dip_addr[MTL_IP_ADDR_LEN]; /* rx destination IP */
  /* source ip is ignored if destination is a multicast address */
  uint8_t sip_addr[MTL_IP_ADDR_LEN]; /* source IP */
  uint16_t dst_port;                 /* udp destination port */
  /* value of MT_RXQ_FLOW_F_* */
  uint32_t flags;
  /* rate in bytes */
  uint64_t bytes_per_sec;

  /* optional for hdr split */
  void* hdr_split_mbuf_cb_priv;
#ifdef ST_HAS_DPDK_HDR_SPLIT /* rte_eth_hdrs_mbuf_callback_fn define with this marco */
  rte_eth_hdrs_mbuf_callback_fn hdr_split_mbuf_cb;
#endif
};

struct mt_cni_udp_detect_entry {
  uint32_t tuple[3]; /* udp tuple identify */
  int pkt_cnt;
  /* linked list */
  MT_TAILQ_ENTRY(mt_cni_udp_detect_entry) next;
};

MT_TAILQ_HEAD(mt_cni_udp_detect_list, mt_cni_udp_detect_entry);

struct mt_csq_entry {
  int idx;
  struct mt_cni_entry* parent;
  struct mt_rxq_flow flow;
  struct rte_ring* ring;
  uint32_t stat_enqueue_cnt;
  uint32_t stat_dequeue_cnt;
  uint32_t stat_enqueue_fail_cnt;
  /* linked list */
  MT_TAILQ_ENTRY(mt_csq_entry) next;
};

MT_TAILQ_HEAD(mt_csq_queue, mt_csq_entry);

struct mt_cni_entry {
  struct mtl_main_impl* impl;
  enum mtl_port port;
  struct mt_rxq_entry* rxq;

  struct mt_csq_queue csq_queues; /* for cni udp queue */
  int csq_idx;
  rte_spinlock_t csq_lock; /* protect csq_queues */

  struct mt_cni_udp_detect_list udp_detect; /* for udp stream debug usage */
  struct mt_rx_pcap pcap;

  /* stat */
  uint32_t eth_rx_cnt;
  uint64_t eth_rx_bytes;
  uint32_t virtio_rx_cnt;      /* rx pkts to kernel */
  uint32_t virtio_rx_fail_cnt; /* rx failed kernel pkts */
  uint32_t virtio_tx_cnt;      /* tx pkts from kernal */
  uint32_t virtio_tx_fail_cnt; /* tx failed kernal pkts */
};

struct mt_cni_impl {
  struct mtl_main_impl* parent;

  pthread_t tid; /* thread id for rx */
  rte_atomic32_t stop_thread;
  bool lcore_tasklet;
  struct mt_sch_tasklet_impl* tasklet;
  int thread_sleep_ms;

  struct mt_cni_entry entries[MTL_PORT_MAX];

#ifdef MTL_HAS_TAP
  pthread_t tap_bkg_tid; /* bkg thread id for tap */
  rte_atomic32_t stop_tap;
  struct mt_txq_entry* tap_tx_q[MTL_PORT_MAX]; /* tap tx queue */
  struct mt_rxq_entry* tap_rx_q[MTL_PORT_MAX]; /* tap rx queue */
  int tap_rx_cnt[MTL_PORT_MAX];
  rte_atomic32_t tap_if_up[MTL_PORT_MAX];
  void* tap_context;
#endif
};

struct mt_arp_entry {
  uint32_t ip;
  struct rte_ether_addr ea;
  rte_atomic32_t mac_ready;
};

struct mt_arp_impl {
  pthread_mutex_t mutex; /* arp impl protect */
  struct mt_arp_entry entries[MT_ARP_ENTRY_MAX];
  bool timer_active;
  enum mtl_port port;
  struct mtl_main_impl* parent;
};

struct mt_mcast_src_entry {
  uint32_t src_ip;
  uint16_t src_ref_cnt;
  TAILQ_ENTRY(mt_mcast_src_entry) entries;
};

TAILQ_HEAD(mt_mcast_src_list, mt_mcast_src_entry);

struct mt_mcast_group_entry {
  uint32_t group_ip;
  uint16_t group_ref_cnt;
  struct mt_mcast_src_list src_list;
  uint16_t src_num;
  TAILQ_ENTRY(mt_mcast_group_entry) entries;
};

TAILQ_HEAD(mt_mcast_group_list, mt_mcast_group_entry);

struct mt_mcast_impl {
  pthread_mutex_t group_mutex;
  struct mt_mcast_group_list group_list;
  uint16_t group_num;
  bool has_external_query;
};

enum mt_dhcp_status {
  MT_DHCP_STATUS_INIT = 0,
  MT_DHCP_STATUS_DISCOVERING, /* no selecting as we always choose the first offer */
  MT_DHCP_STATUS_REQUESTING,
  MT_DHCP_STATUS_BOUND,
  MT_DHCP_STATUS_RENEWING,
  MT_DHCP_STATUS_REBINDING,
  MT_DHCP_STATUS_MAX,
};

struct mt_dhcp_impl {
  pthread_mutex_t mutex; /* dhcp impl protect */
  enum mt_dhcp_status status;
  uint32_t xid;
  uint8_t server_ip[MTL_IP_ADDR_LEN];
  enum mtl_port port;
  struct mtl_main_impl* parent;

  /* cached configuration */
  uint8_t ip[MTL_IP_ADDR_LEN];
  uint8_t netmask[MTL_IP_ADDR_LEN];
  uint8_t gateway[MTL_IP_ADDR_LEN];
  uint8_t dns[MTL_IP_ADDR_LEN];
};

struct mt_sch_tasklet_impl {
  struct mtl_tasklet_ops ops;
  char name[ST_MAX_NAME_LEN];
  struct mtl_sch_impl* sch;

  int idx;
  bool request_exit;
  bool ack_exit;

  /* for time measure */
  struct mt_stat_u64 stat_time;
};

enum mt_sch_type {
  MT_SCH_TYPE_DEFAULT = 0,
  MT_SCH_TYPE_RX_VIDEO_ONLY,
  MT_SCH_TYPE_APP, /* created by user */
  /* dedicated for system task */
  MT_SCH_TYPE_SYSTEM,
  MT_SCH_TYPE_MAX,
};

typedef uint64_t mt_sch_mask_t;

/* all sch */
#define MT_SCH_MASK_ALL ((mt_sch_mask_t)-1)

struct mtl_sch_impl {
  char name[32];
  pthread_mutex_t mutex; /* protect sch context */
  struct mt_sch_tasklet_impl** tasklet;
  uint32_t nb_tasklets; /* the number of tasklet in current sch */
  /* max tasklet index */
  volatile int max_tasklet_idx;
  unsigned int lcore;
  /* the socket id this sch attached to */
  int socket_id;
  bool run_in_thread; /* Run the tasklet inside one thread instead of a pinned lcore. */
  pthread_t tid;      /* thread id for run_in_thread */
  int t_pid;          /* gettid */

  int data_quota_mbs_total; /* total data quota(mb/s) for current sch */
  int data_quota_mbs_limit; /* limit data quota(mb/s) for current sch */
  bool cpu_busy;

  struct mtl_main_impl* parent;
  int idx; /* index for current sch */
  rte_atomic32_t started;
  rte_atomic32_t request_stop;
  rte_atomic32_t stopped;
  rte_atomic32_t active; /* if this sch is active */
  rte_atomic32_t ref_cnt;
  enum mt_sch_type type;

  /* one tx video sessions mgr/transmitter for one sch */
  struct st_video_transmitter_impl video_transmitter;
  struct st_tx_video_sessions_mgr tx_video_mgr;
  bool tx_video_init;
  pthread_mutex_t tx_video_mgr_mutex; /* protect tx_video_mgr */

  /* one rx video sessions mgr for one sch */
  struct st_rx_video_sessions_mgr rx_video_mgr;
  bool rx_video_init;
  pthread_mutex_t rx_video_mgr_mutex; /* protect tx_video_mgr */

  /* one tx audio sessions mgr/transmitter for one sch */
  struct st_tx_audio_sessions_mgr tx_a_mgr;
  struct st_audio_transmitter_impl a_trs;
  bool tx_a_init;
  pthread_mutex_t tx_a_mgr_mutex; /* protect tx_a_mgr */

  /* one rx audio sessions mgr for one sch */
  struct st_rx_audio_sessions_mgr rx_a_mgr;
  bool rx_a_init;
  pthread_mutex_t rx_a_mgr_mutex; /* protect rx_a_mgr */

  /* one tx ancillary sessions mgr/transmitter for one sch */
  struct st_tx_ancillary_sessions_mgr tx_anc_mgr;
  struct st_ancillary_transmitter_impl anc_trs;
  bool tx_anc_init;
  pthread_mutex_t tx_anc_mgr_mutex; /* protect tx_anc_mgr */

  /* one rx ancillary sessions mgr for one sch */
  struct st_rx_ancillary_sessions_mgr rx_anc_mgr;
  bool rx_anc_init;
  pthread_mutex_t rx_anc_mgr_mutex; /* protect rx_anc_mgr */

  /* one tx fast metadata sessions mgr/transmitter for one sch */
  struct st_tx_fastmetadata_sessions_mgr tx_fmd_mgr;
  struct st_fastmetadata_transmitter_impl fmd_trs;
  bool tx_fmd_init;
  pthread_mutex_t tx_fmd_mgr_mutex; /* protect tx_fmd_mgr */

  /* one rx fast metadata sessions mgr for one sch */
  struct st_rx_fastmetadata_sessions_mgr rx_fmd_mgr;
  bool rx_fmd_init;
  pthread_mutex_t rx_fmd_mgr_mutex; /* protect rx_fmd_mgr */

  /* sch sleep info */
  bool allow_sleep;
  pthread_cond_t sleep_wake_cond;
  pthread_mutex_t sleep_wake_mutex;

  uint64_t avg_ns_per_loop;

  /* the sch sleep ratio */
  float sleep_ratio_score;
  uint64_t sleep_ratio_start_ns;
  uint64_t sleep_ratio_sleep_ns;

  uint64_t stat_sleep_ns;
  uint32_t stat_sleep_cnt;
  uint64_t stat_sleep_ns_min;
  uint64_t stat_sleep_ns_max;
  /* for time measure */
  struct mt_stat_u64 stat_time;
};

struct mt_lcore_mgr {
  struct mt_lcore_shm* lcore_shm;
  int lcore_shm_id;
};

/* remember to update lcore_type_names if any item changed */
enum mt_lcore_type {
  MT_LCORE_TYPE_SCH = 0, /* lib scheduler used */
  MT_LCORE_TYPE_TAP,
  MT_LCORE_TYPE_RXV_RING_LCORE,
  MT_LCORE_TYPE_USER,     /* allocated by application */
  MT_LCORE_TYPE_SCH_USER, /* application allocated by mtl_sch_create  */
  MT_LCORE_TYPE_MAX,
};

struct mt_sch_mgr {
  struct mtl_sch_impl sch[MT_MAX_SCH_NUM];
  /* active sch cnt */
  rte_atomic32_t sch_cnt;
  pthread_mutex_t mgr_mutex; /* protect sch mgr */

  struct mt_lcore_mgr lcore_mgr;
  int lcore_lock_fd;

  /* local lcores info */
  bool local_lcores_active[RTE_MAX_LCORE];
  enum mt_lcore_type local_lcores_type[RTE_MAX_LCORE];
};

struct mt_pacing_train_result {
  uint64_t input_bps;        /* input, byte per sec */
  uint64_t profiled_bps;     /* profiled result */
  float pacing_pad_interval; /* result */
};

struct mt_rl_shaper {
  uint64_t rl_bps; /* input, byte per sec */
  uint32_t shaper_profile_id;
  int idx;
};

struct mt_rx_flow_rsp {
  int flow_id; /* flow id for socket based flow */
  struct rte_flow* flow;
  uint16_t queue_id;
  uint16_t dst_port;
};

struct mt_rx_queue {
  enum mtl_port port;
  uint16_t port_id;
  uint16_t queue_id;
  bool active;
  struct mt_rxq_flow flow;
  struct mt_rx_flow_rsp* flow_rsp;
  struct rte_mempool* mbuf_pool;
  unsigned int mbuf_elements;
  /* pool for hdr split payload */
  struct rte_mempool* mbuf_payload_pool;
};

struct mt_tx_queue {
  enum mtl_port port;
  uint16_t port_id;
  uint16_t queue_id;
  bool active;
  /* VF, caused by malicious detection in the PF */
  bool fatal_error;
  int rl_shapers_mapping; /* map to tx_rl_shapers */
  uint64_t bps;           /* bytes per sec for rate limit */
};

/* use rte_eth_dev_set_mc_addr_list instead of rte_eth_dev_mac_addr_add for multicast */
#define MT_DRV_F_USE_MC_ADDR_LIST (MTL_BIT64(0))
/* no rte_eth_stats_reset support */
#define MT_DRV_F_NO_STATUS_RESET (MTL_BIT64(1))
/* no CNI support for RX */
#define MT_DRV_F_NO_CNI (MTL_BIT64(2))
/* the driver is not dpdk based */
#define MT_DRV_F_NOT_DPDK_PMD (MTL_BIT64(3))
/* use kernel socket control path for arp/mcast */
#define MT_DRV_F_USE_KERNEL_CTL (MTL_BIT64(4))
/* no priv for the mbuf in the rx queue */
#define MT_DRV_F_RX_POOL_COMMON (MTL_BIT64(5))
/* no rx flow, for MTL_PMD_DPDK_AF_PACKET and MTL_PMD_KERNEL_SOCKET */
#define MT_DRV_F_RX_NO_FLOW (MTL_BIT64(6))
/* mcast control in data path, for MTL_PMD_KERNEL_SOCKET */
#define MT_DRV_F_MCAST_IN_DP (MTL_BIT64(7))
/* no sys tx queue support */
#define MT_DRV_F_NO_SYS_TX_QUEUE (MTL_BIT64(8))
/* kernel based backend */
#define MT_DRV_F_KERNEL_BASED (MTL_BIT64(9))

struct mt_dev_driver_info {
  char* name;
  enum mt_port_type port_type;
  enum mt_driver_type drv_type;

  enum mt_flow_type flow_type;
  enum mt_rl_type rl_type;
  uint64_t flags; /* value with MT_DRV_F_* */
};

struct mt_interface {
  struct mtl_main_impl* parent;
  enum mtl_port port;
  uint16_t port_id;
  struct rte_eth_dev_info dev_info;
  struct mt_dev_driver_info drv_info;
  enum mtl_rss_mode rss_mode;
  enum mtl_net_proto net_proto;
  int socket_id;                          /* socket id for the port */
  uint32_t feature;                       /* MT_IF_FEATURE_* */
  uint32_t link_speed;                    /* ETH_SPEED_NUM_ */
  struct rte_ether_addr* mcast_mac_lists; /* pool of multicast mac addrs */
  uint32_t mcast_nb;                      /* number of address */
  uint32_t status;                        /* MT_IF_STAT_* */
  /* The port is temporarily off, e.g. during rte_tm_hierarchy_commit */
  rte_atomic32_t resetting;

  /* default tx mbuf_pool */
  struct rte_mempool* tx_mbuf_pool;
  /* default rx mbuf_pool */
  struct rte_mempool* rx_mbuf_pool;
  uint16_t nb_tx_desc;
  uint16_t nb_rx_desc;

  struct rte_mbuf* pad;
  /*
   * protect rl and fdir for vf.
   * _atomic_set_cmd(): There is incomplete cmd 112
   */
  pthread_mutex_t vf_cmd_mutex;

  /* tx queue resources */
  uint16_t nb_tx_q;
  struct mt_tx_queue* tx_queues;
  pthread_mutex_t tx_queues_mutex; /* protect tx_queues */

  /* rx queue resources */
  uint16_t nb_rx_q;
  uint16_t system_rx_queues_end;
  uint16_t hdr_split_rx_queues_end;
  struct mt_rx_queue* rx_queues;
  pthread_mutex_t rx_queues_mutex; /* protect rx_queues */

  /* tx rl info */
  struct mt_rl_shaper tx_rl_shapers[MT_MAX_RL_ITEMS];
  bool tx_rl_root_active;
  /* video rl pacing train result */
  struct mt_pacing_train_result pt_results[MT_MAX_RL_ITEMS];

  /* function ops per interface(pf/vf) */
  uint64_t (*ptp_get_time_fn)(struct mtl_main_impl* impl, enum mtl_port port);

  enum st21_tx_pacing_way tx_pacing_way;

  /* LaunchTime register */
  int tx_dynfield_offset;
  /* tx launch time enable flag */
  uint64_t tx_launch_time_flag;

  /* time base for MTL_FLAG_PTP_SOURCE_TSC*/
  uint64_t tsc_time_base;
  uint64_t real_time_base;

  rte_spinlock_t stats_lock;
  struct mtl_port_status* dev_stats_not_reset; /* for nic without reset func */
  struct mtl_port_status* dev_stats_sw;        /* for MT_DRV_F_NOT_DPDK_PMD */
  struct mtl_port_status stats_sum;            /* for dev_inf_stat dump */
  struct mtl_port_status user_stats_port;      /* for mtl_get_port_stats */
  struct mtl_port_status stats_admin;          /* stats used in admin task */

  uint64_t simulate_malicious_pkt_tsc;

  uint16_t virtio_port_id; /* virtio_user port id */
  bool virtio_port_active; /* virtio_user port active */

  /* the mac for kernel socket based transport */
  struct rte_ether_addr k_mac_addr;

  void* xdp;
};

struct mt_user_info {
  char hostname[64];
  char user[32];
  /* the current process name */
  char comm[64];
  pid_t pid;
};

struct mt_lcore_shm_entry {
  struct mt_user_info u_info;
  pid_t pid;
  enum mt_lcore_type type;
  bool active;
};

struct mt_lcore_shm {
  /* number of used lcores */
  int used;
  /* lcores map info */
  struct mt_lcore_shm_entry lcores_info[RTE_MAX_LCORE];
};

typedef int (*mt_dma_drop_mbuf_cb)(void* priv, struct rte_mbuf* mbuf);

struct mtl_dma_lender_dev {
  enum mt_handle_type type; /* for sanity check */

  struct mt_dma_dev* parent;
  int lender_id;
  bool active;

  void* priv;
  uint16_t nb_borrowed;
  mt_dma_drop_mbuf_cb cb;
};

struct mt_dma_dev {
  int16_t dev_id;
  uint16_t nb_desc;
  bool active;
  bool usable;
  int idx;
  int sch_idx;
  int soc_id;
  uint16_t nb_session; /* number of attached session(lender)s */
  uint16_t max_shared; /* max number of attached session(lender)s */
  /* shared lenders */
  struct mtl_dma_lender_dev lenders[MT_DMA_MAX_SESSIONS];
  uint16_t nb_inflight; /* not atomic since it's in single thread only */
#if MT_DMA_RTE_RING
  struct rte_ring* borrow_queue; /* borrowed mbufs from rx sessions */
#else
  uint16_t inflight_enqueue_idx;
  uint16_t inflight_dequeue_idx;
  struct rte_mbuf** inflight_mbufs;
#endif
  uint64_t stat_inflight_sum;
  uint64_t stat_commit_sum;
};

struct mt_dma_mgr {
  struct mt_dma_dev devs[MTL_DMA_DEV_MAX];
  pthread_mutex_t mutex; /* protect devs */
  uint8_t num_dma_dev;
  rte_atomic32_t num_dma_dev_active;
};

struct mtl_dma_mem {
  void* alloc_addr;  /* the address return from malloc */
  size_t alloc_size; /* the malloc size */
  void* addr;        /* the first page aligned address after alloc_addr */
  size_t valid_size; /* the valid data size from user */
  mtl_iova_t iova;   /* the dma mapped address of addr */
  size_t iova_size;  /* the iova mapped size */
};

struct mt_admin {
  uint64_t period_us;
  pthread_t admin_tid;
  pthread_cond_t admin_wake_cond;
  pthread_mutex_t admin_wake_mutex;
  rte_atomic32_t admin_stop;
};

struct mt_kport_info {
  /* dpdk port name for kernel port(MTL_PMD_DPDK_AF_XDP) */
  char dpdk_port[MTL_PORT_MAX][MTL_PORT_MAX_LEN];
  /* kernel interface name */
  char kernel_if[MTL_PORT_MAX][MTL_PORT_MAX_LEN];
};

struct mt_map_item {
  void* vaddr;
  size_t size;
  mtl_iova_t iova; /* iova address */
};

struct mt_map_mgr {
  pthread_mutex_t mutex;
  struct mt_map_item* items[MT_MAP_MAX_ITEMS];
};

struct mt_var_params {
  /* default sleep time(us) for sch tasklet sleep */
  uint64_t sch_default_sleep_us;
  /* force sleep time(us) for sch tasklet sleep */
  uint64_t sch_force_sleep_us;
  /* sleep(0) threshold */
  uint64_t sch_zero_sleep_threshold_us;
};

typedef int (*mt_stat_cb_t)(void* priv);
struct mt_stat_item {
  /* stat dump callback func */
  mt_stat_cb_t cb_func;
  /* stat dump callback private data */
  void* cb_priv;
  /* name */
  char name[ST_MAX_NAME_LEN];
  /* linked list */
  MT_TAILQ_ENTRY(mt_stat_item) next;
};
/* List of stat items */
MT_TAILQ_HEAD(mt_stat_items_list, mt_stat_item);

struct mt_stat_mgr {
  struct mtl_main_impl* parent;

  uint64_t dump_period_us;
  rte_spinlock_t lock;
  struct mt_stat_items_list head;

  pthread_t stat_tid;
  pthread_cond_t stat_wake_cond;
  pthread_mutex_t stat_wake_mutex;
  rte_atomic32_t stat_stop;
};

enum mt_queue_mode {
  MT_QUEUE_MODE_DPDK = 0,
  MT_QUEUE_MODE_XDP,
  MT_QUEUE_MODE_MAX,
};

struct mt_rsq_impl; /* forward delcare */

struct mt_rsq_entry {
  uint16_t queue_id;
  int idx;
  struct mt_rxq_flow flow;
  struct mt_rx_flow_rsp* flow_rsp;
  struct mt_rsq_impl* parent;
  struct rte_ring* ring;
  /* wa for MTL_PMD_DPDK_AF_PACKET */
  int mcast_fd;
  uint32_t stat_enqueue_cnt;
  uint32_t stat_dequeue_cnt;
  uint32_t stat_enqueue_fail_cnt;
  /* linked list */
  MT_TAILQ_ENTRY(mt_rsq_entry) next;
};
MT_TAILQ_HEAD(mt_rsq_entrys_list, mt_rsq_entry);

struct mt_rsq_queue {
  uint16_t port_id;
  uint16_t queue_id;
  /* for native xdp based shared queue */
  struct mt_rx_xdp_entry* xdp;
  /* List of rsq entry */
  struct mt_rsq_entrys_list head;
  rte_spinlock_t mutex;
  rte_atomic32_t entry_cnt;
  int entry_idx;
  struct mt_rsq_entry* cni_entry;
  /* stat */
  int stat_pkts_recv;
  int stat_pkts_deliver;
};

struct mt_rsq_impl {
  struct mtl_main_impl* parent;
  enum mtl_port port;
  /* sq rx queue resources */
  uint16_t nb_rsq_queues;
  struct mt_rsq_queue* rsq_queues;
  enum mt_queue_mode queue_mode;
};

/* used for sys queue */
#define MT_TXQ_FLOW_F_SYS_QUEUE (MTL_BIT32(0))
/* if launch time enabled */
#define MT_TXQ_FLOW_F_LAUNCH_TIME (MTL_BIT32(1))
/* force to use socket, only for MT_DRV_F_KERNEL_BASED */
#define MT_TXQ_FLOW_F_FORCE_SOCKET (MTL_BIT32(2))

/* request of tx queue flow */
struct mt_txq_flow {
  uint64_t bytes_per_sec; /* rl rate in byte */
  /* mandatory if not sys_queue */
  uint8_t dip_addr[MTL_IP_ADDR_LEN]; /* tx destination IP */
  uint16_t dst_port;                 /* udp destination port */
  /* value with MT_TXQ_FLOW_F_* */
  uint32_t flags;
  /* only for kernel socket */
  uint16_t gso_sz;
};

struct mt_tsq_impl; /* forward delcare */

struct mt_tsq_entry {
  uint16_t queue_id;
  struct mt_txq_flow flow;
  struct mt_tsq_impl* parent;
  struct rte_mempool* tx_pool;
  /* linked list */
  MT_TAILQ_ENTRY(mt_tsq_entry) next;
};
MT_TAILQ_HEAD(mt_tsq_entrys_list, mt_tsq_entry);

struct mt_tsq_queue {
  uint16_t port_id;
  uint16_t queue_id;
  /* shared tx mempool */
  struct rte_mempool* tx_pool;
  /* for native xdp based shared queue */
  struct mt_tx_xdp_entry* xdp;

  /* List of rsq entry */
  struct mt_tsq_entrys_list head;
  pthread_mutex_t mutex;
  rte_spinlock_t tx_mutex;
  rte_atomic32_t entry_cnt;
  bool fatal_error;
  /* stat */
  int stat_pkts_send;
};

struct mt_tsq_impl {
  struct mtl_main_impl* parent;
  enum mtl_port port;
  /* sq tx queue resources */
  uint16_t nb_tsq_queues;
  struct mt_tsq_queue* tsq_queues;
  enum mt_queue_mode queue_mode;
};

struct mt_srss_entry {
  struct mt_rxq_flow flow;
  struct mt_srss_impl* srss;
  int idx;
  struct rte_ring* ring;
  uint32_t stat_enqueue_cnt;
  uint32_t stat_dequeue_cnt;
  uint32_t stat_enqueue_fail_cnt;
  /* linked list */
  MT_TAILQ_ENTRY(mt_srss_entry) next;
};
MT_TAILQ_HEAD(mt_srss_entrys_list, mt_srss_entry);

struct mt_srss_list {
  struct mt_srss_entrys_list entrys_list;
  rte_spinlock_t mutex; /* protect entrys_list */
  int idx;
};

struct mt_srss_sch {
  struct mt_srss_impl* parent;
  int idx;
  uint16_t q_start;
  uint16_t q_end;
  struct mtl_sch_impl* sch;
  struct mt_sch_tasklet_impl* tasklet;
  int quota_mps;

  uint32_t stat_pkts_rx;
};

struct mt_srss_impl {
  struct mtl_main_impl* parent;

  enum mtl_port port;
  enum mt_queue_mode queue_mode;
  uint16_t nb_rx_q;

  /* map entry to different heads as the UDP port number */
  struct mt_srss_list* lists;
  int lists_sz;

  /* sch threads */
  struct mt_srss_sch* schs;
  int schs_cnt;

  pthread_t tid;
  rte_atomic32_t stop_thread;

  struct mt_srss_entry* cni_entry;
  int entry_idx;

  /* for native xdp based srss */
  struct mt_rx_xdp_entry** xdps;
};

#define MT_DP_SOCKET_THREADS_MAX (4)

struct mt_tx_socket_thread {
  struct mt_tx_socket_entry* parent;
  int idx;
  int fd;
  pthread_t tid;
  rte_atomic32_t stop_thread;

#ifndef WINDOWSENV
  struct sockaddr_in send_addr;
  struct msghdr msg;
  char msg_control[CMSG_SPACE(sizeof(uint16_t))];
#endif

  int stat_tx_try;
  int stat_tx_pkt;
  int stat_tx_gso;
};

struct mt_tx_socket_entry {
  struct mtl_main_impl* parent;
  enum mtl_port port;
  struct mt_txq_flow flow;

  uint64_t rate_limit_per_thread;
  uint16_t gso_sz;
  int threads;
  struct rte_ring* ring;
  struct mt_tx_socket_thread threads_data[MT_DP_SOCKET_THREADS_MAX];
  bool stat_registered;
};

struct mt_rx_socket_thread {
  struct mt_rx_socket_entry* parent;
  int idx;
  struct rte_mbuf* mbuf;
  pthread_t tid;
  rte_atomic32_t stop_thread;

  int stat_rx_try;
  int stat_rx_pkt;
};

struct mt_rx_socket_entry {
  struct mtl_main_impl* parent;
  enum mtl_port port;
  struct mt_rxq_flow flow;

  struct rte_mempool* pool;
  uint16_t pool_element_sz;
  int fd;

  uint64_t rate_limit_per_thread;
  int threads;
  struct rte_ring* ring;
  struct mt_rx_socket_thread threads_data[MT_DP_SOCKET_THREADS_MAX];
  bool stat_registered;
};

struct mt_tx_xdp_entry {
  struct mtl_main_impl* parent;
  enum mtl_port port;
  struct mt_txq_flow flow;
  uint16_t queue_id;
  struct mt_xdp_queue* xq;
};

struct mt_rx_xdp_entry {
  struct mtl_main_impl* parent;
  enum mtl_port port;
  struct mt_rxq_flow flow;
  uint16_t queue_id;
  struct mt_xdp_queue* xq;
  struct mt_rx_flow_rsp* flow_rsp;
  bool skip_udp_port_check;
  bool skip_all_check;
  int mcast_fd;
};

struct mt_flow_impl {
  pthread_mutex_t mutex; /* protect mt_rx_flow_create */
};

struct mt_dp_impl {
  /* the shared tx sys queue */
  struct mt_txq_entry* txq_sys_entry;
  rte_spinlock_t txq_sys_entry_lock; /* protect txq_sys_entry */
};

struct mtl_main_impl {
  struct mt_interface inf[MTL_PORT_MAX];

  struct mtl_init_params user_para;
  struct mt_var_params var_para;
  struct mt_kport_info kport_info;
  enum mt_handle_type type; /* for sanity check */
  uint64_t tsc_hz;
  pthread_t tsc_cal_tid;

  enum rte_iova_mode iova_mode; /* current IOVA mode */
  size_t page_size;

  /* flow */
  struct mt_flow_impl* flow[MTL_PORT_MAX];
  /* data path queue mgr */
  struct mt_dp_impl* dp[MTL_PORT_MAX];
  /* rss */
  struct mt_rss_impl* rss[MTL_PORT_MAX];
  struct mt_srss_impl* srss[MTL_PORT_MAX];
  /* shared rx queue mgr */
  struct mt_rsq_impl* rsq[MTL_PORT_MAX];
  struct mt_tsq_impl* tsq[MTL_PORT_MAX];

  /* stat */
  struct mt_stat_mgr stat_mgr;

  /* dev context */
  rte_atomic32_t instance_started;  /* if mt instance is started */
  rte_atomic32_t instance_in_reset; /* if mt instance is in reset */
  /* if mt instance is aborted, in case for ctrl-c from app */
  rte_atomic32_t instance_aborted;
  struct mtl_sch_impl* main_sch; /* system sch */

  /* admin context */
  struct mt_admin admin;

  /* cni context */
  struct mt_cni_impl cni;

  /* ptp context */
  struct mt_ptp_impl* ptp[MTL_PORT_MAX];
  uint64_t ptp_usync;
  uint64_t ptp_usync_tsc;
  /* arp context */
  struct mt_arp_impl* arp[MTL_PORT_MAX];
  /* mcast context */
  struct mt_mcast_impl* mcast[MTL_PORT_MAX];
  /* dhcp context */
  struct mt_dhcp_impl* dhcp[MTL_PORT_MAX];

  /* sch context */
  struct mt_sch_mgr sch_mgr;
  uint32_t sch_schedule_ns;
  uint32_t tasklets_nb_per_sch;
  uint32_t tx_audio_sessions_max_per_sch;
  uint32_t rx_audio_sessions_max_per_sch;

  /* st plugin dev mgr */
  struct st_plugin_mgr plugin_mgr;

  struct mt_user_info u_info;

  void* mudp_rxq_mgr[MTL_PORT_MAX];

  /* cnt for open sessions */
  rte_atomic32_t st20_tx_sessions_cnt;
  rte_atomic32_t st22_tx_sessions_cnt;
  rte_atomic32_t st30_tx_sessions_cnt;
  rte_atomic32_t st40_tx_sessions_cnt;
  rte_atomic32_t st41_tx_sessions_cnt;
  rte_atomic32_t st20_rx_sessions_cnt;
  rte_atomic32_t st22_rx_sessions_cnt;
  rte_atomic32_t st30_rx_sessions_cnt;
  rte_atomic32_t st40_rx_sessions_cnt;
  rte_atomic32_t st41_rx_sessions_cnt;
  /* active lcore cnt */
  rte_atomic32_t lcore_cnt;

  /* rx timestamp register */
  int dynfield_offset;

  struct mt_dma_mgr dma_mgr;

  struct mt_map_mgr map_mgr;

  uint16_t pkt_udp_suggest_max_size;
  uint16_t rx_pool_data_size;
  int mempool_idx;

  int arp_timeout_ms;
  bool privileged; /* if app running with root privilege */

  /* connect to mtl manager */
  int instance_fd;
};

static inline struct mtl_init_params* mt_get_user_params(struct mtl_main_impl* impl) {
  return &impl->user_para;
}

static inline bool mt_is_privileged(struct mtl_main_impl* impl) {
  return impl->privileged;
}

static inline bool mt_is_manager_connected(struct mtl_main_impl* impl) {
  return impl->instance_fd > 0;
}

static inline struct mt_interface* mt_if(struct mtl_main_impl* impl, enum mtl_port port) {
  return &impl->inf[port];
}

static inline uint16_t mt_port_id(struct mtl_main_impl* impl, enum mtl_port port) {
  return mt_if(impl, port)->port_id;
}

static inline struct rte_device* mt_port_device(struct mtl_main_impl* impl,
                                                enum mtl_port port) {
  return mt_if(impl, port)->dev_info.device;
}

static inline enum mt_port_type mt_port_type(struct mtl_main_impl* impl,
                                             enum mtl_port port) {
  return mt_if(impl, port)->drv_info.port_type;
}

enum mtl_port mt_port_by_id(struct mtl_main_impl* impl, uint16_t port_id);
uint8_t* mt_sip_addr(struct mtl_main_impl* impl, enum mtl_port port);
uint8_t* mt_sip_netmask(struct mtl_main_impl* impl, enum mtl_port port);
uint8_t* mt_sip_gateway(struct mtl_main_impl* impl, enum mtl_port port);
int mt_dst_ip_mac(struct mtl_main_impl* impl, uint8_t dip[MTL_IP_ADDR_LEN],
                  struct rte_ether_addr* ea, enum mtl_port port, int timeout_ms);

static inline enum mtl_pmd_type mt_pmd_type(struct mtl_main_impl* impl,
                                            enum mtl_port port) {
  return mt_get_user_params(impl)->pmd[port];
}

static inline bool mt_pmd_is_dpdk_user(struct mtl_main_impl* impl, enum mtl_port port) {
  if (MTL_PMD_DPDK_USER == mt_get_user_params(impl)->pmd[port])
    return true;
  else
    return false;
}

static inline bool mt_pmd_is_kernel_based(struct mtl_main_impl* impl,
                                          enum mtl_port port) {
  if (MTL_PMD_DPDK_USER == mt_get_user_params(impl)->pmd[port])
    return false;
  else
    return true;
}

static inline bool mt_drv_use_kernel_ctl(struct mtl_main_impl* impl, enum mtl_port port) {
  if (mt_if(impl, port)->drv_info.flags & MT_DRV_F_USE_KERNEL_CTL)
    return true;
  else
    return false;
}

static inline bool mt_drv_dpdk_based(struct mtl_main_impl* impl, enum mtl_port port) {
  if (mt_if(impl, port)->drv_info.flags & MT_DRV_F_NOT_DPDK_PMD)
    return false;
  else
    return true;
}

static inline bool mt_drv_mcast_in_dp(struct mtl_main_impl* impl, enum mtl_port port) {
  if (mt_if(impl, port)->drv_info.flags & MT_DRV_F_MCAST_IN_DP)
    return true;
  else
    return false;
}

static inline const char* mt_kernel_if_name(struct mtl_main_impl* impl,
                                            enum mtl_port port) {
  return impl->kport_info.kernel_if[port];
}

static inline bool mt_drv_no_cni(struct mtl_main_impl* impl, enum mtl_port port) {
  if (mt_if(impl, port)->drv_info.flags & MT_DRV_F_NO_CNI)
    return true;
  else
    return false;
}

static inline bool mt_drv_no_sys_txq(struct mtl_main_impl* impl, enum mtl_port port) {
  if (mt_if(impl, port)->drv_info.flags & MT_DRV_F_NO_SYS_TX_QUEUE)
    return true;
  else
    return false;
}

static inline bool mt_drv_kernel_based(struct mtl_main_impl* impl, enum mtl_port port) {
  if (mt_if(impl, port)->drv_info.flags & MT_DRV_F_KERNEL_BASED)
    return true;
  else
    return false;
}

static inline bool mt_pmd_is_dpdk_af_xdp(struct mtl_main_impl* impl, enum mtl_port port) {
  if (MTL_PMD_DPDK_AF_XDP == mt_get_user_params(impl)->pmd[port])
    return true;
  else
    return false;
}

static inline bool mt_pmd_is_dpdk_af_packet(struct mtl_main_impl* impl,
                                            enum mtl_port port) {
  if (MTL_PMD_DPDK_AF_PACKET == mt_get_user_params(impl)->pmd[port])
    return true;
  else
    return false;
}

static inline bool mt_pmd_is_kernel_socket(struct mtl_main_impl* impl,
                                           enum mtl_port port) {
  if (MTL_PMD_KERNEL_SOCKET == mt_get_user_params(impl)->pmd[port])
    return true;
  else
    return false;
}

static inline bool mt_pmd_is_native_af_xdp(struct mtl_main_impl* impl,
                                           enum mtl_port port) {
  if (MTL_PMD_NATIVE_AF_XDP == mt_get_user_params(impl)->pmd[port])
    return true;
  else
    return false;
}

static inline int mt_num_ports(struct mtl_main_impl* impl) {
  return RTE_MIN(mt_get_user_params(impl)->num_ports, MTL_PORT_MAX);
}

bool mt_is_valid_socket(struct mtl_main_impl* impl, int soc_id);

/* if user enable the phc2sys service */
static inline bool mt_user_phc2sys_service(struct mtl_main_impl* impl) {
  if (mt_get_user_params(impl)->flags & MTL_FLAG_PHC2SYS_ENABLE)
    return true;
  else
    return false;
}

/* if user enable the ptp service */
static inline bool mt_user_ptp_service(struct mtl_main_impl* impl) {
  if (mt_get_user_params(impl)->flags & MTL_FLAG_PTP_ENABLE)
    return true;
  else
    return false;
}

/* if user enable the not numa bind for lcore thread */
static inline bool mt_user_not_bind_numa(struct mtl_main_impl* impl) {
  if (mt_get_user_params(impl)->flags & MTL_FLAG_NOT_BIND_NUMA)
    return true;
  else
    return false;
}

/* if user enable the auto start/stop */
static inline bool mt_user_auto_start_stop(struct mtl_main_impl* impl) {
  if (mt_get_user_params(impl)->flags & MTL_FLAG_DEV_AUTO_START_STOP)
    return true;
  else
    return false;
}

/* if user enable the auto start/stop */
static inline bool mt_user_across_numa_core(struct mtl_main_impl* impl) {
  if (mt_get_user_params(impl)->flags & MTL_FLAG_ALLOW_ACROSS_NUMA_CORE)
    return true;
  else
    return false;
}

/* if user enable the MTL_FLAG_NO_MULTICAST */
static inline bool mt_user_no_multicast(struct mtl_main_impl* impl) {
  if (mt_get_user_params(impl)->flags & MTL_FLAG_NO_MULTICAST)
    return true;
  else
    return false;
}

/* if user disable the af xdp zc */
static inline bool mt_user_af_xdp_zc(struct mtl_main_impl* impl) {
  if (mt_get_user_params(impl)->flags & MTL_FLAG_AF_XDP_ZC_DISABLE)
    return false;
  else
    return true;
}

/* if user enable the ptp time source func */
static inline bool mt_user_ptp_time_fn(struct mtl_main_impl* impl) {
  if (mt_get_user_params(impl)->ptp_get_time_fn)
    return true;
  else
    return false;
}

/* if user has customized sch quota */
static inline bool mt_user_quota_active(struct mtl_main_impl* impl) {
  if (mt_get_user_params(impl)->data_quota_mbs_per_sch)
    return true;
  else
    return false;
}

/* if user enable hw offload timestamp */
static inline bool mt_user_hw_timestamp(struct mtl_main_impl* impl) {
  if (mt_get_user_params(impl)->flags & MTL_FLAG_ENABLE_HW_TIMESTAMP)
    return true;
  else
    return false;
}

/* if user enable separate sch for rx video session */
static inline bool mt_user_rxv_separate_sch(struct mtl_main_impl* impl) {
  if (mt_get_user_params(impl)->flags & MTL_FLAG_RX_SEPARATE_VIDEO_LCORE)
    return true;
  else
    return false;
}

/* if user enable dedicated lcore for system tasks(CNI, PTP, etc...) */
static inline bool mt_user_dedicated_sys_lcore(struct mtl_main_impl* impl) {
  if (mt_get_user_params(impl)->flags & MTL_FLAG_DEDICATED_SYS_LCORE)
    return true;
  else
    return false;
}

/* if user enable tx video migrate feature */
static inline bool mt_user_tx_video_migrate(struct mtl_main_impl* impl) {
  if (mt_get_user_params(impl)->flags & MTL_FLAG_TX_VIDEO_MIGRATE)
    return true;
  else
    return false;
}

/* if user enable rx video migrate feature */
static inline bool mt_user_rx_video_migrate(struct mtl_main_impl* impl) {
  if (mt_get_user_params(impl)->flags & MTL_FLAG_RX_VIDEO_MIGRATE)
    return true;
  else
    return false;
}

/* if user enable tasklet time measure */
static inline bool mt_user_tasklet_time_measure(struct mtl_main_impl* impl) {
  if (mt_get_user_params(impl)->flags & MTL_FLAG_TASKLET_TIME_MEASURE)
    return true;
  else
    return false;
}

bool mt_sessions_time_measure(struct mtl_main_impl* impl);

/* if user enable rx mono pool */
static inline bool mt_user_rx_mono_pool(struct mtl_main_impl* impl) {
  if (mt_get_user_params(impl)->flags & MTL_FLAG_RX_MONO_POOL)
    return true;
  else
    return false;
}

/* if user enable tx mono pool */
static inline bool mt_user_tx_mono_pool(struct mtl_main_impl* impl) {
  if (mt_get_user_params(impl)->flags & MTL_FLAG_TX_MONO_POOL)
    return true;
  else
    return false;
}

/* if user force tx to no chain mode */
static inline bool mt_user_tx_no_chain(struct mtl_main_impl* impl) {
  if (mt_get_user_params(impl)->flags & MTL_FLAG_TX_NO_CHAIN)
    return true;
  else
    return false;
}

static inline bool mt_has_cni(struct mtl_main_impl* impl, enum mtl_port port) {
  if (impl->cni.entries[port].rxq)
    return true;
  else
    return false;
}

static inline bool mt_has_cni_rx(struct mtl_main_impl* impl, enum mtl_port port) {
  if ((mt_get_user_params(impl)->flags & MTL_FLAG_RX_USE_CNI) && mt_has_cni(impl, port))
    return true;
  else
    return false;
}

static inline bool mt_has_virtio_user(struct mtl_main_impl* impl, enum mtl_port port) {
  if ((mt_get_user_params(impl)->flags & MTL_FLAG_VIRTIO_USER) &&
      mt_pmd_is_dpdk_user(impl, port))
    return true;
  else
    return false;
}

static inline bool mt_dhcp_service_active(struct mtl_main_impl* impl,
                                          enum mtl_port port) {
  if ((mt_if(impl, port)->net_proto == MTL_PROTO_DHCP) && impl->dhcp[port])
    return true;
  else
    return false;
}

static inline enum mtl_rss_mode mt_if_rss_mode(struct mtl_main_impl* impl,
                                               enum mtl_port port) {
  return mt_if(impl, port)->rss_mode;
}

static inline bool mt_has_srss(struct mtl_main_impl* impl, enum mtl_port port) {
  return mt_if_rss_mode(impl, port) != MTL_RSS_MODE_NONE;
}

/* if user enable udp lcore mode */
static inline bool mt_user_udp_lcore(struct mtl_main_impl* impl, enum mtl_port port) {
  MTL_MAY_UNUSED(port);
  if (mt_get_user_params(impl)->flags & MTL_FLAG_UDP_LCORE)
    return true;
  else
    return false;
}

/* if user enable random src port */
static inline bool mt_user_random_src_port(struct mtl_main_impl* impl) {
  if (mt_get_user_params(impl)->flags & MTL_FLAG_RANDOM_SRC_PORT)
    return true;
  else
    return false;
}

/* if user enable multi src port */
static inline bool mt_user_multi_src_port(struct mtl_main_impl* impl) {
  if (mt_get_user_params(impl)->flags & MTL_FLAG_MULTI_SRC_PORT)
    return true;
  else
    return false;
}

/* if user enable shared tx queue */
static inline bool mt_user_shared_txq(struct mtl_main_impl* impl, enum mtl_port port) {
  MTL_MAY_UNUSED(port);
  if (mt_get_user_params(impl)->flags & MTL_FLAG_SHARED_TX_QUEUE)
    return true;
  else
    return false;
}

/* if user enable shared rx queue */
static inline bool mt_user_shared_rxq(struct mtl_main_impl* impl, enum mtl_port port) {
  MTL_MAY_UNUSED(port);
  if (mt_get_user_params(impl)->flags & MTL_FLAG_SHARED_RX_QUEUE)
    return true;
  else
    return false;
}

/* if user disable system rx queue */
static inline bool mt_user_no_system_rxq(struct mtl_main_impl* impl) {
  if (mt_get_user_params(impl)->flags & MTL_FLAG_DISABLE_SYSTEM_RX_QUEUES)
    return true;
  else
    return false;
}

/* if user enable ptp tsc source */
static inline bool mt_user_ptp_tsc_source(struct mtl_main_impl* impl) {
  if (mt_get_user_params(impl)->flags & MTL_FLAG_PTP_SOURCE_TSC)
    return true;
  else
    return false;
}

/* if user enable tasklet thread */
static inline bool mt_user_tasklet_thread(struct mtl_main_impl* impl) {
  if (mt_get_user_params(impl)->flags & MTL_FLAG_TASKLET_THREAD)
    return true;
  else
    return false;
}

/* if user enable tasklet sleep */
static inline bool mt_user_tasklet_sleep(struct mtl_main_impl* impl) {
  if (mt_get_user_params(impl)->flags & MTL_FLAG_TASKLET_SLEEP)
    return true;
  else
    return false;
}

static inline bool mt_if_has_timesync(struct mtl_main_impl* impl, enum mtl_port port) {
  if (mt_if(impl, port)->feature & MT_IF_FEATURE_TIMESYNC)
    return true;
  else
    return false;
}

static inline bool mt_if_has_offload_timestamp(struct mtl_main_impl* impl,
                                               enum mtl_port port) {
  if (mt_if(impl, port)->feature & MT_IF_FEATURE_RX_OFFLOAD_TIMESTAMP)
    return true;
  else
    return false;
}

static inline bool mt_if_has_offload_ipv4_cksum(struct mtl_main_impl* impl,
                                                enum mtl_port port) {
  if (mt_if(impl, port)->feature & MT_IF_FEATURE_TX_OFFLOAD_IPV4_CKSUM)
    return true;
  else
    return false;
}

static inline bool mt_if_has_multi_seg(struct mtl_main_impl* impl, enum mtl_port port) {
  if (mt_if(impl, port)->feature & MT_IF_FEATURE_TX_MULTI_SEGS)
    return true;
  else
    return false;
}

static inline bool mt_if_has_hdr_split(struct mtl_main_impl* impl, enum mtl_port port) {
  if (mt_if(impl, port)->feature & MT_IF_FEATURE_RXQ_OFFLOAD_BUFFER_SPLIT)
    return true;
  else
    return false;
}

#ifdef MTL_SIMULATE_PACKET_DROPS
static inline bool mt_if_has_packet_loss_simulation(struct mtl_main_impl* impl) {
  if (mt_get_user_params(impl)->flags & MTL_FLAG_REDUNDANT_SIMULATE_PACKET_LOSS)
    return true;
  else
    return false;
}
#endif /* MTL_SIMULATE_PACKET_DROPS */

static inline struct rte_mempool* mt_if_hdr_split_pool(struct mt_interface* inf,
                                                       uint16_t q) {
  return inf->rx_queues[q].mbuf_payload_pool;
}

static inline uint16_t mt_if_nb_tx_desc(struct mtl_main_impl* impl, enum mtl_port port) {
  return mt_if(impl, port)->nb_tx_desc;
}

static inline bool mt_if_port_is_down(struct mtl_main_impl* impl, enum mtl_port port) {
  return !!(mt_if(impl, port)->status & MT_IF_STAT_PORT_DOWN);
}

static inline bool mt_if_allow_port_down(struct mtl_main_impl* impl, enum mtl_port port) {
  return !!(mt_get_user_params(impl)->port_params[port].flags & MTL_PORT_FLAG_ALLOW_DOWN_INITIALIZATION);
}

static inline uint16_t mt_if_nb_rx_desc(struct mtl_main_impl* impl, enum mtl_port port) {
  return mt_if(impl, port)->nb_rx_desc;
}

static inline uint16_t mt_if_nb_tx_burst(struct mtl_main_impl* impl, enum mtl_port port) {
  uint16_t burst_pkts;

  if (mt_pmd_is_dpdk_af_xdp(impl, port)) {
    /* same umem for both tx and rx */
    burst_pkts = RTE_MAX(mt_if_nb_rx_desc(impl, port), mt_if_nb_tx_desc(impl, port));
  } else {
    burst_pkts = mt_if_nb_tx_desc(impl, port);
  }

  return burst_pkts;
}

static inline int mt_socket_id(struct mtl_main_impl* impl, enum mtl_port port) {
  return mt_if(impl, port)->socket_id;
}

static inline bool mt_started(struct mtl_main_impl* impl) {
  if (rte_atomic32_read(&impl->instance_started))
    return true;
  else
    return false;
}

static inline bool mt_in_reset(struct mtl_main_impl* impl) {
  if (rte_atomic32_read(&impl->instance_in_reset))
    return true;
  else
    return false;
}

static inline bool mt_aborted(struct mtl_main_impl* impl) {
  if (rte_atomic32_read(&impl->instance_aborted))
    return true;
  else
    return false;
}

static inline uint32_t mt_sch_schedule_ns(struct mtl_main_impl* impl) {
  return impl->sch_schedule_ns;
}

static inline struct rte_mempool* mt_sys_tx_mempool(struct mtl_main_impl* impl,
                                                    enum mtl_port port) {
  return mt_if(impl, port)->tx_mbuf_pool;
}

static inline struct rte_mempool* mt_sys_rx_mempool(struct mtl_main_impl* impl,
                                                    enum mtl_port port) {
  return mt_if(impl, port)->rx_mbuf_pool;
}

static inline struct rte_mbuf* mt_get_pad(struct mtl_main_impl* impl,
                                          enum mtl_port port) {
  return mt_if(impl, port)->pad;
}

static inline struct mt_dma_mgr* mt_get_dma_mgr(struct mtl_main_impl* impl) {
  return &impl->dma_mgr;
}

static inline uint64_t mt_sch_default_sleep_us(struct mtl_main_impl* impl) {
  return impl->var_para.sch_default_sleep_us;
}

static inline uint64_t mt_sch_force_sleep_us(struct mtl_main_impl* impl) {
  return impl->var_para.sch_force_sleep_us;
}

static inline uint64_t mt_sch_zero_sleep_thresh_us(struct mtl_main_impl* impl) {
  return impl->var_para.sch_zero_sleep_threshold_us;
}

static inline void mt_sleep_us(unsigned int us) {
  return rte_delay_us_sleep(us);
}

static inline void mt_sleep_ms(unsigned int ms) {
  return mt_sleep_us(ms * 1000);
}

static inline void mt_delay_us(unsigned int us) {
  return rte_delay_us_block(us);
}

static inline void mt_free_mbufs(struct rte_mbuf** pkts, int num) {
  for (int i = 0; i < num; i++) {
    rte_pktmbuf_free(pkts[i]);
    pkts[i] = NULL;
  }
}

static inline void mt_mbuf_init_ipv4(struct rte_mbuf* pkt) {
  pkt->l2_len = sizeof(struct rte_ether_hdr); /* 14 */
  pkt->l3_len = sizeof(struct rte_ipv4_hdr);  /* 20 */
#if RTE_VERSION >= RTE_VERSION_NUM(21, 11, 0, 0)
  pkt->ol_flags |= RTE_MBUF_F_TX_IPV4 | RTE_MBUF_F_TX_IP_CKSUM;
#else
  pkt->ol_flags |= PKT_TX_IPV4 | PKT_TX_IP_CKSUM;
#endif
}

static inline uint64_t mt_timespec_to_ns(const struct timespec* ts) {
  return ((uint64_t)ts->tv_sec * NS_PER_S) + ts->tv_nsec;
}

static inline void mt_ns_to_timespec(uint64_t ns, struct timespec* ts) {
  ts->tv_sec = ns / NS_PER_S;
  ts->tv_nsec = ns % NS_PER_S;
}

static inline int mt_wait_tsc_stable(struct mtl_main_impl* impl) {
  if (impl->tsc_cal_tid) {
    pthread_join(impl->tsc_cal_tid, NULL);
    impl->tsc_cal_tid = 0;
  }

  return 0;
}

/* Return relative TSC time in nanoseconds */
static inline uint64_t mt_get_tsc(struct mtl_main_impl* impl) {
  double tsc = rte_get_tsc_cycles();
  double tsc_hz = impl->tsc_hz;
  double time_nano = tsc / (tsc_hz / ((double)NS_PER_S));
  return time_nano;
}

/* busy loop until target time reach */
static inline void mt_tsc_delay_to(struct mtl_main_impl* impl, uint64_t target) {
  while (mt_get_tsc(impl) < target) {
  }
}

/* Monotonic time (in nanoseconds) since some unspecified starting point. */
static inline uint64_t mt_get_monotonic_time(void) {
  struct timespec ts;

  clock_gettime(MT_CLOCK_MONOTONIC_ID, &ts);
  return mt_timespec_to_ns(&ts);
}

static inline uint64_t mt_get_real_time(void) {
  struct timespec ts;

  clock_gettime(CLOCK_REALTIME, &ts);
  return mt_timespec_to_ns(&ts);
}

static inline void st_tx_mbuf_set_tsc(struct rte_mbuf* mbuf, uint64_t time_stamp) {
  struct mt_muf_priv_data* priv = rte_mbuf_to_priv(mbuf);
  priv->tx_priv.tsc_time_stamp = time_stamp;
}

static inline uint64_t st_tx_mbuf_get_tsc(struct rte_mbuf* mbuf) {
  struct mt_muf_priv_data* priv = rte_mbuf_to_priv(mbuf);
  return priv->tx_priv.tsc_time_stamp;
}

static inline void st_tx_mbuf_set_ptp(struct rte_mbuf* mbuf, uint64_t time_stamp) {
  struct mt_muf_priv_data* priv = rte_mbuf_to_priv(mbuf);
  priv->tx_priv.ptp_time_stamp = time_stamp;
}

static inline uint64_t st_tx_mbuf_get_ptp(struct rte_mbuf* mbuf) {
  struct mt_muf_priv_data* priv = rte_mbuf_to_priv(mbuf);
  return priv->tx_priv.ptp_time_stamp;
}

static inline void st_tx_mbuf_set_idx(struct rte_mbuf* mbuf, uint32_t idx) {
  struct mt_muf_priv_data* priv = rte_mbuf_to_priv(mbuf);
  priv->tx_priv.idx = idx;
}

static inline uint32_t st_tx_mbuf_get_idx(struct rte_mbuf* mbuf) {
  struct mt_muf_priv_data* priv = rte_mbuf_to_priv(mbuf);
  return priv->tx_priv.idx;
}

static inline void st_tx_mbuf_set_priv(struct rte_mbuf* mbuf, void* p) {
  struct mt_muf_priv_data* priv = rte_mbuf_to_priv(mbuf);
  priv->tx_priv.priv = p;
}

static inline void* st_tx_mbuf_get_priv(struct rte_mbuf* mbuf) {
  struct mt_muf_priv_data* priv = rte_mbuf_to_priv(mbuf);
  return priv->tx_priv.priv;
}

static inline void st_rx_mbuf_set_lender(struct rte_mbuf* mbuf, uint32_t lender) {
  struct mt_muf_priv_data* priv = rte_mbuf_to_priv(mbuf);
  priv->rx_priv.lender = lender;
}

static inline uint32_t st_rx_mbuf_get_lender(struct rte_mbuf* mbuf) {
  struct mt_muf_priv_data* priv = rte_mbuf_to_priv(mbuf);
  return priv->rx_priv.lender;
}

static inline void st_rx_mbuf_set_offset(struct rte_mbuf* mbuf, uint32_t offset) {
  struct mt_muf_priv_data* priv = rte_mbuf_to_priv(mbuf);
  priv->rx_priv.offset = offset;
}

static inline uint32_t st_rx_mbuf_get_offset(struct rte_mbuf* mbuf) {
  struct mt_muf_priv_data* priv = rte_mbuf_to_priv(mbuf);
  return priv->rx_priv.offset;
}

static inline void st_rx_mbuf_set_len(struct rte_mbuf* mbuf, uint32_t len) {
  struct mt_muf_priv_data* priv = rte_mbuf_to_priv(mbuf);
  priv->rx_priv.len = len;
}

static inline uint32_t st_rx_mbuf_get_len(struct rte_mbuf* mbuf) {
  struct mt_muf_priv_data* priv = rte_mbuf_to_priv(mbuf);
  return priv->rx_priv.len;
}

uint64_t mt_mbuf_time_stamp(struct mtl_main_impl* impl, struct rte_mbuf* mbuf,
                            enum mtl_port port);

static inline uint64_t mt_get_ptp_time(struct mtl_main_impl* impl, enum mtl_port port) {
  return mt_if(impl, port)->ptp_get_time_fn(impl, port);
}

int mt_ptp_wait_stable(struct mtl_main_impl* impl, enum mtl_port port, int timeout_ms);

uint64_t mt_get_raw_ptp_time(struct mtl_main_impl* impl, enum mtl_port port);

#if RTE_VERSION >= RTE_VERSION_NUM(21, 11, 0, 0)
static inline struct rte_ether_addr* mt_eth_s_addr(struct rte_ether_hdr* eth) {
  return &eth->src_addr;
}

static inline struct rte_ether_addr* mt_eth_d_addr(struct rte_ether_hdr* eth) {
  return &eth->dst_addr;
}
#else
static inline struct rte_ether_addr* mt_eth_s_addr(struct rte_ether_hdr* eth) {
  return &eth->s_addr;
}

static inline struct rte_ether_addr* mt_eth_d_addr(struct rte_ether_hdr* eth) {
  return &eth->d_addr;
}
#endif

#if (JSON_C_VERSION_NUM >= ((0 << 16) | (13 << 8) | 0)) || \
    (JSON_C_VERSION_NUM < ((0 << 16) | (10 << 8) | 0))
static inline json_object* mt_json_object_get(json_object* obj, const char* key) {
  return json_object_object_get(obj, key);
}
#else
static inline json_object* mt_json_object_get(json_object* obj, const char* key) {
  json_object* value;
  int ret = json_object_object_get_ex(obj, key, &value);
  if (ret) return value;
  return NULL;
}
#endif

static inline bool mt_spinlock_lock_timeout(struct mtl_main_impl* impl,
                                            rte_spinlock_t* lock, int timeout_us) {
  uint64_t time = mt_get_tsc(impl);
  uint64_t end = time + timeout_us * NS_PER_US;
  while (time < end) {
    if (rte_spinlock_trylock(lock)) return true;
    time = mt_get_tsc(impl);
  }
  return false; /* timeout */
}

#endif
