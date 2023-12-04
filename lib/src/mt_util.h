/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef _MT_LIB_UTIL_HEAD_H_
#define _MT_LIB_UTIL_HEAD_H_

#include "mt_main.h"

static inline bool mt_rtp_len_valid(uint16_t len) {
  if (len <= 0 || len > MTL_PKT_MAX_RTP_BYTES)
    return false;
  else
    return true;
}

/* ip from 224.x.x.x to 239.x.x.x */
static inline bool mt_is_multicast_ip(const uint8_t ip[MTL_IP_ADDR_LEN]) {
  if (ip[0] >= 224 && ip[0] <= 239)
    return true;
  else
    return false;
}

/* if it is a local address area */
static inline bool mt_is_lan_ip(uint8_t ip[MTL_IP_ADDR_LEN], uint8_t sip[MTL_IP_ADDR_LEN],
                                uint8_t netmask[MTL_IP_ADDR_LEN]) {
  for (int i = 0; i < MTL_IP_ADDR_LEN; i++) {
    if ((ip[i] & netmask[i]) != (sip[i] & netmask[i])) return false;
  }

  return true;
}

static inline uint32_t mt_ip_to_u32(uint8_t ip[MTL_IP_ADDR_LEN]) {
  uint32_t group = ((uint32_t)ip[0] << 0);
  group |= ((uint32_t)ip[1] << 8);
  group |= ((uint32_t)ip[2] << 16);
  group |= ((uint32_t)ip[3] << 24);
  return group;
}

static inline void mt_u32_to_ip(uint32_t group, uint8_t ip[MTL_IP_ADDR_LEN]) {
  ip[0] = group >> 0;
  ip[1] = group >> 8;
  ip[2] = group >> 16;
  ip[3] = group >> 24;
}

bool mt_bitmap_test_and_set(uint8_t* bitmap, int idx);
bool mt_bitmap_test(uint8_t* bitmap, int idx);
bool mt_bitmap_test_and_unset(uint8_t* bitmap, int idx);

/* only for mbuf ring with RING_F_SP_ENQ | RING_F_SC_DEQ */
int mt_ring_dequeue_clean(struct rte_ring* ring);

void mt_mbuf_sanity_check(struct rte_mbuf** mbufs, uint16_t nb, char* tag);

int mt_pacing_train_result_add(struct mtl_main_impl* impl, enum mtl_port port,
                               uint64_t rl_bps, float pad_interval);

int mt_pacing_train_result_search(struct mtl_main_impl* impl, enum mtl_port port,
                                  uint64_t rl_bps, float* pad_interval);

int mt_build_port_map(struct mtl_main_impl* impl, char** ports, enum mtl_port* maps,
                      int num_ports);

/* logical session port to main(physical) port */
static inline enum mtl_port mt_port_logic2phy(enum mtl_port* maps,
                                              enum mtl_session_port logic) {
  return maps[logic];
}

void st_video_rtp_dump(enum mtl_port port, int idx, char* tag,
                       struct st20_rfc4175_rtp_hdr* rtp);

void mt_mbuf_dump(enum mtl_port port, int idx, char* tag, struct rte_mbuf* m);
void mt_mbuf_dump_hdr(enum mtl_port port, int idx, char* tag, struct rte_mbuf* m);

void mt_lcore_dump();

void mt_eth_link_dump(uint16_t port_id);

/* 7 bits payload type define in RFC3550 */
static inline bool st_is_valid_payload_type(int payload_type) {
  if (payload_type > 0 && payload_type < 0x7F)
    return true;
  else
    return false;
}

void mt_eth_macaddr_dump(enum mtl_port port, char* tag, struct rte_ether_addr* mac_addr);

static inline bool st_rx_seq_drop(uint16_t new_id, uint16_t old_id, uint16_t delta) {
  if ((new_id <= old_id) && ((old_id - new_id) < delta))
    return true;
  else
    return false;
}

struct rte_mbuf* mt_build_pad(struct mtl_main_impl* impl, struct rte_mempool* mempool,
                              enum mtl_port port, uint16_t ether_type, uint16_t len);

int mt_macaddr_get(struct mtl_main_impl* impl, enum mtl_port port,
                   struct rte_ether_addr* mac_addr);

struct rte_mempool* mt_mempool_create_by_ops(struct mtl_main_impl* impl,
                                             enum mtl_port port, const char* name,
                                             unsigned int n, unsigned int cache_size,
                                             uint16_t priv_size, uint16_t element_size,
                                             const char* ops_name);

static inline struct rte_mempool* mt_mempool_create(
    struct mtl_main_impl* impl, enum mtl_port port, const char* name, unsigned int n,
    unsigned int cache_size, uint16_t priv_size, uint16_t element_size) {
  /* default with stack */
  return mt_mempool_create_by_ops(impl, port, name, n, cache_size, priv_size,
                                  element_size, "stack");
}

static inline struct rte_mempool* mt_mempool_create_common(struct mtl_main_impl* impl,
                                                           enum mtl_port port,
                                                           const char* name,
                                                           unsigned int n) {
  return mt_mempool_create(impl, port, name, n, MT_MBUF_CACHE_SIZE,
                           sizeof(struct mt_muf_priv_data), MT_MBUF_DEFAULT_DATA_SIZE);
}

int mt_mempool_free(struct rte_mempool* mp);

uint16_t mt_rf1071_check_sum(uint8_t* p, size_t len, bool convert);

struct mt_u64_fifo {
  uint64_t* data;
  int write_idx;
  int read_idx;
  int size;
  int used;
};

