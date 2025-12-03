/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

/**
 * @file mtl_api.h
 *
 * This header define the public interfaces of Media Transport Library.
 *
 */

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifndef _MTL_API_HEAD_H_
#define _MTL_API_HEAD_H_

#if defined(__cplusplus)
extern "C" {
#endif

#include "mtl_build_config.h"

/**
 * Compiler specific pack specifier
 */
#ifdef __GNUC__
#define MTL_PACK(__Declaration__) __Declaration__ __attribute__((__packed__))
#endif

#ifdef _MSC_VER
#define MTL_PACK(__Declaration__) \
  __pragma(pack(push, 1)) __Declaration__ __pragma(pack(pop))
#endif

/**
 * Macro to compute a version number usable for comparisons
 */
#define MTL_VERSION_NUM(a, b, c) ((a) << 16 | (b) << 8 | (c))
/**
 * All version numbers in one to compare with ST_VERSION_NUM()
 */
#define MTL_VERSION \
  MTL_VERSION_NUM(MTL_VERSION_MAJOR, MTL_VERSION_MINOR, MTL_VERSION_LAST)

/**
 * Get the uint64_t value for a specified bit set(0 to 63).
 */
#define MTL_BIT64(nr) (UINT64_C(1) << (nr))

/**
 * Get the uint32_t value for a specified bit set(0 to 31).
 */
#define MTL_BIT32(nr) (UINT32_C(1) << (nr))

/**
 * Max length of a DPDK port name and session logical port
 */
#define MTL_PORT_MAX_LEN (64)
/**
 * Length of a IPV4 address
 */
#define MTL_IP_ADDR_LEN (4)
/**
 * Length of a mac address
 */
#define MTL_MAC_ADDR_LEN (6)
/**
 * Defined if current platform is little endian
 */
#define MTL_LITTLE_ENDIAN /* x86 use little endian */

/** Standard mtu size is 1500 */
#define MTL_MTU_MAX_BYTES (1500)

/** Standard UDP is 1460 bytes, mtu is 1500 */
#define MTL_UDP_MAX_BYTES (1460)

/**
 * Max bytes in one RTP packet, include payload and header
 * standard UDP is 1460 bytes, and UDP headers are 8 bytes
 * leave 100 for network extra space
 */
#define MTL_PKT_MAX_RTP_BYTES (MTL_UDP_MAX_BYTES - 8 - 100)

/**
 * Max allowed number of dma devs
 */
#define MTL_DMA_DEV_MAX (32)

/**
 * Max length of a pcap dump filename
 */
#define MTL_PCAP_FILE_MAX_LEN (32)

/** Helper to get array size from arrays */
#define MTL_ARRAY_SIZE(arr) (sizeof(arr) / sizeof(arr[0]))

/** Helper to get M unit */
#define MTL_STAT_M_UNIT (1000 * 1000)

/**
 * Handle to MTL transport device context
 */
typedef struct mtl_main_impl* mtl_handle;
/**
 * Handle to st user dma device
 */
typedef struct mtl_dma_lender_dev* mtl_udma_handle;

/**
 * IO virtual address type.
 */
typedef uint64_t mtl_iova_t;

/**
 * CPU virtual address type.
 */
typedef uint64_t mtl_cpuva_t;

/**
 * Handle to dma mem
 */
typedef struct mtl_dma_mem* mtl_dma_mem_handle;

/**
 * Bad IOVA address
 */
#define MTL_BAD_IOVA ((mtl_iova_t)-1)

/**
 * Macro to align a value, align should be a power-of-two value.
 */
#define MTL_ALIGN(val, align) (((val) + ((align)-1)) & ~((align)-1))

#ifdef __MTL_PYTHON_BUILD__
/** swig not support __deprecated__ */
#define __mtl_deprecated_msg(msg)
#else
/** Macro to mark functions and fields to be removal */
#define __mtl_deprecated_msg(msg) __attribute__((__deprecated__(msg)))
#endif

#ifndef MTL_MAY_UNUSED
/** Macro to mark unused-parameter */
#define MTL_MAY_UNUSED(x) (void)(x)
#endif

/**
 * Port logical type
 */
enum mtl_port {
  MTL_PORT_P = 0, /**< primary port */
  MTL_PORT_R,     /**< redundant port */
  MTL_PORT_2,     /**< port index: 2 */
  MTL_PORT_3,     /**< port index: 3 */
  MTL_PORT_4,     /**< port index: 4 */
  MTL_PORT_5,     /**< port index: 5 */
  MTL_PORT_6,     /**< port index: 6 */
  MTL_PORT_7,     /**< port index: 7 */
};

#ifdef MTL_DEBUG
struct mtl_debug_port_packet_loss {
  /**
   * Debug option for test purposes only.
   * Enable packet loss simulation on redundant TX streams for debug use only.
   * Requires MTL_FLAG_REDUNDANT_SIMULATE_PACKET_LOSS to be set.
   *
   * The fields below let a caller target specific streams or distribute loss by
   * percentage. Without these overrides, the flag alone drops an equal share of
   * packets across redundant streams (currently limited to two).
   *
   * Example for two streams if tx_stream_loss_divider is set to 3 and
   * tx_stream_loss_id is set to 0 on stream 1 and 2 on stream 2:
   *  stream id 1 |PACKET-1--|DROP------|DROP------|...
   *  stream id 2 |DROP------|PACKET-2--|DROP------|...
   *  stream id 3 |DROP------|DROP------|PACKET-3--|...
   */
  uint16_t tx_stream_loss_id;
  uint16_t tx_stream_loss_divider;
};
#endif

#define MTL_PORT_MAX (MTL_PORT_7 + 1)

/**
 * Session port logical type
 */
enum mtl_session_port {
  MTL_SESSION_PORT_P = 0, /**< primary session(logical) port */
  MTL_SESSION_PORT_R,     /**< redundant session(logical) port */
};

#define MTL_SESSION_PORT_MAX (MTL_SESSION_PORT_R + 1)

/**
 * Log level type to MTL context
 */
enum mtl_log_level {
  /** debug log level */
  MTL_LOG_LEVEL_DEBUG = 0,
  /** info log level */
  MTL_LOG_LEVEL_INFO,
  /** notice log level */
  MTL_LOG_LEVEL_NOTICE,
  /** warning log level */
  MTL_LOG_LEVEL_WARNING,
  /** error log level */
  MTL_LOG_LEVEL_ERR,
  /** critical log level */
  MTL_LOG_LEVEL_CRIT,
  /** max value of this enum */
  MTL_LOG_LEVEL_MAX,
};

/* keep compatibility for MTL_LOG_LEVEL_ERR */
#define MTL_LOG_LEVEL_ERROR (MTL_LOG_LEVEL_ERR)

/* log formatter */
typedef void (*mtl_log_prefix_formatter_t)(char* buf, size_t buf_sz);

/* log printer, similar to printf */
typedef void (*mtl_log_printer_t)(enum mtl_log_level level, const char* format, ...);

/**
 * Poll mode driver type, not change the enum value any more if one PMD type is marked as
 * production quality.
 */
enum mtl_pmd_type {
  /** DPDK user driver PMD */
  MTL_PMD_DPDK_USER = 0,
  /** Run MTL directly on AF_XDP, CAP_NET_RAW is needed for UMEM creation */
  MTL_PMD_NATIVE_AF_XDP = 4,

  MTL_PMD_EXPERIMENTAL = 16,
  /** Below PMDs are only for experimental usage, not for production usage. */
  /** experimental, Run MTL directly on kernel socket APIs */
  MTL_PMD_KERNEL_SOCKET = 17,
  /** experimental, DPDK PMD with address family(kernel) high performance packet
     processing */
  MTL_PMD_DPDK_AF_XDP = 19,
  /** experimental, DPDK PMD send and receive raw packets through the kernel */
  MTL_PMD_DPDK_AF_PACKET = 20,
  /** max value of this enum */
  MTL_PMD_TYPE_MAX,
};

/**
 * RSS mode
 */
enum mtl_rss_mode {
  /** not using rss */
  MTL_RSS_MODE_NONE = 0,
  /** hash with both l3 src and dst, not use now */
  MTL_RSS_MODE_L3,
  /** hash with l3 src and dst address, l4 src port and dst port, used with shared rss */
  MTL_RSS_MODE_L3_L4,
  /** max value of this enum */
  MTL_RSS_MODE_MAX,
};

/**
 * IOVA mode
 */
enum mtl_iova_mode {
  /** let DPDK to choose IOVA mode */
  MTL_IOVA_MODE_AUTO = 0,
  /** using IOVA VA mode */
  MTL_IOVA_MODE_VA,
  /** using IOVA PA mode */
  MTL_IOVA_MODE_PA,
  /** max value of this enum */
  MTL_IOVA_MODE_MAX,
};

/**
 * Interface network protocol
 */
enum mtl_net_proto {
  /** using static IP configuration */
  MTL_PROTO_STATIC = 0,
  /** using DHCP(auto) IP configuration */
  MTL_PROTO_DHCP,
  /** max value of this enum */
  MTL_PROTO_MAX,
};

/**
 * Transport type
 */
enum mtl_transport_type {
  /** st2110 protocol transport */
  MTL_TRANSPORT_ST2110 = 0,
  /** udp transport */
  MTL_TRANSPORT_UDP,
  /** max value of this enum */
  MTL_TRANSPORT_TYPE_MAX,
};

/**
 * SIMD level type
 */
enum mtl_simd_level {
  MTL_SIMD_LEVEL_NONE = 0,     /**< Scalar */
  MTL_SIMD_LEVEL_AVX2,         /**< AVX2 */
  MTL_SIMD_LEVEL_AVX512,       /**< AVX512 */
  MTL_SIMD_LEVEL_AVX512_VBMI2, /**< AVX512 VBMI2 */
  MTL_SIMD_LEVEL_MAX,          /**< max value of this enum */
};

/**
 * st21 tx pacing way
 */
enum st21_tx_pacing_way {
  /** auto detected pacing */
  ST21_TX_PACING_WAY_AUTO = 0,
  /** rate limit based pacing */
  ST21_TX_PACING_WAY_RL,
  /** tsc based pacing */
  ST21_TX_PACING_WAY_TSC,
  /** tsn based pacing */
  ST21_TX_PACING_WAY_TSN,
  /** ptp based pacing */
  ST21_TX_PACING_WAY_PTP,
  /** best effort sending */
  ST21_TX_PACING_WAY_BE,
  /** tsc based pacing with single bulk transmitter */
  ST21_TX_PACING_WAY_TSC_NARROW,
  /** Max value of this enum */
  ST21_TX_PACING_WAY_MAX,
};

/** MTL init flag */
enum mtl_init_flag {
  /** lib will bind all MTL threads to NIC numa socket, default behavior */
  MTL_FLAG_BIND_NUMA = (MTL_BIT64(0)),
  /** Enable built-in PTP implementation */
  MTL_FLAG_PTP_ENABLE = (MTL_BIT64(1)),
  /** Separated lcore for RX video(st2110-20/st2110-22) sessions. */
  MTL_FLAG_RX_SEPARATE_VIDEO_LCORE = (MTL_BIT64(2)),
  /**
   * Enable migrate mode for rx video session if current LCORE is too busy to handle the
   * rx video tasklet, the busy session may be migrated to a new LCORE.
   * If not enable, rx video will always use static mapping based on quota.
   */
  MTL_FLAG_TX_VIDEO_MIGRATE = (MTL_BIT64(3)),
  /**
   * Enable migrate mode for rx video session if current LCORE is too busy to handle the
   * rx video tasklet, the busy session may be migrated to a new LCORE.
   * If not enable, rx video will always use static mapping based on quota.
   */
  MTL_FLAG_RX_VIDEO_MIGRATE = (MTL_BIT64(4)),
  /**
   * Run the tasklet inside one thread instead of a pinned lcore.
   */
  MTL_FLAG_TASKLET_THREAD = (MTL_BIT64(5)),
  /**
   * Enable the tasklet sleep if routine report task done.
   */
  MTL_FLAG_TASKLET_SLEEP = (MTL_BIT64(6)),
  /**
   * Set the supported SIMD bitwidth of rx/tx burst to 512 bit(AVX512).
   */
  MTL_FLAG_RXTX_SIMD_512 = (MTL_BIT64(7)),
  /**
   * Enable HW offload timestamp for all RX packets target the compliance analyze. Only
   * can work for PF on E810 now.
   */
  MTL_FLAG_ENABLE_HW_TIMESTAMP = (MTL_BIT64(8)),
  /**
   * Use PI controller for built-in PTP implementation, only for PF now.
   */
  MTL_FLAG_PTP_PI = (MTL_BIT64(9)),
  /**
   * Enable background lcore mode for MTL_TRANSPORT_UDP.
   */
  MTL_FLAG_UDP_LCORE = (MTL_BIT64(10)),
  /**
   * Enable random source port for MTL_TRANSPORT_ST2110 tx.
   */
  MTL_FLAG_RANDOM_SRC_PORT = (MTL_BIT64(11)),
  /**
   * Enable multiple source port for MTL_TRANSPORT_ST2110 20 tx.
   */
  MTL_FLAG_MULTI_SRC_PORT = (MTL_BIT64(12)),
  /**
   * Enable shared queue for tx.
   */
  MTL_FLAG_SHARED_TX_QUEUE = (MTL_BIT64(13)),
  /**
   * Enable shared queue for rx.
   */
  MTL_FLAG_SHARED_RX_QUEUE = (MTL_BIT64(14)),
  /**
   * Enable built-in PHC2SYS implementation.
   * CAP_SYS_TIME is needed.
   */
  MTL_FLAG_PHC2SYS_ENABLE = (MTL_BIT64(15)),
  /**
   * Enable virtio_user as exception path.
   * CAP_NET_ADMIN is needed.
   */
  MTL_FLAG_VIRTIO_USER = (MTL_BIT64(16)),
  /**
   * Do mtl_start in mtl_init, mtl_stop in mtl_uninit, and skip the mtl_start/mtl_stop
   */
  MTL_FLAG_DEV_AUTO_START_STOP = (MTL_BIT64(17)),
  /**
   * Enable the use of cores across NUMA nodes; by default, only cores within the same
   * NUMA node as the NIC are used due to the high cost of cross-NUMA communication.
   */
  MTL_FLAG_ALLOW_ACROSS_NUMA_CORE = (MTL_BIT64(18)),
  /** not send multicast join message, for the SDN switch case which deliver the stream
   * directly
   */
  MTL_FLAG_NO_MULTICAST = (MTL_BIT64(19)),
  /** Dedicated lcore for system CNI tasks. */
  MTL_FLAG_DEDICATED_SYS_LCORE = (MTL_BIT64(20)),
  /** not bind all MTL threads to NIC numa socket */
  MTL_FLAG_NOT_BIND_NUMA = (MTL_BIT64(21)),

  /**
   * use thread for cni message handling
   */
  MTL_FLAG_CNI_THREAD = (MTL_BIT64(32)),
  /**
   * use lcore tasklet for cni message handling
   */
  MTL_FLAG_CNI_TASKLET = (MTL_BIT64(33)),
  /**
   * Enable NIC promiscuous mode for RX
   */
  MTL_FLAG_NIC_RX_PROMISCUOUS = (MTL_BIT64(34)),
  /**
   * use unicast address for ptp PTP_DELAY_REQ message
   */
  MTL_FLAG_PTP_UNICAST_ADDR = (MTL_BIT64(35)),
  /**
   * Mono memory pool for all rx queue(sessions)
   */
  MTL_FLAG_RX_MONO_POOL = (MTL_BIT64(36)),
  /**
   * Enable the routine time measurement in tasklet and sessions
   */
  MTL_FLAG_TASKLET_TIME_MEASURE = (MTL_BIT64(38)),
  /**
   * Disable the zero copy for af_xdp, use copy mode only
   */
  MTL_FLAG_AF_XDP_ZC_DISABLE = (MTL_BIT64(39)),
  /**
   * Mono memory pool for all tx queue(session)
   */
  MTL_FLAG_TX_MONO_POOL = (MTL_BIT64(40)),
  /**
   * Disable system rx queues, pls use mcast or manual TX mac.
   */
  MTL_FLAG_DISABLE_SYSTEM_RX_QUEUES = (MTL_BIT64(41)),
  /**

   * Force to get ptp time from tsc source.
   */
  MTL_FLAG_PTP_SOURCE_TSC = (MTL_BIT64(42)),
  /**
   * Disable TX chain mbuf, use same mbuf for header and payload.
   * Will do memcpy from framebuffer to packet payload.
   */
  MTL_FLAG_TX_NO_CHAIN = (MTL_BIT64(43)),
  /**
   * Disable the pkt check for TX burst API.
   */
  MTL_FLAG_TX_NO_BURST_CHK = (MTL_BIT64(44)),
  /**
   * Use CNI based queue for RX.
   */
  MTL_FLAG_RX_USE_CNI = (MTL_BIT64(45)),
  /**
   * To exclusively use port only for flow, the application must ensure that all RX
   * streams have unique UDP port numbers.
   */
  MTL_FLAG_RX_UDP_PORT_ONLY = (MTL_BIT64(46)),
  /** not bind current process to NIC numa socket */
  MTL_FLAG_NOT_BIND_PROCESS_NUMA = (MTL_BIT64(47)),

#ifdef MTL_DEBUG
  /** tis to test don't use ok babe ? ⸜(｡˃ ᵕ ˂ )⸝♡ */
  MTL_FLAG_REDUNDANT_SIMULATE_PACKET_LOSS = (MTL_BIT64(63))
#endif
};

/** MTL port init flag */
enum mtl_port_init_flag {
  /** user force the NUMA id instead reading from NIC PCIE topology */
  MTL_PORT_FLAG_FORCE_NUMA = (MTL_BIT64(0)),
};

struct mtl_ptp_sync_notify_meta {
  /** offset to UTC of current master PTP */
  int16_t master_utc_offset;
  /* phc delta of current sync */
  int64_t delta;
};

/**
 * The structure describing how to init a mtl port device.
 */
struct mtl_port_init_params {
  /** Optional. Flags to control MTL port. See MTL_PORT_FLAG_* for possible value */
  uint64_t flags;
  /** Lib will force assign the numa for current port to this numa id if
   * MTL_PORT_FLAG_FORCE_NUMA is set. Not set MTL_PORT_FLAG_FORCE_NUMA if you don't know
   * the detail.
   */
  int socket_id;
};

/**
 * The structure describing how to init the mtl context.
 * Include the PCIE port and other required info.
 */
struct mtl_init_params {
  /**
   * Mandatory. PCIE BDF port, ex: 0000:af:01.0.
   * MTL_PMD_NATIVE_AF_XDP, use native_af_xdp + ifname, ex: native_af_xdp:enp175s0f0.
   *
   * Below PMDs are only for experimental usage, not for production usage.
   * MTL_PMD_KERNEL_SOCKET, use kernel + ifname, ex: kernel:enp175s0f0.
   * MTL_PMD_DPDK_AF_XDP, use dpdk_af_xdp + ifname, ex: dpdk_af_xdp:enp175s0f0.
   * MTL_PMD_DPDK_AF_PACKET, use dpdk_af_packet + ifname, ex: dpdk_af_packet:enp175s0f0.
   */
  char port[MTL_PORT_MAX][MTL_PORT_MAX_LEN];
#ifdef MTL_DEBUG
  /** Debug option for test purposes only. See struct mtl_debug_port_packet_loss */
  struct mtl_debug_port_packet_loss port_packet_loss[MTL_PORT_MAX];
#endif
  /** Mandatory. The element number in the port array, 1 to MTL_PORT_MAX_LEN */
  uint8_t num_ports;
  /**
   * Mandatory. Interface network protocol.
   * Static(default) or DHCP(please make sure you have a DHCP server inside LAN)
   */
  enum mtl_net_proto net_proto[MTL_PORT_MAX];
  /** Mandatory. dpdk user pmd(default) or af_xdp. Use mtl_pmd_by_port_name helper to get
   * PMD type */
  enum mtl_pmd_type pmd[MTL_PORT_MAX];
  /**
   * Mandatory. Max NIC tx queues requested the lib to support.
   * for MTL_TRANSPORT_ST2110, you can use helper api: st_tx_sessions_queue_cnt to
   * calculate.
   */
  uint16_t tx_queues_cnt[MTL_PORT_MAX];
  /**
   * Mandatory. Max NIC rx queues requested the lib to support.
   * for MTL_TRANSPORT_ST2110, you can use helper api: st_rx_sessions_queue_cnt to
   * calculate.
   */
  uint16_t rx_queues_cnt[MTL_PORT_MAX];

  /**
   * Mandatory for MTL_PMD_DPDK_USER. The static assigned IP for ports.
   * This is ignored when MTL_PROTO_DHCP enabled.
   */
  uint8_t sip_addr[MTL_PORT_MAX][MTL_IP_ADDR_LEN];
  /**
   * Optional for MTL_PMD_DPDK_USER. Net mask for ports.
   * This is ignored when MTL_PROTO_DHCP enabled.
   * Lib will use 255.255.255.0 as default if this item is zero.
   */
  uint8_t netmask[MTL_PORT_MAX][MTL_IP_ADDR_LEN];
  /**
   * Optional for MTL_PMD_DPDK_USER. Default gateway for ports.
   * This is ignored when MTL_PROTO_DHCP enabled.
   * User can use "route -n" to get gateway before bind the port to DPDK PMD.
   */
  uint8_t gateway[MTL_PORT_MAX][MTL_IP_ADDR_LEN];

  /** Optional. Flags to control MTL behaviors. See MTL_FLAG_* for possible value */
  uint64_t flags;
  /** Optional. Private data to the cb functions(ptp_get_time_fn and stat_dump_cb_fn) */
  void* priv;
  /** Optional. log level control */
  enum mtl_log_level log_level;
  /**
   * Optional. The logical cores list can be used in the MTL, e.g. "28,29,30,31".
   * If not assigned, the core usage will be determined by MTL itself.
   */
  char* lcores;

  /**
   * Optional. Dma(CBDMA or DSA) device can be used in the MTL.
   * DMA can be used to offload the CPU for copy the payload for video rx sessions.
   * See more from ST20_RX_FLAG_DMA_OFFLOAD in st20_api.h.
   * PCIE BDF path like 0000:80:04.0.
   */
  char dma_dev_port[MTL_DMA_DEV_MAX][MTL_PORT_MAX_LEN];
  /** Optional. The element number in the dma_dev_port array, leave to zero if no DMA */
  uint8_t num_dma_dev_port;

  /**
   * Optional. If using rss (L3 or L4) for the rx packets classification, default use RTE
   * flow director.
   */
  enum mtl_rss_mode rss_mode;
  /**
   * Optional. Select default(auto) or force IOVA(va or pa) mode.
   */
  enum mtl_iova_mode iova_mode;
  /**
   * Optional. Number of transmit descriptors for each NIC TX queue, 0 means determined by
   * lib. It will affect the memory usage and the performance.
   */
  uint16_t nb_tx_desc;
  /**
   * Optional. Number of receive descriptors for each NIC RX queue, 0 means determined by
   * lib. It will affect the memory usage and the performance.
   */
  uint16_t nb_rx_desc;

  /**
   * Optional. Function to acquire current ptp time(in nanoseconds) from user.
   * If NULL, MTL will get from built-in ptp source(NIC) if built-in ptp4l is enabled or
   * system time if built-in ptp4l is not enabled.
   */
  uint64_t (*ptp_get_time_fn)(void* priv);
  /** Optional for MTL_FLAG_PTP_ENABLE. The callback is notified every time the built-in
   * PTP protocol receives a valid PTP_DELAY_RESP message from the PTP grandmaster. */
  void (*ptp_sync_notify)(void* priv, struct mtl_ptp_sync_notify_meta* meta);

  /** Optional. Stats dump period in seconds, zero means determined by lib(10s default) */
  uint16_t dump_period_s;
  /** Optional. Stats dump callback for user in every dump_period_s */
  void (*stat_dump_cb_fn)(void* priv);

  /**
   * Optional for MTL_TRANSPORT_ST2110. The st21 tx pacing way, leave to zero(auto) if you
   * don't known the detail.
   */
  enum st21_tx_pacing_way pacing;
  /**
   * Optional for MTL_TRANSPORT_ST2110. The max data quota for the sessions of each lcore
   * can handled, 0 means determined by lib. If exceed this limit, the new created
   * sessions will be scheduled to a new lcore.
   */
  uint32_t data_quota_mbs_per_sch;
  /**
   * Optional for MTL_TRANSPORT_ST2110. The number of max tx audio session for each lcore,
   * 0 means determined by lib */
  uint32_t tx_audio_sessions_max_per_sch;
  /**
   * Optional for MTL_TRANSPORT_ST2110. The number of max rx audio session for each lcore,
   * 0 means determined by lib */
  uint32_t rx_audio_sessions_max_per_sch;
  /**
   * Optional for MTL_TRANSPORT_ST2110. Suggest max allowed udp size for each network pkt,
   * leave to zero if you don't known detail.
   */
  uint16_t pkt_udp_suggest_max_size;
  /**
   * Optional for MTL_TRANSPORT_ST2110. The number for hdr split queues of rx, should be
   * smaller than rx_sessions_cnt_max. Experimental feature for header split.
   */
  uint16_t nb_rx_hdr_split_queues;
  /**
   * Optional for MTL_TRANSPORT_ST2110. Suggest data room size for rx mempool,
   * the final data room size may be aligned to larger value,
   * some NICs may need this to avoid mbuf split.
   */
  uint16_t rx_pool_data_size;
  /** Optional. the maximum number of memzones in DPDK, leave zero to use default 2560 */
  uint32_t memzone_max;

  /** Optional. The number of tasklets for each lcore, 0 means determined by lib */
  uint32_t tasklets_nb_per_sch;

  /** Optional.
   * Set the ARP timeout value in seconds for ST2110 sessions when using a unicast
   * address. Leave to zero to use the system's default timeout of 60 seconds.
   */
  uint16_t arp_timeout_s;

  /** Optional. Number of scheduler(lcore) used for rss dispatch, 0 means only 1 core */
  uint16_t rss_sch_nb[MTL_PORT_MAX];

  /** Optional for MTL_FLAG_PTP_ENABLE. The ptp pi controller proportional gain. */
  double kp;
  /** Optional for MTL_FLAG_PTP_ENABLE. The ptp pi controller integral gain. */
  double ki;

  /** Optional, all future port params should be placed into this struct */
  struct mtl_port_init_params port_params[MTL_PORT_MAX];

  /* The Core ID designated for the DPDK main thread. If set to 0, the DPDK default core
   * is used. */
  uint32_t main_lcore;

  /**
   * deprecated for MTL_TRANSPORT_ST2110.
   * max tx sessions(st20, st22, st30, st40) requested the lib to support,
   * use mtl_get_fix_info to query the actual count.
   * dpdk context will allocate the hw resources(queues, memory) based on this number.
   */
#ifdef __MTL_LIB_BUILD__
  uint16_t tx_sessions_cnt_max;
#else
  uint16_t tx_sessions_cnt_max __mtl_deprecated_msg("Use tx_queues_cnt instead");
#endif
  /**
   * deprecated for MTL_TRANSPORT_ST2110.
   * max rx sessions(st20, st22, st30, st40) requested the lib to support,
   * use mtl_get_fix_info to query the actual count.
   * dpdk context will allocate the hw resources(queues, memory) based on this number.
   */
#ifdef __MTL_LIB_BUILD__
  uint16_t rx_sessions_cnt_max;
#else
  uint16_t rx_sessions_cnt_max __mtl_deprecated_msg("Use rx_queues_cnt instead");
#endif
};

/**
 * A structure used to retrieve fixed info for an MTL instance.
 */
struct mtl_fix_info {
  /** the flags in mtl_init_params */
  uint64_t init_flags;
  /** number of pcie ports */
  uint8_t num_ports;
  /** max dma dev count for current transport context */
  uint8_t dma_dev_cnt_max;
};

/**
 * A structure used to retrieve varied info for an MTL instance.
 */
struct mtl_var_info {
  /** active scheduler count */
  uint8_t sch_cnt;
  /** active lcore count */
  uint8_t lcore_cnt;
  /** active dma dev count for current transport context */
  uint8_t dma_dev_cnt;
  /** if transport device is started(mtl_start) */
  bool dev_started;
};

/**
 * A structure used to retrieve general statistics(I/O) for a MTL port.
 */
struct mtl_port_status {
  /** Total number of received packets. */
  uint64_t rx_packets;
  /** Total number of transmitted packets. */
  uint64_t tx_packets;
  /** Total number of received bytes. */
  uint64_t rx_bytes;
  /** Total number of transmitted bytes. */
  uint64_t tx_bytes;
  /** Total number of failed received packets. */
  uint64_t rx_err_packets;
  /** Total number of received packets dropped by the HW. (i.e. Rx queues are full) */
  uint64_t rx_hw_dropped_packets;
  /** Total number of Rx mbuf allocation failures. */
  uint64_t rx_nombuf_packets;
  /** Total number of failed transmitted packets. */
  uint64_t tx_err_packets;
};

/**
 * Retrieve the fixed information of an MTL instance.
 *
 * @param mt
 *   The handle to MTL instance.
 * @param info
 *   A pointer to info structure.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if fail.
 */
int mtl_get_fix_info(mtl_handle mt, struct mtl_fix_info* info);

/**
 * Retrieve the varied information of an MTL instance.
 *
 * @param mt
 *   The handle to MTL instance.
 * @param info
 *   A pointer to info structure.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if fail.
 */
int mtl_get_var_info(mtl_handle mt, struct mtl_var_info* info);

/**
 * Retrieve the general statistics(I/O) for a MTL port.
 *
 * @param mt
 *   The handle to MTL instance.
 * @param port
 *   The port index.
 * @param stats
 *   A pointer to stats structure.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if fail.
 */
int mtl_get_port_stats(mtl_handle mt, enum mtl_port port, struct mtl_port_status* stats);

/**
 * Reset the general statistics(I/O) for a MTL port.
 *
 * @param mt
 *   The handle to MTL instance.
 * @param port
 *   The port index.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if fail.
 */
int mtl_reset_port_stats(mtl_handle mt, enum mtl_port port);

/**
 * Get the numa socket id for a MTL port.
 *
 * @param mt
 *   The handle to MTL instance.
 * @param port
 *   The port index.
 * @return
 *   >= 0 The numa socket ID.
 *   - <0: Error code if fail.
 */
int mtl_get_numa_id(mtl_handle mt, enum mtl_port port);

/** Helper to set the port for struct mtl_init_params */
int mtl_para_port_set(struct mtl_init_params* p, enum mtl_port port, char* name);
/** Helper to set the sip for struct mtl_init_params */
int mtl_para_sip_set(struct mtl_init_params* p, enum mtl_port port, char* ip);
/** Helper to set the gateway for struct mtl_init_params */
int mtl_para_gateway_set(struct mtl_init_params* p, enum mtl_port port, char* gateway);
/** Helper to set the netmask for struct mtl_init_params */
int mtl_para_netmask_set(struct mtl_init_params* p, enum mtl_port port, char* netmask);
/** Helper to set the dma dev port for struct mtl_init_params */
int mtl_para_dma_port_set(struct mtl_init_params* p, enum mtl_port port, char* name);
/** Helper to set the tx queues number for struct mtl_init_params */
static inline void mtl_para_tx_queues_cnt_set(struct mtl_init_params* p,
                                              enum mtl_port port, uint16_t cnt) {
  p->tx_queues_cnt[port] = cnt;
}
/** Helper to set the rx queues number for struct mtl_init_params */
static inline void mtl_para_rx_queues_cnt_set(struct mtl_init_params* p,
                                              enum mtl_port port, uint16_t cnt) {
  p->rx_queues_cnt[port] = cnt;
}
/** Helper to set the PMD type for struct mtl_init_params */
static inline void mtl_para_pmd_set(struct mtl_init_params* p, enum mtl_port port,
                                    enum mtl_pmd_type pmd) {
  p->pmd[port] = pmd;
}

/** Helper to get the port from struct mtl_init_params */
static inline char* mtl_para_port_get(struct mtl_init_params* p, enum mtl_port port) {
  return p->port[port];
}

/**
 * Inline function returning primary port pointer from mtl_init_params
 * @param p
 *   The pointer to the init parameters.
 * @return
 *     Primary port name pointer
 */
static inline char* mtl_p_port(struct mtl_init_params* p) {
  return mtl_para_port_get(p, MTL_PORT_P);
}

/**
 * Inline function returning redundant port pointer from mtl_init_params
 * @param p
 *   The pointer to the init parameters.
 * @return
 *     Redundant port name pointer
 */
static inline char* mtl_r_port(struct mtl_init_params* p) {
  return mtl_para_port_get(p, MTL_PORT_R);
}

/**
 * Inline helper function returning primary port source IP address pointer
 * from mtl_init_params
 * @param p
 *   The pointer to the init parameters.
 * @return
 *     Primary port IP address pointer
 */
static inline uint8_t* mtl_p_sip_addr(struct mtl_init_params* p) {
  return p->sip_addr[MTL_PORT_P];
}

/**
 * Inline helper function returning redundant port source IP address pointer
 * from mtl_init_params
 * @param p
 *   The pointer to the init parameters.
 * @return
 *     Redundant port IP address pointer
 */
static inline uint8_t* mtl_r_sip_addr(struct mtl_init_params* p) {
  return p->sip_addr[MTL_PORT_R];
}

/**
 * Function returning version string
 * @return
 *     ST version string
 */
const char* mtl_version(void);

/**
 * Initialize the MTL transport device context which based on DPDK.
 *
 * @param p
 *   The pointer to the init parameters.
 * @return
 *   - NULL on error.
 *   - Otherwise, the handle to the MTL transport device context.
 */
mtl_handle mtl_init(struct mtl_init_params* p);

/**
 * Un-initialize the MTL transport device context.
 *
 * @param mt
 *   The handle to the MTL transport device context.
 * @return
 *   - 0: Success, device un-initialized.
 *   - <0: Error code of the device un-initialize.
 */
int mtl_uninit(mtl_handle mt);

/**
 * Start the MTL transport device context.
 *
 * @param mt
 *   The handle to the MTL transport device context.
 * @return
 *   - 0: Success, device started.
 *   - <0: Error code of the device start.
 */
int mtl_start(mtl_handle mt);

/**
 * Stop the MTL transport device context.
 *
 * @param mt
 *   The handle to the MTL transport device context.
 * @return
 *   - 0: Success, device stopped.
 *   - <0: Error code of the device stop.
 */
int mtl_stop(mtl_handle mt);

/**
 * Abort the MTL transport device context.
 * Usually called in the exception case, e.g CTRL-C.
 *
 * @param mt
 *   The handle to the MTL transport device context.
 * @return
 *   - 0: Success, device aborted.
 *   - <0: Error code of the device abort.
 */
int mtl_abort(mtl_handle mt);

/**
 * Set the log level for the MTL transport device context.
 *
 * @param mt
 *   The handle to the MTL transport device context.
 * @param level
 *   The log level define.
 * @return
 *   - 0: Success.
 *   - <0: Error code.
 */
int mtl_set_log_level(mtl_handle mt, enum mtl_log_level level);

/**
 * Get the log level for the MTL transport device context.
 *
 * @param mt
 *   The handle to the MTL transport device context.
 * @return
 *   The log level.
 */
enum mtl_log_level mtl_get_log_level(mtl_handle mt);

/**
 * Set the log prefix user customized formatter.
 *
 * A example formatter is like below:
 *   static void log_default_prefix(char* buf, size_t sz) {
 *     time_t now;
 *     struct tm tm;
 *
 *     time(&now);
 *     localtime_r(&now, &tm);
 *     strftime(buf, sz, "%Y-%m-%d %H:%M:%S, ", &tm);
 *   }
 *
 * @param f
 *   The log formatter func.
 * @return
 *   - 0: Success.
 *   - <0: Error code.
 */
int mtl_set_log_prefix_formatter(mtl_log_prefix_formatter_t f);

/**
 * Set up a custom log printer for the user. By default, MTL uses `RTE_LOG` for outputting
 * logs. However, applications can register a customized log printer using this function.
 * Whenever MTL needs to log anything, the registered log callback will be invoked.
 *
 * A example printer is like below:
 * static void log_user_printer(enum mtl_log_level level, const char* format, ...) {
 *   MTL_MAY_UNUSED(level);
 *   va_list args;
 *
 *   // Init variadic argument list
 *   va_start(args, format);
 *   // Use vprintf to pass the variadic arguments to printf
 *   vprintf(format, args);
 *   // End variadic argument list
 *   va_end(args);
 * }
 *
 * @param mtl_log_printer_t
 *   The user log printer.
 * @return
 *   - 0: Success.
 *   - <0: Error code.
 */
int mtl_set_log_printer(mtl_log_printer_t f);

/**
 * Change the stream that will be used by the logging system.
 *
 * The FILE* f argument represents the stream to be used for logging.
 * If f is NULL, the default output is used. This can be done at any time.
 *
 * @param f
 *   Pointer to the FILE stream.
 * @return
 *   - 0: Success.
 *   - <0: Error code.
 */
int mtl_openlog_stream(FILE* f);

/**
 * Enable or disable sleep mode for sch.
 *
 * @param mt
 *   The handle to the MTL transport device context.
 * @param sch_idx
 *   The sch index, get from st20_tx_get_sch_idx or st20_rx_get_sch_idx.
 * @param enable
 *   enable or not.
 * @return
 *   - 0: Success.
 *   - <0: Error code.
 */
int mtl_sch_enable_sleep(mtl_handle mt, int sch_idx, bool enable);

/**
 * Set the sleep us for the sch if MTL_FLAG_TASKLET_SLEEP is enabled.
 * Debug usage only.
 *
 * @param mt
 *   The handle to the MTL transport device context.
 * @param us
 *   The max sleep us.
 * @return
 *   - 0: Success.
 *   - <0: Error code.
 */
int mtl_sch_set_sleep_us(mtl_handle mt, uint64_t us);

/**
 * Request one DPDK lcore from the MTL transport device context.
 *
 * @param mt
 *   The handle to the MTL transport device context.
 * @param lcore
 *   A pointer to the returned lcore number.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if fail.
 */
int mtl_get_lcore(mtl_handle mt, unsigned int* lcore);

/**
 * Bind one thread to lcore.
 *
 * @param mt
 *   The handle to the MTL transport device context.
 * @param thread
 *   the thread which request the bind action.
 * @param lcore
 *   the DPDK lcore which requested by mtl_get_lcore.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if fail.
 */
int mtl_bind_to_lcore(mtl_handle mt, pthread_t thread, unsigned int lcore);

/**
 * Put back the DPDK lcore which requested from the MTL transport device context.
 *
 * @param mt
 *   The handle to the MTL transport device context.
 * @param lcore
 *   the DPDK lcore which requested by mtl_get_lcore.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if fail.
 */
int mtl_put_lcore(mtl_handle mt, unsigned int lcore);

/**
 * Performance optimized memcpy, e.g. AVX-512.
 *
 * @param dest
 *   Pointer to the destination of the data.
 * @param src
 *   Pointer to the source data.
 * @param n
 *   Number of bytes to copy..
 * @return
 *   - Pointer to the destination data.
 */
void* mtl_memcpy(void* dest, const void* src, size_t n);

/**
 * Read cached time from ptp source.
 *
 * @param mt
 *   The handle to the MTL transport device context.
 * @return
 *   - The time in nanoseconds in current ptp system
 */
uint64_t mtl_ptp_read_time(mtl_handle mt);

/**
 * Read raw time from ptp source.
 *
 * @param mt
 *   The handle to the MTL transport device context.
 * @return
 *   - The time in nanoseconds in current ptp system
 */
uint64_t mtl_ptp_read_time_raw(mtl_handle mt);

/**
 * Allocate memory from the huge-page area of memory. The memory is not cleared.
 * In NUMA systems, the memory allocated from the same NUMA socket of the port.
 * Note the memory is mmap to IOVA already, use mtl_hp_virt2iova to get the iova.
 *
 * @param mt
 *   The handle to the MTL transport device context.
 * @param size
 *   Size (in bytes) to be allocated.
 * @param port
 *   Port for the memory to be allocated.
 * @return
 *   - NULL on error. Not enough memory, or invalid arguments
 *   - Otherwise, the pointer to the allocated memory.
 */
void* mtl_hp_malloc(mtl_handle mt, size_t size, enum mtl_port port);

/**
 * Allocate zero'ed memory from the huge-page area of memory.
 * Equivalent to mtl_hp_malloc() except that the memory zone is cleared with zero.
 * In NUMA systems, the memory allocated from the same NUMA socket of the port.
 * Note the memory is mmap to IOVA already, use mtl_hp_virt2iova to get the iova.
 *
 * @param mt
 *   The handle to the MTL transport device context.
 * @param size
 *   Size (in bytes) to be allocated.
 * @param port
 *   Port for the memory to be allocated.
 * @return
 *   - NULL on error. Not enough memory, or invalid arguments
 *   - Otherwise, the virtual address pointer to the allocated memory.
 */
void* mtl_hp_zmalloc(mtl_handle mt, size_t size, enum mtl_port port);

/**
 * Frees the memory pointed by the pointer.
 *
 * This pointer must have been returned by a previous call to
 * mtl_hp_malloc(), mtl_hp_zmalloc().
 * The behaviour is undefined if the pointer does not match this requirement.
 *
 * @param mt
 *   The handle to the MTL transport device context.
 * @param ptr
 *   The virtual address pointer to memory to be freed.
 */
void mtl_hp_free(mtl_handle mt, void* ptr);

/**
 * Return the IO address of a virtual address from mtl_hp_malloc/mtl_hp_zmalloc
 *
 * @param mt
 *   The handle to the MTL transport device context.
 * @param vaddr
 *   Virtual address obtained from previous mtl_hp_malloc/mtl_hp_zmalloc call
 * @return
 *   MTL_BAD_IOVA on error
 *   otherwise return an address suitable for IO
 */
mtl_iova_t mtl_hp_virt2iova(mtl_handle mt, const void* vaddr);

/**
 * Return the detected page size on the system.
 *
 * @param mt
 *   The handle to the MTL transport device context.
 * @return
 *   page size
 */
size_t mtl_page_size(mtl_handle mt);

/**
 * Sleep with us.
 *
 * @param us
 *   The us to sleep.
 */
void mtl_sleep_us(unsigned int us);

/**
 * Busy delay with us.
 *
 * @param us
 *   The us to sleep.
 */
void mtl_delay_us(unsigned int us);

/**
 * Perform DMA mapping with virtual address that can be used for IO.
 * The virtual address and size must align to page size(mtl_page_size).
 *
 * @param mt
 *   The handle to the MTL transport device context.
 * @param vaddr
 *   Virtual address of memory to be mapped and must align to page size.
 * @param size
 *   Length of memory segment being mapped.
 *
 * @return
 *   MTL_BAD_IOVA on error
 *   otherwise return an address suitable for IO
 */
mtl_iova_t mtl_dma_map(mtl_handle mt, const void* vaddr, size_t size);

/**
 * Perform DMA unmapping on the mtl_dma_map
 *
 * @param mt
 *   The handle to the MTL transport device context.
 * @param vaddr
 *   Virtual address of memory to be unmapped and must align to page size.
 * @param iova
 *   iova address
 * @param size
 *   Length of memory segment being unmapped.
 *
 * @return
 *   - 0 if successful.
 *   - <0: Error code if fail.
 */
int mtl_dma_unmap(mtl_handle mt, const void* vaddr, mtl_iova_t iova, size_t size);

/**
 * Allocate memory block more than required and map valid data to IOVA.
 *
 * The memory malloc layout:
 *
 * |___________|/////////////// valid ////////////////|____|___|
 *
 * |___________|<--------------- size --------------->|____|___|
 *
 * |___________|<---------------- iova_size -------------->|___|
 *
 * |<--------------- alloc_size (pgsz multiple)--------------->|
 *
 * *alloc_addr *addr(page aligned)
 *
 * @param mt
 *   The handle to the MTL transport device context.
 * @param size
 *   Size of valid data.
 * @return
 *   - NULL on error.
 *   - Otherwise, the handle to the dma mem.
 */
mtl_dma_mem_handle mtl_dma_mem_alloc(mtl_handle mt, size_t size);

/**
 * Free the dma mem memory block.
 * This will use memset to clear the st dma mem struct.
 *
 * @param mt
 *   The handle to the MTL transport device context.
 * @param handle
 *   The handle to the st dma mem.
 */
void mtl_dma_mem_free(mtl_handle mt, mtl_dma_mem_handle handle);

/**
 * Get the begin address of dma mapped memory.
 *
 * @param handle
 *   The handle to the st dma mem.
 * @return
 *   - Begin address of dma mapped memory.
 */
void* mtl_dma_mem_addr(mtl_dma_mem_handle handle);

/**
 * Get the begin IOVA of dma mapped memory.
 *
 * @param handle
 *   The handle to the st dma mem.
 * @return
 *   - Begin IOVA of dma mapped memory.
 */
mtl_iova_t mtl_dma_mem_iova(mtl_dma_mem_handle handle);

/**
 * Allocate a user DMA dev from the dma_dev_port(mtl_init_params) list.
 * In NUMA systems, the dma dev allocated from the same NUMA socket of the port.
 *
 * @param mt
 *   The handle to the MTL transport device context.
 * @param nb_desc
 *   Number of descriptor for the user DMA device
 * @param port
 *   Port for the user DMA device to be allocated.
 * @return
 *   - NULL on error.
 *   - Otherwise, the handle to the st user dma dev.
 */
mtl_udma_handle mtl_udma_create(mtl_handle mt, uint16_t nb_desc, enum mtl_port port);

/**
 * Free the st user dma dev.
 *
 * @param handle
 *   The handle to the st user dma dev.
 * @return
 *   - 0: Success.
 *   - <0: Error code of the free.
 */
int mtl_udma_free(mtl_udma_handle handle);

/**
 * Enqueue a copy operation onto the user dma dev.
 *
 * @param handle
 *   The handle to the st user dma dev.
 * @param dst
 *   The mtl_iova_t address of the destination buffer.
 *   Must be the memory address by mtl_hp_virt2iova.
 * @param src
 *   The mtl_iova_t address of the source buffer.
 *   Must be the memory address by mtl_hp_virt2iova.
 * @param length
 *   The length of the data to be copied.
 *
 * @return
 *   - 0..UINT16_MAX: index of enqueued job.
 *   - -ENOSPC: if no space left to enqueue.
 *   - other values < 0 on failure.
 */
int mtl_udma_copy(mtl_udma_handle handle, mtl_iova_t dst, mtl_iova_t src,
                  uint32_t length);

/**
 * Enqueue a fill operation onto the virtual DMA channel.
 *
 * @param handle
 *   The handle to the st user dma dev.
 * @param dst
 *   The mtl_iova_t address of the destination buffer.
 *   Must be the memory address by mtl_hp_virt2iova.
 * @param pattern
 *   The pattern(u64) to populate the destination buffer with.
 * @param length
 *   The length of the data to be copied.
 *
 * @return
 *   - 0..UINT16_MAX: index of enqueued job.
 *   - -ENOSPC: if no space left to enqueue.
 *   - other values < 0 on failure.
 */
int mtl_udma_fill(mtl_udma_handle handle, mtl_iova_t dst, uint64_t pattern,
                  uint32_t length);

/**
 * Enqueue a fill operation onto the virtual DMA channel.
 *
 * @param handle
 *   The handle to the st user dma dev.
 * @param dst
 *   The mtl_iova_t address of the destination buffer.
 *   Must be the memory address by mtl_hp_virt2iova.
 * @param pattern
 *   The pattern(u8) to populate the destination buffer with.
 * @param length
 *   The length of the data to be copied.
 *
 * @return
 *   - 0..UINT16_MAX: index of enqueued job.
 *   - -ENOSPC: if no space left to enqueue.
 *   - other values < 0 on failure.
 */
static inline int mtl_udma_fill_u8(mtl_udma_handle handle, mtl_iova_t dst,
                                   uint8_t pattern, uint32_t length) {
  uint64_t pattern_u64;
  /* pattern to u64 */
  memset(&pattern_u64, pattern, sizeof(pattern_u64));
  return mtl_udma_fill(handle, dst, pattern_u64, length);
}

/**
 * Trigger hardware to begin performing enqueued operations.
 *
 * @param handle
 *   The handle to the st user dma dev.
 * @return
 *   - 0: Success.
 *   - <0: Error code of the submit.
 */
int mtl_udma_submit(mtl_udma_handle handle);

/**
 * Return the number of operations that have been successfully completed.
 *
 * @param handle
 *   The handle to the st user dma dev.
 * @param nb_cpls
 *   The maximum number of completed operations that can be processed.
 *
 * @return
 *   The number of operations that successfully completed. This return value
 *   must be less than or equal to the value of nb_cpls.
 */
uint16_t mtl_udma_completed(mtl_udma_handle handle, const uint16_t nb_cpls);

/**
 * Get the rss mode.
 *
 * @param mt
 *   The handle to the MTL transport device context.
 * @return
 *   - enum mtl_rss_mode.
 */
enum mtl_rss_mode mtl_rss_mode_get(mtl_handle mt);

/**
 * Get the iova mode.
 *
 * @param mt
 *   The handle to the MTL transport device context.
 * @return
 *   - enum mtl_iova_mode.
 */
enum mtl_iova_mode mtl_iova_mode_get(mtl_handle mt);

/**
 * Get the ip info(address, netmask, gateway) for one mtl port.
 *
 * @param handle
 *   The handle to the st user dma dev.
 * @param port
 *   The port.
 * @param ip
 *   The buffer for IP address.
 * @param netmask
 *   The buffer for netmask address.
 * @param gateway
 *   The buffer for gateway address.
 * @return
 *   - 0: Success.
 *   - <0: Error code.
 */
int mtl_port_ip_info(mtl_handle mt, enum mtl_port port, uint8_t ip[MTL_IP_ADDR_LEN],
                     uint8_t netmask[MTL_IP_ADDR_LEN], uint8_t gateway[MTL_IP_ADDR_LEN]);

/**
 * Check if the pmd of one mtl port is dpdk based or not.
 *
 * @param handle
 *   The handle to the st user dma dev.
 * @param port
 *   The port.
 * @return
 *   - true: a DPDK based PMD.
 *   - false: not a DPDK based PMD, MTL_PMD_KERNEL_SOCKET or MTL_PMD_NATIVE_AF_XDP.
 */
bool mtl_pmd_is_dpdk_based(mtl_handle mt, enum mtl_port port);

/**
 * Get SIMD level current cpu supported.
 *
 * @return
 *   - The simd level
 */
enum mtl_simd_level mtl_get_simd_level(void);

/**
 * Get name of CPU simd level
 *
 * @param level
 *     The simd level
 * @return
 *     simd level name
 */
const char* mtl_get_simd_level_name(enum mtl_simd_level level);

/**
 * Helper function to get pmd type by port name.
 *
 * @param port
 *   port name.
 * @return
 *   pmd type.
 */
enum mtl_pmd_type mtl_pmd_by_port_name(const char* port);

/**
 * Helper function to check if it's a af_xdp based pmd.
 *
 * @param pmd
 *   pmd type.
 * @return
 *   true or false.
 */
static inline bool mtl_pmd_is_af_xdp(enum mtl_pmd_type pmd) {
  if (pmd == MTL_PMD_DPDK_AF_XDP || pmd == MTL_PMD_NATIVE_AF_XDP)
    return true;
  else
    return false;
}

/**
 * Helper function to get ip for interface.
 *
 * @param if_name
 *   if name.
 * @param ip
 *   point to IP address.
 * @return
 *   - 0: Success.
 *   - <0: Error code.
 */
int mtl_get_if_ip(char* if_name, uint8_t ip[MTL_IP_ADDR_LEN],
                  uint8_t netmask[MTL_IP_ADDR_LEN]);

/**
 * Set thread names.
 * @param tid
 *   Thread id.
 * @param name
 *   Thread name to set.
 * @return
 *   - 0: Success.
 *   - <0: Error code.
 */
int mtl_thread_setname(pthread_t tid, const char* name);

/**
 * Helper function which align a size with pages
 *
 * @param sz
 *   Input fb size.
 * @param pg_sz
 *   Page size want to aligned.
 * @return
 *     The aligned size.
 */
static inline size_t mtl_size_page_align(size_t sz, size_t pg_sz) {
  if (sz % pg_sz) sz += pg_sz - (sz % pg_sz);
  return sz;
}

/** Helper struct to perform mtl_memcpy with mtl_cpuva_t */
struct mtl_memcpy_ops {
  mtl_cpuva_t dst;
  mtl_cpuva_t src;
  size_t sz;
};
/** Helper function to perform mtl_memcpy with mtl_cpuva_t */
static inline int mtl_memcpy_action(struct mtl_memcpy_ops* ops) {
  mtl_memcpy((void*)ops->dst, (const void*)ops->src, ops->sz);
  return 0;
}

/** Helper function to check if MTL Manager is alive */
bool mtl_is_manager_alive(void);


#if defined(__cplusplus)
}
#endif

#endif
