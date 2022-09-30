/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

/**
 * @file st_dpdk_api.h
 *
 * Interfaces to Intel(R) Media Streaming Library
 *
 * This header define the public interfaces of Intel(R) Media Streaming Library
 *
 */

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#ifndef _ST_DPDK_API_HEAD_H_
#define _ST_DPDK_API_HEAD_H_

#if defined(__cplusplus)
extern "C" {
#endif

/**
 * Major version number of Media Streaming Library
 */
#define ST_VERSION_MAJOR (22)
/**
 * Minor version number of Media Streaming Library
 */
#define ST_VERSION_MINOR (12)
/**
 * Last version number of Media Streaming Library
 */
#define ST_VERSION_LAST (0)
/**
 * Macro to compute a version number usable for comparisons
 */
#define ST_VERSION_NUM(a, b, c) ((a) << 16 | (b) << 8 | (c))
/**
 * All version numbers in one to compare with ST_VERSION_NUM()
 */
#define ST_VERSION ST_VERSION_NUM(ST_VERSION_MAJOR, ST_VERSION_MINOR, ST_VERSION_LAST)

/**
 * Get the uint64_t value for a specified bit set(0 to 63).
 */
#define ST_BIT64(nr) (UINT64_C(1) << (nr))

/**
 * Get the uint32_t value for a specified bit set(0 to 31).
 */
#define ST_BIT32(nr) (UINT32_C(1) << (nr))

/**
 * Max length of a DPDK port name
 */
#define ST_PORT_MAX_LEN (64)
/**
 * Length of a IPV4 address
 */
#define ST_IP_ADDR_LEN (4)
/**
 * Defined if current platform is little endian
 */
#define ST_LITTLE_ENDIAN /* x86 use little endian */

/**
 * Max bytes in one RTP packet, include payload and header
 * standard UDP is 1460 bytes, and UDP headers are 8 bytes
 * leave 100 for network extra space
 */
#define ST_PKT_MAX_RTP_BYTES (1460 - 8 - 100)

/**
 * Max allowed number of dma devs
 */
#define ST_DMA_DEV_MAX (8)

/**
 * Max length of a pcap dump file name
 */
#define ST_PCAP_FILE_MAX_LEN (32)

/**
 * Handle to media streaming device context
 */
typedef struct st_main_impl* st_handle;
/**
 * Handle to st user dma device
 */
typedef struct st_dma_lender_dev* st_udma_handle;

/**
 * IO virtual address type.
 */
typedef uint64_t st_iova_t;

/**
 * Handle to dma mem
 */
typedef struct st_dma_mem* st_dma_mem_handle;

/**
 * Bad IOVA address
 */
#define ST_BAD_IOVA ((st_iova_t)-1)

/**
 * Macro to align a value, align should be a power-of-two value.
 */
#define ST_ALIGN(val, align) (((val) + ((align)-1)) & ~((align)-1))

/**
 * Port logical type
 */
enum st_port {
  ST_PORT_P = 0, /**< primary port */
  ST_PORT_R,     /**< redundant port */
  ST_PORT_MAX,   /**< max value of this enum */
};

/**
 * Log level type to media context
 */
enum st_log_level {
  ST_LOG_LEVEL_DEBUG = 0, /**< debug log level */
  ST_LOG_LEVEL_INFO,      /**< info log level */
  ST_LOG_LEVEL_WARNING,   /**< warning log level */
  ST_LOG_LEVEL_ERROR,     /**< error log level */
  ST_LOG_LEVEL_MAX,       /**< max value of this enum */
};

/**
 * Poll mode driver type
 */
enum st_pmd_type {
  /** DPDK user driver PMD */
  ST_PMD_DPDK_USER = 0,
  /** address family(kernel) high performance packet processing */
  ST_PMD_DPDK_AF_XDP,
  /** max value of this enum */
  ST_PMD_TYPE_MAX,
};

/**
 * SIMD level type
 */
enum st_simd_level {
  ST_SIMD_LEVEL_NONE = 0,     /**< Scalar */
  ST_SIMD_LEVEL_AVX2,         /**< AVX2 */
  ST_SIMD_LEVEL_AVX512,       /**< AVX512 */
  ST_SIMD_LEVEL_AVX512_VBMI2, /**< AVX512 VBMI2 */
  ST_SIMD_LEVEL_MAX,          /**< max value of this enum */
};

/**
 * Timestamp type of st2110-10
 */
enum st10_timestamp_fmt {
  /** the media clock time in nanoseconds since the TAI epoch */
  ST10_TIMESTAMP_FMT_TAI = 0,
  /**
   * the raw media clock value defined in ST2110-10, whose units vary by essence
   * sampling rate(90k for video, 48K/96K for audio).
   */
  ST10_TIMESTAMP_FMT_MEDIA_CLK,
  /** max value of this enum */
  ST10_TIMESTAMP_FMT_MAX,
};

/**
 * FPS type of media streaming, Frame per second or Field per second
 */
enum st_fps {
  ST_FPS_P59_94 = 0, /**< 59.94 fps */
  ST_FPS_P50,        /**< 50 fps */
  ST_FPS_P29_97,     /**< 29.97 fps */
  ST_FPS_P25,        /**< 25 fps */
  ST_FPS_P119_88,    /**< 119.88 fps */
  ST_FPS_MAX,        /**< max value of this enum */
};

/**
 * Frame status type of rx streaming
 */
enum st_frame_status {
  /** All pixels of the frame were received */
  ST_FRAME_STATUS_COMPLETE = 0,
  /**
   * There was some packet loss, but the complete frame was reconstructed using packets
   * from primary and redundant streams
   */
  ST_FRAME_STATUS_RECONSTRUCTED,
  /** Packets were lost */
  ST_FRAME_STATUS_CORRUPTED,
  /** Max value of this enum */
  ST_FRAME_STATUS_MAX,
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
  /** Max value of this enum */
  ST21_TX_PACING_WAY_MAX,
};

/**
 * A structure describing rfc3550 rtp header, size: 12
 */
struct st_rfc3550_rtp_hdr {
#ifdef ST_LITTLE_ENDIAN
  /** CSRC count(CC) */
  uint8_t csrc_count : 4;
  /** extension(X) */
  uint8_t extension : 1;
  /** padding(P) */
  uint8_t padding : 1;
  /** version(V) */
  uint8_t version : 2;
  /** payload type(PT) */
  uint8_t payload_type : 7;
  /** marker(M) */
  uint8_t marker : 1;
#else
  /** version(V) */
  uint8_t version : 2;
  /** padding(P) */
  uint8_t padding : 1;
  /** extension(X) */
  uint8_t extension : 1;
  /** CSRC count(CC) */
  uint8_t csrc_count : 4;
  /** marker(M) */
  uint8_t marker : 1;
  /** payload type(PT) */
  uint8_t payload_type : 7;
#endif
  /** sequence number */
  uint16_t seq_number;
  /** timestamp */
  uint32_t tmstamp;
  /** synchronization source */
  uint32_t ssrc;
} __attribute__((__packed__));

/**
 * Flag bit in flags of struct st_init_params.
 * If set, lib will call numa_bind to bind app thread and memory to NIC socket also.
 */
#define ST_FLAG_BIND_NUMA (ST_BIT64(0))
/**
 * Flag bit in flags of struct st_init_params.
 * Enable built-in PTP implementation, only for PF now.
 * If not enable, it will use system time as the PTP source.
 */
#define ST_FLAG_PTP_ENABLE (ST_BIT64(1))
/**
 * Flag bit in flags of struct st_init_params.
 * Separated lcore for RX video(st2110-20/st2110-22) session.
 */
#define ST_FLAG_RX_SEPARATE_VIDEO_LCORE (ST_BIT64(2))
/**
 * Flag bit in flags of struct st_init_params.
 * Enable migrate mode for tx video session if current LCORE is too busy to handle the
 * tx video tasklet, the busy session may be migrated to a new LCORE.
 * If not enable, tx video will always use static mapping based on quota.
 */
#define ST_FLAG_TX_VIDEO_MIGRATE (ST_BIT64(3))
/**
 * Flag bit in flags of struct st_init_params.
 * Enable migrate mode for rx video session if current LCORE is too busy to handle the
 * rx video tasklet, the busy session may be migrated to a new LCORE.
 * If not enable, rx video will always use static mapping based on quota.
 */
#define ST_FLAG_RX_VIDEO_MIGRATE (ST_BIT64(4))
/**
 * Flag bit in flags of struct st_init_params.
 * Run the tasklet inside one thread instead of a pinned lcore.
 */
#define ST_FLAG_TASKLET_THREAD (ST_BIT64(5))
/**
 * Flag bit in flags of struct st_init_params.
 * Enable the tasklet sleep if routine report task done.
 */
#define ST_FLAG_TASKLET_SLEEP (ST_BIT64(6))
/**
 * Flag bit in flags of struct st_init_params.
 * Set the supported SIMD bitwidth of rx/tx burst to 512 bit(AVX512).
 */
#define ST_FLAG_RXTX_SIMD_512 (ST_BIT64(7))

/**
 * Flag bit in flags of struct st_init_params, debug usage only.
 * dedicate thread for cni message
 */
#define ST_FLAG_CNI_THREAD (ST_BIT64(16))
/**
 * Flag bit in flags of struct st_init_params, debug usage only.
 * Enable video rx ebu check
 */
#define ST_FLAG_RX_VIDEO_EBU (ST_BIT64(17))
/**
 * Flag bit in flags of struct st_init_params, debug usage only.
 * Enable NIC promiscuous mode for RX
 */
#define ST_FLAG_NIC_RX_PROMISCUOUS (ST_BIT64(20))
/**
 * Flag bit in flags of struct st_init_params, debug usage only.
 * use unicast address for ptp PTP_DELAY_REQ message
 */
#define ST_FLAG_PTP_UNICAST_ADDR (ST_BIT64(21))
/**
 * Flag bit in flags of struct st_init_params, debug usage only.
 * Mono memory pool for all rx queue(sessions)
 */
#define ST_FLAG_RX_MONO_POOL (ST_BIT64(22))
/**
 * Flag bit in flags of struct st_init_params, debug usage only.
 * Do st_start in st_init, st_stop in st_uninit, and skip the st_start/st_stop
 */
#define ST_FLAG_DEV_AUTO_START_STOP (ST_BIT64(24))
/**
 * Flag bit in flags of struct st_init_params, debug usage only.
 * Enable tasklet time measurement, report status if tasklet run time longer than
 * tasklet_time_thresh_us in st_init_params.
 */
#define ST_FLAG_TASKLET_TIME_MEASURE (ST_BIT64(25))
/**
 * Flag bit in flags of struct st_init_params, debug usage only.
 * Disable the zero copy for af_xdp tx video session
 */
#define ST_FLAG_AF_XDP_ZC_DISABLE (ST_BIT64(26))
/**
 * Flag bit in flags of struct st_init_params, debug usage only.
 * Mono memory pool for all tx queue(session)
 */
#define ST_FLAG_TX_MONO_POOL (ST_BIT64(27))
/**
 * Flag bit in flags of struct st_init_params, debug usage only.
 * Disable system rx queues, pls use mcast or manual TX mac.
 */
#define ST_FLAG_DISABLE_SYSTEM_RX_QUEUES (ST_BIT64(28))

/**
 * The structure describing how to init af_xdp interface.
 * See https://doc.dpdk.org/guides/nics/af_xdp.html for detail.
 */
struct st_af_xdp_params {
  /** starting netdev queue id, must > 0, 0 is reserved for system usage */
  uint8_t start_queue;
  /** total netdev queue number, must > 0 */
  uint8_t queue_count;
};

/**
 * The structure describing how to init the streaming dpdk context.
 * Include the PCIE port and other required info.
 */
struct st_init_params {
  /** Pcie BDF path like 0000:af:00.0 or enp175s0f0(AF_XDP) */
  char port[ST_PORT_MAX][ST_PORT_MAX_LEN];
  /** number of pcie ports, 1 or 2 */
  uint8_t num_ports;
  /** dpdk user pmd or af_xdp */
  enum st_pmd_type pmd[ST_PORT_MAX];
  /**
   * af_xdp port info, only for ST_PMD_DPDK_AF_XDP.
   * ST_PMD_DPDK_AF_XDP will use the IP of kernel itself.
   */
  struct st_af_xdp_params xdp_info[ST_PORT_MAX];
  /** source IP of ports, olny for ST_PMD_DPDK_AF_XDP */
  uint8_t sip_addr[ST_PORT_MAX][ST_IP_ADDR_LEN];
  /**
   * max tx sessions(st20, st22, st30, st40) requested the lib to support,
   * use st_get_cap to query the actual count.
   * dpdk context will allocate the hw resources(queues, memory) based on this number.
   */
  uint16_t tx_sessions_cnt_max;
  /**
   * max rx sessions(st20, st22, st30, st40) requested the lib to support,
   * use st_get_cap to query the actual count.
   * dpdk context will allocate the hw resources(queues, memory) based on this number.
   */
  uint16_t rx_sessions_cnt_max;
  /**
   * logical cores list can be used, e.g. "28,29,30,31".
   * NULL means determined by system itself
   */
  char* lcores;
  /** dma(CBDMA or DSA) dev Pcie BDF path like 0000:80:04.0 */
  char dma_dev_port[ST_DMA_DEV_MAX][ST_PORT_MAX_LEN];
  /** number of dma dev ports in dma_dev_port, leave to zero if no dma dev */
  uint8_t num_dma_dev_port;
  /** log level */
  enum st_log_level log_level;
  /** flags, value in ST_FLAG_* */
  uint64_t flags;
  /** private data to the callback function */
  void* priv;
  /**
   * Function to acquire current ptp time(in nanoseconds) from user.
   * if NULL, ST instance will get from built-in ptp source(NIC) or system time instead.
   */
  uint64_t (*ptp_get_time_fn)(void* priv);
  /** stats dump peroid in seconds, 0 means determined by lib */
  uint16_t dump_period_s;
  /** stats dump callabck in every dump_period_s */
  void (*stat_dump_cb_fn)(void* priv);
  /** data quota for each lcore, 0 means determined by lib */
  uint32_t data_quota_mbs_per_sch;
  /**
   * number of transmit descriptors for each NIC TX queue, 0 means determined by lib.
   * It will affect the memory usage and the performance.
   */
  uint16_t nb_tx_desc;
  /**
   * number of receive descriptors for each NIC RX queue, 0 means determined by lib.
   * It will affect the memory usage and the performance.
   */
  uint16_t nb_rx_desc;
  /**
   * Suggest max allowed udp size for each network pkt, leave to zero if you don't known.
   */
  uint16_t pkt_udp_suggest_max_size;
  /**
   * The number for hdr split queues of rx, should smaller than rx_sessions_cnt_max.
   * Experimental feature.
   */
  uint16_t nb_rx_hdr_split_queues;
  /**
   * Suggest data room size for rx mempool,
   * the final data room size may be aligned to larger value,
   * some NICs may need this to avoid mbuf split.
   */
  uint16_t rx_pool_data_size;
  /**
   * The st21 tx pacing way, leave to zero(auto) if you don't known the detail.
   */
  enum st21_tx_pacing_way pacing;
};

/**
 * The structure describing the source address(ip addr and port) info for RX.
 * Leave redundant info to zero if the session only has primary port.
 */
struct st_rx_source_info {
  /** source IP address of sender */
  uint8_t sip_addr[ST_PORT_MAX][ST_IP_ADDR_LEN];
  /** UDP port number */
  uint16_t udp_port[ST_PORT_MAX];
};

/**
 * A structure used to retrieve capacity for an ST instance.
 */
struct st_cap {
  /** max tx session count for current streaming context */
  uint16_t tx_sessions_cnt_max;
  /** max rx session count for current streaming context */
  uint16_t rx_sessions_cnt_max;
  /** max dma dev count for current streaming context */
  uint8_t dma_dev_cnt_max;
  /** the flags in st_init_params */
  uint64_t init_flags;
};

/**
 * A structure used to retrieve state for an ST instance.
 */
struct st_stats {
  /** st20 tx session count in current streaming context */
  uint16_t st20_tx_sessions_cnt;
  /** st22 tx session count in current streaming context */
  uint16_t st22_tx_sessions_cnt;
  /** st30 tx session count in current streaming context */
  uint16_t st30_tx_sessions_cnt;
  /** st40 tx session count in current streaming context */
  uint16_t st40_tx_sessions_cnt;
  /** st20 rx session count in current streaming context */
  uint16_t st20_rx_sessions_cnt;
  /** st22 rx session count in current streaming context */
  uint16_t st22_rx_sessions_cnt;
  /** st30 rx session count in current streaming context */
  uint16_t st30_rx_sessions_cnt;
  /** st40 rx session count in current streaming context */
  uint16_t st40_rx_sessions_cnt;
  /** active scheduler count in current streaming context */
  uint8_t sch_cnt;
  /** active lcore count in current streaming context */
  uint8_t lcore_cnt;
  /** active dma dev count for current streaming context */
  uint8_t dma_dev_cnt;
  /** if streaming device is started(st_start) */
  uint8_t dev_started;
};

/**
 * Pcap dump meta data for synchronous st**_rx_pcapng_dump.
 */
struct st_pcap_dump_meta {
  /** file path for the pcap dump file */
  char file_name[ST_PCAP_FILE_MAX_LEN];
  /** number of packets dumped */
  uint32_t dumped_packets;
};

/**
 * The structure describing queue info attached to one session.
 */
struct st_queue_meta {
  /** 1 or 2, num of ports this session attached to */
  uint8_t num_port;
  /** starting netdev queue id */
  uint8_t start_queue[ST_PORT_MAX];
  /** queue id this session attached to */
  uint8_t queue_id[ST_PORT_MAX];
};

/**
 * Inline function returning primary port pointer from st_init_params
 * @param p
 *   The pointer to the init parameters.
 * @return
 *     Primary port name pointer
 */
static inline char* st_p_port(struct st_init_params* p) { return p->port[ST_PORT_P]; }

/**
 * Inline function returning redundant port pointer from st_init_params
 * @param p
 *   The pointer to the init parameters.
 * @return
 *     Redundant port name pointer
 */
static inline char* st_r_port(struct st_init_params* p) { return p->port[ST_PORT_R]; }

/**
 * Inline helper function returning primary port source IP address pointer
 * from st_init_params
 * @param p
 *   The pointer to the init parameters.
 * @return
 *     Primary port IP address pointer
 */
static inline uint8_t* st_p_sip_addr(struct st_init_params* p) {
  return p->sip_addr[ST_PORT_P];
}

/**
 * Inline helper function returning redundant port source IP address pointer
 * from st_init_params
 * @param p
 *   The pointer to the init parameters.
 * @return
 *     Redundant port IP address pointer
 */
static inline uint8_t* st_r_sip_addr(struct st_init_params* p) {
  return p->sip_addr[ST_PORT_R];
}

/**
 * Function returning version string
 * @return
 *     ST version string
 */
const char* st_version(void);

/**
 * Initialize the media streaming device context which based on DPDK.
 *
 * @param p
 *   The pointer to the init parameters.
 * @return
 *   - NULL on error.
 *   - Otherwise, the handle to the media streaming device context.
 */
st_handle st_init(struct st_init_params* p);

/**
 * Un-initialize the media streaming device context.
 *
 * @param st
 *   The handle to the media streaming device context.
 * @return
 *   - 0: Success, device un-initialized.
 *   - <0: Error code of the device un-initialize.
 */
int st_uninit(st_handle st);

/**
 * Start the media streaming device context.
 *
 * @param st
 *   The handle to the media streaming device context.
 * @return
 *   - 0: Success, device started.
 *   - <0: Error code of the device start.
 */
int st_start(st_handle st);

/**
 * Stop the media streaming device context.
 *
 * @param st
 *   The handle to the media streaming device context.
 * @return
 *   - 0: Success, device stopped.
 *   - <0: Error code of the device stop.
 */
int st_stop(st_handle st);

/**
 * Abort the media streaming device context.
 * Usually called in the exception case, e.g CTRL-C.
 *
 * @param st
 *   The handle to the media streaming device context.
 * @return
 *   - 0: Success, device aborted.
 *   - <0: Error code of the device abort.
 */
int st_request_exit(st_handle st);

/**
 * Retrieve the capacity of the media streaming device context.
 *
 * @param st
 *   The handle to the media streaming device context.
 * @param cap
 *   A pointer to a structure of type *st_cap* to be filled.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if fail.
 */
int st_get_cap(st_handle st, struct st_cap* cap);

/**
 * Retrieve the stat info of the media streaming device context.
 *
 * @param st
 *   The handle to the media streaming device context.
 * @param stats
 *   A pointer to a structure of type *st_stats* to be filled.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if fail.
 */
int st_get_stats(st_handle st, struct st_stats* stats);

/**
 * Request one DPDK lcore from the media streaming device context.
 *
 * @param st
 *   The handle to the media streaming device context.
 * @param lcore
 *   A pointer to the retured lcore number.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if fail.
 */
int st_get_lcore(st_handle st, unsigned int* lcore);

/**
 * Bind one thread to lcore.
 *
 * @param st
 *   The handle to the media streaming device context.
 * @param thread
 *   the thread wchich request the bind action.
 * @param lcore
 *   the DPDK lcore which requested by st_get_lcore.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if fail.
 */
int st_bind_to_lcore(st_handle st, pthread_t thread, unsigned int lcore);

/**
 * Put back the DPDK lcore which requested from the media streaming device context.
 *
 * @param st
 *   The handle to the media streaming device context.
 * @param lcore
 *   the DPDK lcore which requested by st_get_lcore.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if fail.
 */
int st_put_lcore(st_handle st, unsigned int lcore);

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
void* st_memcpy(void* dest, const void* src, size_t n);

/**
 * Allocate memory from the huge-page area of memory. The memory is not cleared.
 * In NUMA systems, the memory allocated from the same NUMA socket of the port.
 * Note the mmeory is mmap to IOVA already, use st_hp_virt2iova to get the iova.
 *
 * @param st
 *   The handle to the media streaming device context.
 * @param size
 *   Size (in bytes) to be allocated.
 * @param port
 *   Port for the memory to be allocated.
 * @return
 *   - NULL on error. Not enough memory, or invalid arguments
 *   - Otherwise, the pointer to the allocated memory.
 */
void* st_hp_malloc(st_handle st, size_t size, enum st_port port);

/**
 * Allocate zero'ed memory from the huge-page area of memory.
 * Equivalent to st_hp_malloc() except that the memory zone is cleared with zero.
 * In NUMA systems, the memory allocated from the same NUMA socket of the port.
 * Note the mmeory is mmap to IOVA already, use st_hp_virt2iova to get the iova.
 *
 * @param st
 *   The handle to the media streaming device context.
 * @param size
 *   Size (in bytes) to be allocated.
 * @param port
 *   Port for the memory to be allocated.
 * @return
 *   - NULL on error. Not enough memory, or invalid arguments
 *   - Otherwise, the virtual address pointer to the allocated memory.
 */
void* st_hp_zmalloc(st_handle st, size_t size, enum st_port port);

/**
 * Frees the memory pointed by the pointer.
 *
 * This pointer must have been returned by a previous call to
 * st_hp_malloc(), st_hp_zmalloc().
 * The behaviour is undefined if the pointer does not match this requirement.
 *
 * @param st
 *   The handle to the media streaming device context.
 * @param ptr
 *   The virtual address pointer to memory to be freed.
 */
void st_hp_free(st_handle st, void* ptr);

/**
 * Return the IO address of a virtual address from st_hp_malloc/st_hp_zmalloc
 *
 * @param st
 *   The handle to the media streaming device context.
 * @param vaddr
 *   Virtual address obtained from previous st_hp_malloc/st_hp_zmalloc call
 * @return
 *   ST_BAD_IOVA on error
 *   otherwise return an address suitable for IO
 */
st_iova_t st_hp_virt2iova(st_handle st, const void* vaddr);

/**
 * Return the detected page size on the system.
 *
 * @param st
 *   The handle to the media streaming device context.
 * @return
 *   page size
 */
size_t st_page_size(st_handle st);

/**
 * Perform DMA mapping with virtual address that can be used for IO.
 * The virtual address and size must align to page size(st_page_size).
 *
 * @param st
 *   The handle to the media streaming device context.
 * @param vaddr
 *   Virtual address of memory to be mapped and must align to page size.
 * @param size
 *   Length of memory segment being mapped.
 *
 * @return
 *   ST_BAD_IOVA on error
 *   otherwise return an address suitable for IO
 */
st_iova_t st_dma_map(st_handle st, const void* vaddr, size_t size);

/**
 * Perform DMA unmapping on the st_dma_map
 *
 * @param st
 *   The handle to the media streaming device context.
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
int st_dma_unmap(st_handle st, const void* vaddr, st_iova_t iova, size_t size);

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
 * @param st
 *   The handle to the media streaming device context.
 * @param size
 *   Size of valid data.
 * @return
 *   - NULL on error.
 *   - Otherwise, the handle to the dma mem.
 */
st_dma_mem_handle st_dma_mem_alloc(st_handle st, size_t size);

/**
 * Free the dma mem memory block.
 * This will use memset to clear the st dma mem struct.
 *
 * @param st
 *   The handle to the media streaming device context.
 * @param handle
 *   The handle to the st dma mem.
 */
void st_dma_mem_free(st_handle st, st_dma_mem_handle handle);

/**
 * Get the begin address of dma mapped memory.
 *
 * @param handle
 *   The handle to the st dma mem.
 * @return
 *   - Begin address of dma mapped memory.
 */
void* st_dma_mem_addr(st_dma_mem_handle handle);

/**
 * Get the begin IOVA of dma mapped memory.
 *
 * @param handle
 *   The handle to the st dma mem.
 * @return
 *   - Begin IOVA of dma mapped memory.
 */
st_iova_t st_dma_mem_iova(st_dma_mem_handle handle);

/**
 * Allocate a user DMA dev from the dma_dev_port(st_init_params) list.
 * In NUMA systems, the dma dev allocated from the same NUMA socket of the port.
 *
 * @param st
 *   The handle to the media streaming device context.
 * @param nb_desc
 *   Number of descriptor for the user DMA device
 * @param port
 *   Port for the user DMA device to be allocated.
 * @return
 *   - NULL on error.
 *   - Otherwise, the handle to the st user dma dev.
 */
st_udma_handle st_udma_create(st_handle st, uint16_t nb_desc, enum st_port port);

/**
 * Free the st user dma dev.
 *
 * @param handle
 *   The handle to the st user dma dev.
 * @return
 *   - 0: Success.
 *   - <0: Error code of the free.
 */
int st_udma_free(st_udma_handle handle);

/**
 * Enqueue a copy operation onto the user dma dev.
 *
 * @param handle
 *   The handle to the st user dma dev.
 * @param dst
 *   The st_iova_t address of the destination buffer.
 *   Must be the memory address by st_hp_virt2iova.
 * @param src
 *   The st_iova_t address of the source buffer.
 *   Must be the memory address by st_hp_virt2iova.
 * @param length
 *   The length of the data to be copied.
 *
 * @return
 *   - 0..UINT16_MAX: index of enqueued job.
 *   - -ENOSPC: if no space left to enqueue.
 *   - other values < 0 on failure.
 */
int st_udma_copy(st_udma_handle handle, st_iova_t dst, st_iova_t src, uint32_t length);

/**
 * Enqueue a fill operation onto the virtual DMA channel.
 *
 * @param handle
 *   The handle to the st user dma dev.
 * @param dst
 *   The st_iova_t address of the destination buffer.
 *   Must be the memory address by st_hp_virt2iova.
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
int st_udma_fill(st_udma_handle handle, st_iova_t dst, uint64_t pattern, uint32_t length);

/**
 * Enqueue a fill operation onto the virtual DMA channel.
 *
 * @param handle
 *   The handle to the st user dma dev.
 * @param dst
 *   The st_iova_t address of the destination buffer.
 *   Must be the memory address by st_hp_virt2iova.
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
static inline int st_udma_fill_u8(st_udma_handle handle, st_iova_t dst, uint8_t pattern,
                                  uint32_t length) {
  uint64_t pattern_u64;
  /* pattern to u64 */
  memset(&pattern_u64, pattern, sizeof(pattern_u64));
  return st_udma_fill(handle, dst, pattern_u64, length);
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
int st_udma_submit(st_udma_handle handle);

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
uint16_t st_udma_completed(st_udma_handle handle, const uint16_t nb_cpls);

/**
 * Read current time from ptp source.
 *
 * @param st
 *   The handle to the media streaming device context.
 * @return
 *   - The time in nanoseconds in current ptp system
 */
uint64_t st_ptp_read_time(st_handle st);

/**
 * Get SIMD level current cpu supported.
 *
 * @return
 *   - The simd level
 */
enum st_simd_level st_get_simd_level(void);

/**
 * Get name of CPU simd level
 *
 * @param level
 *     The simd level
 * @return
 *     simd level name
 */
const char* st_get_simd_level_name(enum st_simd_level level);

/**
 * Inline function to check the  rx frame is a completed frame.
 * @param status
 *   The input frame status.
 * @return
 *     Complete or not.
 */
static inline bool st_is_frame_complete(enum st_frame_status status) {
  if ((status == ST_FRAME_STATUS_COMPLETE) || (status == ST_FRAME_STATUS_RECONSTRUCTED))
    return true;
  else
    return false;
}

/**
 * Helper function returning accurate frame rate from enum st_fps
 * @param fps
 *   enum st_fps fps.
 * @return
 *   frame rate number
 */
double st_frame_rate(enum st_fps fps);

/**
 * Helper function to convert ST10_TIMESTAMP_FMT_TAI to ST10_TIMESTAMP_FMT_MEDIA_CLK.
 *
 * @param tai_ns
 *   time in nanoseconds since the TAI epoch.
 * @param sampling_rate
 *   sampling rate(90k for video, 48K/96K for audio).
 * @return
 *   the raw media clock value defined in ST2110-10, whose units vary by sampling_rate
 */
uint32_t st10_tai_to_media_clk(uint64_t tai_ns, uint32_t sampling_rate);

/**
 * Helper function to convert ST10_TIMESTAMP_FMT_MEDIA_CLK to nanoseconds.
 *
 * @param media_ts
 *   the raw media clock value defined in ST2110-10, whose units vary by sampling_rate.
 * @param sampling_rate
 *   sampling rate(90k for video, 48K/96K for audio).
 * @return
 *   time in nanoseconds.
 */
uint64_t st10_media_clk_to_ns(uint32_t media_ts, uint32_t sampling_rate);

/**
 * Helper function to get pmd type by port name.
 *
 * @param port
 *   port name.
 * @return
 *   pmd type.
 */
enum st_pmd_type st_pmd_by_port_name(const char* port);

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
int st_get_if_ip(char* if_name, uint8_t ip[ST_IP_ADDR_LEN]);

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
static inline size_t st_size_page_align(size_t sz, size_t pg_sz) {
  if (sz % pg_sz) sz += pg_sz - (sz % pg_sz);
  return sz;
}

#if defined(__cplusplus)
}
#endif

#endif