struct mt_u64_fifo* mt_u64_fifo_init(int size, int soc_id);
int mt_u64_fifo_uinit(struct mt_u64_fifo* fifo);
int mt_u64_fifo_put(struct mt_u64_fifo* fifo, const uint64_t item);
int mt_u64_fifo_get(struct mt_u64_fifo* fifo, uint64_t* item);
int mt_u64_fifo_put_bulk(struct mt_u64_fifo* fifo, const uint64_t* items, uint32_t n);
int mt_u64_fifo_get_bulk(struct mt_u64_fifo* fifo, uint64_t* items, uint32_t n);
int mt_u64_fifo_read_back(struct mt_u64_fifo* fifo, uint64_t* item);
int mt_u64_fifo_read_front(struct mt_u64_fifo* fifo, uint64_t* item);
int mt_u64_fifo_read_any(struct mt_u64_fifo* fifo, uint64_t* item, int skip);
int mt_u64_fifo_read_any_bulk(struct mt_u64_fifo* fifo, uint64_t* items, uint32_t n,
                              int skip);

static inline int mt_u64_fifo_full(struct mt_u64_fifo* fifo) {
  return fifo->used == fifo->size;
}

static inline int mt_u64_fifo_count(struct mt_u64_fifo* fifo) { return fifo->used; }

static inline int mt_u64_fifo_free_count(struct mt_u64_fifo* fifo) {
  return fifo->size - fifo->used;
}

/* only for the mbuf fifo */
int mt_fifo_mbuf_clean(struct mt_u64_fifo* fifo);

struct mt_cvt_dma_ctx {
  struct mt_u64_fifo* fifo;
  int* tran;
  int* done;
};

struct mt_cvt_dma_ctx* mt_cvt_dma_ctx_init(int fifo_size, int soc_id, int type_num);
int mt_cvt_dma_ctx_uinit(struct mt_cvt_dma_ctx* ctx);
int mt_cvt_dma_ctx_push(struct mt_cvt_dma_ctx* ctx, int type);
int mt_cvt_dma_ctx_pop(struct mt_cvt_dma_ctx* ctx);
static inline int mt_cvt_dma_ctx_get_done(struct mt_cvt_dma_ctx* ctx, int type) {
  return ctx->done[type];
}

static inline int mt_cvt_dma_ctx_get_tran(struct mt_cvt_dma_ctx* ctx, int type) {
  return ctx->tran[type];
}

int mt_run_cmd(const char* cmd, char* out, size_t out_len);

int mt_ip_addr_check(uint8_t* ip);

int st_tx_dest_info_check(struct st_tx_dest_info* src, int num_ports);

int st_rx_source_info_check(struct st_rx_source_info* src, int num_ports);

int st_frame_trans_uinit(struct st_frame_trans* frame);

int st_vsync_calculate(struct mtl_main_impl* impl, struct st_vsync_info* vsync);

uint16_t mt_random_port(uint16_t base_port);

static inline const char* mt_string_safe(const char* msg) { return msg ? msg : "null"; }

static inline void mt_mbuf_refcnt_inc_bulk(struct rte_mbuf** mbufs, uint16_t nb) {
  struct rte_mbuf* m = NULL;
  for (uint16_t i = 0; i < nb; i++) {
    m = mbufs[i];
    while (m) {
      rte_mbuf_refcnt_update(m, 1);
      m = m->next;
    }
  }
}

static inline bool mt_udp_matched(const struct mt_rxq_flow* flow,
                                  const struct mt_udp_hdr* hdr) {
  const struct rte_ipv4_hdr* ipv4 = &hdr->ipv4;
  const struct rte_udp_hdr* udp = &hdr->udp;
  bool ip_matched, port_matched;

  if (flow->flags & MT_RXQ_FLOW_F_NO_IP) {
    ip_matched = true;
  } else {
    ip_matched = mt_is_multicast_ip(flow->dip_addr)
                     ? (ipv4->dst_addr == *(uint32_t*)flow->dip_addr)
                     : (ipv4->src_addr == *(uint32_t*)flow->dip_addr);
  }
  if (flow->flags & MT_RXQ_FLOW_F_NO_PORT) {
    port_matched = true;
  } else {
    port_matched = ntohs(udp->dst_port) == flow->dst_port;
  }

  if (ip_matched && port_matched)
    return true;
  else
    return false;
}

#ifdef WINDOWSENV
static inline int mt_fd_set_nonbolck(int fd) {
  MTL_MAY_UNUSED(fd);
  return -ENOTSUP;
}
#else
static inline int mt_fd_set_nonbolck(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  flags |= O_NONBLOCK;
  return fcntl(fd, F_SETFL, flags);
}
#endif

const char* mt_dpdk_afxdp_port2if(const char* port);
const char* mt_dpdk_afpkt_port2if(const char* port);
const char* mt_kernel_port2if(const char* port);
const char* mt_native_afxdp_port2if(const char* port);

int mt_user_info_init(struct mt_user_info* info);

struct mt_cpu_usage {
  uint64_t user;
  uint64_t nice;
  uint64_t system;
  uint64_t idle;
  uint64_t iowait;
  uint64_t irq;
  uint64_t softirq;
  uint64_t steal;
};

int mt_read_cpu_usage(struct mt_cpu_usage* usages, int* cpu_ids, int num_cpus);

double mt_calculate_cpu_usage(struct mt_cpu_usage* prev, struct mt_cpu_usage* curr);

bool mt_file_exists(const char* filename);

int mt_sysfs_write_uint32(const char* path, uint32_t value);

uint32_t mt_softrss(uint32_t* input_tuple, uint32_t input_len);

#endif
