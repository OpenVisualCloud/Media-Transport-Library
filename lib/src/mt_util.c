/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "mt_util.h"

#ifndef WINDOWSENV
#include <pwd.h>
#endif

#include "datapath/mt_queue.h"
#include "mt_log.h"
#include "mt_main.h"

#ifdef MTL_GPU_DIRECT_ENABLED
#include <mtl_gpu_direct/gpu.h>
#endif /* MTL_GPU_DIRECT_ENABLED */

#ifdef MTL_HAS_ASAN
#include <execinfo.h>

#define MAX_BT_SIZE 32
/* backtrace info */
struct mt_backtrace_info {
  void* pointer;
  size_t size;
  char** bt_strings;
  int bt_size;
  MT_TAILQ_ENTRY(mt_backtrace_info) next;
};

/* List of backtrace info */
MT_TAILQ_HEAD(mt_backtrace_info_list, mt_backtrace_info);
static struct mt_backtrace_info_list g_bt_head;
static pthread_mutex_t g_bt_mutex;

/* additional memleak check for mempool_create since dpdk asan not support this */
static int g_mt_mempool_create_cnt;

int mt_asan_init(void) {
  mt_pthread_mutex_init(&g_bt_mutex, NULL);
  MT_TAILQ_INIT(&g_bt_head);
  return 0;
}

int mt_asan_check(void) {
  /* check if any not freed */
  int leak_cnt = 0;
  struct mt_backtrace_info* bt_info = NULL;
  while ((bt_info = MT_TAILQ_FIRST(&g_bt_head))) {
    info("%s, \033[31mleak of %" PRIu64 " byte(s) at %p\033[0m\n", __func__,
         bt_info->size, bt_info->pointer);
    if (bt_info->bt_strings) {
      info("%s, backtrace info:\n", __func__);
      for (int i = 0; i < bt_info->bt_size; ++i) {
        info("%s, %s\n", __func__, bt_info->bt_strings[i]);
      }
      mt_free(bt_info->bt_strings);
      bt_info->bt_strings = NULL;
    }
    MT_TAILQ_REMOVE(&g_bt_head, bt_info, next);
    rte_free(bt_info->pointer); /* free the leak from rte_malloc */
    mt_free(bt_info);
    leak_cnt++;
  }
  if (leak_cnt)
    info("%s, \033[33mfound %d rte_malloc leak(s) in total\033[0m\n", __func__, leak_cnt);

  mt_pthread_mutex_destroy(&g_bt_mutex);

  if (g_mt_mempool_create_cnt != 0) {
    err("%s, detect not free mempool, leak cnt %d\n", __func__, g_mt_mempool_create_cnt);
  }

  return 0;
}

void* mt_rte_malloc_socket(size_t sz, int socket) {
  void* p = rte_malloc_socket(MT_DPDK_LIB_NAME, sz, RTE_CACHE_LINE_SIZE, socket);
  if (p) {
    /* insert bt_info to list */
    struct mt_backtrace_info* bt_info = mt_zmalloc(sizeof(*bt_info));
    if (bt_info) {
      bt_info->pointer = p;
      bt_info->size = sz;
      void* buffer[MAX_BT_SIZE];
      bt_info->bt_size = backtrace(buffer, MAX_BT_SIZE);
      bt_info->bt_strings = backtrace_symbols(buffer, bt_info->bt_size);
      mt_pthread_mutex_lock(&g_bt_mutex);
      MT_TAILQ_INSERT_TAIL(&g_bt_head, bt_info, next);
      mt_pthread_mutex_unlock(&g_bt_mutex);
    }
  }
  return p;
}

void* mt_rte_zmalloc_socket(size_t sz, int socket) {
  void* p = rte_zmalloc_socket(MT_DPDK_LIB_NAME, sz, RTE_CACHE_LINE_SIZE, socket);
  if (p) {
    /* insert bt_info to list */
    struct mt_backtrace_info* bt_info = mt_zmalloc(sizeof(*bt_info));
    if (bt_info) {
      bt_info->pointer = p;
      bt_info->size = sz;
      void* buffer[MAX_BT_SIZE];
      bt_info->bt_size = backtrace(buffer, MAX_BT_SIZE);
      bt_info->bt_strings = backtrace_symbols(buffer, bt_info->bt_size);
      mt_pthread_mutex_lock(&g_bt_mutex);
      MT_TAILQ_INSERT_TAIL(&g_bt_head, bt_info, next);
      mt_pthread_mutex_unlock(&g_bt_mutex);
    }
  }
  return p;
}

void mt_rte_free(void* p) {
  /* remove bt_info from list */
  struct mt_backtrace_info *bt_info, *tmp_bt_info;
  mt_pthread_mutex_lock(&g_bt_mutex);
  for (bt_info = MT_TAILQ_FIRST(&g_bt_head); bt_info != NULL; bt_info = tmp_bt_info) {
    tmp_bt_info = MT_TAILQ_NEXT(bt_info, next);
    if (bt_info->pointer == p) {
      MT_TAILQ_REMOVE(&g_bt_head, bt_info, next);
      if (bt_info->bt_strings) mt_free(bt_info->bt_strings);
      mt_free(bt_info);
      break;
    }
  }
  mt_pthread_mutex_unlock(&g_bt_mutex);
  if (bt_info == NULL) { /* not found */
    err("%s, \033[31m%p already freed\033[0m\n", __func__, p);
  }
  rte_free(p);
}
#endif

bool mt_bitmap_test(uint8_t* bitmap, int idx) {
  int pos = idx / 8;
  int off = idx % 8;
  uint8_t bits = bitmap[pos];

  return (bits & (0x1 << off)) ? true : false;
}

bool mt_bitmap_test_and_set(uint8_t* bitmap, int idx) {
  int pos = idx / 8;
  int off = idx % 8;
  uint8_t bits = bitmap[pos];

  /* already set */
  if (bits & (0x1 << off)) return true;

  /* set the bit */
  bitmap[pos] = bits | (0x1 << off);
  return false;
}

bool mt_bitmap_test_and_unset(uint8_t* bitmap, int idx) {
  int pos = idx / 8;
  int off = idx % 8;
  uint8_t bits = bitmap[pos];

  /* already unset */
  if (!(bits & (0x1 << off))) return true;

  /* unset the bit */
  bitmap[pos] = bits & (UINT8_MAX ^ (0x1 << off));
  return false;
}

int mt_ring_dequeue_clean(struct rte_ring* ring) {
  int ret;
  struct rte_mbuf* pkt;
  unsigned int count = rte_ring_count(ring);

  if (count) info("%s, count %d for ring %s\n", __func__, count, ring->name);
  /* dequeue and free all mbufs in the ring */
  do {
    ret = rte_ring_sc_dequeue(ring, (void**)&pkt);
    if (ret < 0) break;
    rte_pktmbuf_free(pkt);
  } while (1);

  dbg("%s, end\n", __func__);
  return 0;
}

void mt_mbuf_sanity_check(struct rte_mbuf** mbufs, uint16_t nb, char* tag) {
  struct rte_mbuf* mbuf;

  for (int i = 0; i < nb; i++) {
    mbuf = mbufs[i];
    if ((mbuf->pkt_len < 60) || (mbuf->nb_segs > 2) || (mbuf->pkt_len > 1514)) {
      err("%s(%s), fail on %d len %d nb_segs %d\n", __func__, tag ? tag : "", i,
          mbuf->pkt_len, mbuf->nb_segs);
    }
  }
}

enum mtl_port mt_port_by_name(struct mtl_main_impl* impl, const char* name) {
  struct mtl_init_params* p = mt_get_user_params(impl);
  int main_num_ports = p->num_ports;

  if (!name) {
    err("%s, name is NULL\n", __func__);
    return MTL_PORT_MAX;
  }

  for (enum mtl_port i = 0; i < main_num_ports; i++) {
    if (0 == strncmp(p->port[i], name, MTL_PORT_MAX_LEN)) {
      return i;
    }
  }

  err("%s, %s is not valid\n", __func__, name);
  return MTL_PORT_MAX;
}

int mt_build_port_map(struct mtl_main_impl* impl, char** ports, enum mtl_port* maps,
                      int num_ports) {
  struct mtl_init_params* p = mt_get_user_params(impl);
  int main_num_ports = p->num_ports;

  if (num_ports > main_num_ports) {
    err("%s, invalid num_ports %d\n", __func__, num_ports);
    return -EIO;
  }

  for (int i = 0; i < num_ports; i++) {
    int j;
    for (j = 0; j < main_num_ports; j++) {
      if (0 == strncmp(p->port[j], ports[i], MTL_PORT_MAX_LEN)) {
        maps[i] = j;
        break;
      }
    }

    if (j >= main_num_ports) {
      err("%s(%d), invalid port %s\n", __func__, i, ports[i]);
      return -EIO;
    }
  }

  if (num_ports > 1) {
    if (maps[0] == maps[1]) {
      err("%s, map to same port %d(%s)\n", __func__, maps[0], ports[0]);
      return -EIO;
    }
  }

  return 0;
}

int mt_pacing_train_pad_result_add(struct mtl_main_impl* impl, enum mtl_port port,
                                   uint64_t input_bps, float pad_interval) {
  struct mt_pacing_train_result* ptr = &mt_if(impl, port)->pt_results[0];

  for (int i = 0; i < MT_MAX_RL_ITEMS; i++) {
    if (ptr[i].input_bps) continue;
    ptr[i].input_bps = input_bps;
    ptr[i].pacing_pad_interval = pad_interval;
    return 0;
  }

  err("%s(%d), no space\n", __func__, port);
  return -ENOMEM;
}

int mt_pacing_train_pad_result_search(struct mtl_main_impl* impl, enum mtl_port port,
                                      uint64_t rl_bps, float* pad_interval) {
  struct mt_pacing_train_result* ptr = &mt_if(impl, port)->pt_results[0];

  for (int i = 0; i < MT_MAX_RL_ITEMS; i++) {
    if (rl_bps == ptr[i].input_bps && ptr[i].pacing_pad_interval) {
      *pad_interval = ptr[i].pacing_pad_interval;
      return 0;
    }
  }

  dbg("%s(%d), no entry for %" PRIu64 "\n", __func__, port, rl_bps);
  return -EINVAL;
}

int mt_pacing_train_bps_result_add(struct mtl_main_impl* impl, enum mtl_port port,
                                   uint64_t input_bps, uint64_t profiled_bps) {
  struct mt_pacing_train_result* ptr = &mt_if(impl, port)->pt_results[0];

  for (int i = 0; i < MT_MAX_RL_ITEMS; i++) {
    if (ptr[i].input_bps) continue;
    ptr[i].input_bps = input_bps;
    ptr[i].profiled_bps = profiled_bps;
    return 0;
  }

  err("%s(%d), no space\n", __func__, port);
  return -ENOMEM;
}

int mt_pacing_train_bps_result_search(struct mtl_main_impl* impl, enum mtl_port port,
                                      uint64_t input_bps, uint64_t* profiled_bps) {
  struct mt_pacing_train_result* ptr = &mt_if(impl, port)->pt_results[0];

  for (int i = 0; i < MT_MAX_RL_ITEMS; i++) {
    if (input_bps == ptr[i].input_bps && ptr[i].profiled_bps) {
      *profiled_bps = ptr[i].profiled_bps;
      return 0;
    }
  }

  dbg("%s(%d), no entry for %" PRIu64 "\n", __func__, port, input_bps);
  return -EINVAL;
}

void st_video_rtp_dump(enum mtl_port port, int idx, char* tag,
                       struct st20_rfc4175_rtp_hdr* rtp) {
  uint16_t line1_number = ntohs(rtp->row_number);
  uint16_t line1_offset = ntohs(rtp->row_offset);
  uint16_t line1_length = ntohs(rtp->row_length);
  uint32_t tmstamp = ntohl(rtp->base.tmstamp);
  uint32_t seq_id = (uint32_t)ntohs(rtp->base.seq_number) |
                    (((uint32_t)ntohs(rtp->seq_number_ext)) << 16);
  struct st20_rfc4175_extra_rtp_hdr* extra_rtp = NULL;

  if (line1_offset & ST20_SRD_OFFSET_CONTINUATION) {
    line1_offset &= ~ST20_SRD_OFFSET_CONTINUATION;
    extra_rtp = (struct st20_rfc4175_extra_rtp_hdr*)&rtp[1];
  }

  if (tag) info("%s(%d,%d), %s\n", __func__, port, idx, tag);
  info("tmstamp: 0x%x, seq_id: %u\n", tmstamp, seq_id);
  info("line: no %d offset %d len %d\n", line1_number, line1_offset, line1_length);
  if (extra_rtp) {
    uint16_t line2_number = ntohs(extra_rtp->row_number);
    uint16_t line2_offset = ntohs(extra_rtp->row_offset);
    uint16_t line2_length = ntohs(extra_rtp->row_length);

    info("extra line: no %d offset %d len %d\n", line2_number, line2_offset,
         line2_length);
  }
}

void mt_mbuf_dump_hdr(enum mtl_port port, int idx, char* tag, struct rte_mbuf* m) {
  struct rte_ether_hdr* eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr*);
  size_t hdr_offset = sizeof(struct rte_ether_hdr);
  struct rte_ipv4_hdr* ipv4 = NULL;
  struct rte_udp_hdr* udp = NULL;
  uint16_t ether_type = ntohs(eth->ether_type);
  uint8_t* mac;
  uint8_t* ip;

  if (tag) info("%s(%d,%d), %s\n", __func__, port, idx, tag);
  info("ether_type 0x%x\n", ether_type);
  mac = &mt_eth_d_addr(eth)->addr_bytes[0];
  info("d_mac %02x:%02x:%02x:%02x:%02x:%02x\n", mac[0], mac[1], mac[2], mac[3], mac[4],
       mac[5]);
  mac = &mt_eth_s_addr(eth)->addr_bytes[0];
  info("s_mac %02x:%02x:%02x:%02x:%02x:%02x\n", mac[0], mac[1], mac[2], mac[3], mac[4],
       mac[5]);

  if (ether_type == RTE_ETHER_TYPE_IPV4) {
    ipv4 = rte_pktmbuf_mtod_offset(m, struct rte_ipv4_hdr*, hdr_offset);
    hdr_offset += sizeof(*ipv4);
    udp = rte_pktmbuf_mtod_offset(m, struct rte_udp_hdr*, hdr_offset);
    hdr_offset += sizeof(*udp);
  }

  if (ipv4) {
    ip = (uint8_t*)&ipv4->dst_addr;
    info("d_ip %d.%d.%d.%d\n", ip[0], ip[1], ip[2], ip[3]);
    ip = (uint8_t*)&ipv4->src_addr;
    info("s_ip %d.%d.%d.%d\n", ip[0], ip[1], ip[2], ip[3]);
  }
  if (udp) {
    info("dst_port %d src_port %d\n", ntohs(udp->dst_port), ntohs(udp->src_port));
  }
}

void mt_mbuf_dump(enum mtl_port port, int idx, char* tag, struct rte_mbuf* m) {
  mt_mbuf_dump_hdr(port, idx, tag, m);
  rte_pktmbuf_dump(stdout, m, m->data_len);
}

void mt_lcore_dump() {
  rte_lcore_dump(stdout);
}

void mt_eth_link_dump(uint16_t port_id) {
  struct rte_eth_link eth_link;
  int err;

  err = rte_eth_link_get_nowait(port_id, &eth_link);
  if (err < 0) {
    err("%s, failed to get link status for port %d, ret %d\n", __func__, port_id, err);
    return;
  }

  critical("%s(%d), link_speed %dg link_status %d link_duplex %d link_autoneg %d\n",
           __func__, port_id, eth_link.link_speed / 1000, eth_link.link_status,
           eth_link.link_duplex, eth_link.link_autoneg);
}

void mt_eth_macaddr_dump(enum mtl_port port, char* tag, struct rte_ether_addr* mac_addr) {
  if (tag) info("%s(%d), %s\n", __func__, port, tag);

  uint8_t* addr = &mac_addr->addr_bytes[0];
  info("%02x:%02x:%02x:%02x:%02x:%02x\n", addr[0], addr[1], addr[2], addr[3], addr[4],
       addr[5]);
}

struct rte_mbuf* mt_build_pad(struct mtl_main_impl* impl, struct rte_mempool* mempool,
                              enum mtl_port port, uint16_t ether_type, uint16_t len) {
  struct rte_ether_addr src_mac;
  struct rte_mbuf* pad;
  struct rte_ether_hdr* eth_hdr;

  pad = rte_pktmbuf_alloc(mempool);
  if (unlikely(pad == NULL)) {
    err("%s, fail to allocate pad pktmbuf\n", __func__);
    return NULL;
  }

  mt_macaddr_get(impl, port, &src_mac);
  rte_pktmbuf_append(pad, len);
  pad->data_len = len;
  pad->pkt_len = len;

  eth_hdr = rte_pktmbuf_mtod(pad, struct rte_ether_hdr*);
  memset((char*)eth_hdr, 0, len);
  eth_hdr->ether_type = htons(ether_type);
  mt_eth_d_addr(eth_hdr)->addr_bytes[0] = 0x01;
  mt_eth_d_addr(eth_hdr)->addr_bytes[1] = 0x80;
  mt_eth_d_addr(eth_hdr)->addr_bytes[2] = 0xC2;
  mt_eth_d_addr(eth_hdr)->addr_bytes[5] = 0x01;
  rte_memcpy(mt_eth_s_addr(eth_hdr), &src_mac, RTE_ETHER_ADDR_LEN);

  return pad;
}

int mt_macaddr_get(struct mtl_main_impl* impl, enum mtl_port port,
                   struct rte_ether_addr* mac_addr) {
  struct mt_interface* inf = mt_if(impl, port);

  if (inf->drv_info.flags & MT_DRV_F_NOT_DPDK_PMD) {
    mtl_memcpy(mac_addr, &inf->k_mac_addr, sizeof(*mac_addr));
    return 0;
  }

  uint16_t port_id = mt_port_id(impl, port);
  return rte_eth_macaddr_get(port_id, mac_addr);
}

void* mt_mempool_mem_addr(struct rte_mempool* mp) {
  struct rte_mempool_memhdr* hdr = STAILQ_FIRST(&mp->mem_list);
  if (mp->nb_mem_chunks != 1)
    err("%s(%s), invalid nb_mem_chunks %u\n", __func__, mp->name, mp->nb_mem_chunks);
  return hdr->addr;
}

size_t mt_mempool_mem_size(struct rte_mempool* mp) {
  struct rte_mempool_memhdr* hdr = STAILQ_FIRST(&mp->mem_list);
  if (mp->nb_mem_chunks != 1)
    err("%s(%s), invalid nb_mem_chunks %u\n", __func__, mp->name, mp->nb_mem_chunks);
  return hdr->len;
}

uint32_t mt_mempool_obj_size(struct rte_mempool* mp) {
  return rte_mempool_calc_obj_size(mp->elt_size, mp->flags, NULL);
}

int mt_mempool_dump(struct rte_mempool* mp) {
  uint32_t populated_size = mp->populated_size;
  struct rte_mbuf* mbufs[populated_size];
  uint32_t mbufs_alloced = 0;
  void* base_addr = mt_mempool_mem_addr(mp);
  void* end_addr = base_addr + mt_mempool_mem_size(mp);
  void* last_hdr = NULL;

  info("%s(%s), %u mbufs object size %u, memory range: %p to %p\n", __func__, mp->name,
       populated_size, mt_mempool_obj_size(mp), base_addr, end_addr);
  for (uint32_t i = 0; i < populated_size; i++) {
    mbufs[i] = rte_pktmbuf_alloc(mp);
    if (!mbufs[i]) break;
    mbufs_alloced++;
    void* hdr = rte_pktmbuf_mtod(mbufs[i], void*);
    info("%s(%s), mbuf %u hdr %p step %" PRId64 "\n", __func__, mp->name, i, hdr,
         hdr - last_hdr);
    last_hdr = hdr;
  }

  rte_pktmbuf_free_bulk(mbufs, mbufs_alloced);
  return 0;
}

struct rte_mempool* mt_mempool_create_by_ops(struct mtl_main_impl* impl, const char* name,
                                             unsigned int n, unsigned int cache_size,
                                             uint16_t priv_size, uint16_t element_size,
                                             const char* ops_name, int socket_id) {
  char name_with_idx[32]; /* 32 is the max length allowed by mempool api, in our lib we
                             use concise names so it won't exceed this length */
  struct rte_mempool* mbuf_pool;
  uint16_t data_room_size;
  unsigned int ret = 1;
  float size_m;

  /*
   * https://doc.dpdk.org/api-21.05/rte__mbuf_8h.html#a9e4bd0ae9e01d0f4dfe7d27cfb0d9a7f
   * rte_pktmbuf_pool_create_by_ops api describes
   * optimum size (in terms of memory usage) for a mempool
   * is when n is a power of two minus one: n = (2^q - 1).
   */
  while (ret - 1 <= n && ret) ret <<= 1;

  dbg("%s(%d), optimize number of elements in the mbuf pool from %d to %d\n ", __func__,
      socket_id, n, ret - 1);
  n = ret - 1;

  if (cache_size && (element_size % cache_size)) /* align to cache size */
    element_size = (element_size / cache_size + 1) * cache_size;

  snprintf(name_with_idx, sizeof(name_with_idx), "%s_%d", name, impl->mempool_idx++);
  data_room_size = element_size + MT_MBUF_HEADROOM_SIZE; /* include head room */
  mbuf_pool = rte_pktmbuf_pool_create_by_ops(name_with_idx, n, cache_size, priv_size,
                                             data_room_size, socket_id, ops_name);

  if (!mbuf_pool) {
    err("%s(%d), fail(%s) for %s, n %u\n", __func__, socket_id, rte_strerror(rte_errno),
        name, n);
  } else {
    size_m = (float)n * (data_room_size + priv_size) / (1024 * 1024);
    info("%s(%d), succ at %p size %fm n %u d %u for %s\n", __func__, socket_id, mbuf_pool,
         size_m, n, element_size, name_with_idx);
#ifdef MTL_HAS_ASAN
    g_mt_mempool_create_cnt++;
#endif
  }

  return mbuf_pool;
}

int mt_mempool_free(struct rte_mempool* mp) {
  unsigned int in_use_count = rte_mempool_in_use_count(mp);
  if (in_use_count) {
    /* failed to free the mempool, caused by the mbuf is still in nix tx queues? */
    warn("%s, still has %d mbuf in mempool %s\n", __func__, in_use_count, mp->name);
    return -EBUSY;
  }

  /* no any in-use mbuf */
  info("%s, free mempool %s\n", __func__, mp->name);
  rte_mempool_free(mp);
#ifdef MTL_HAS_ASAN
  g_mt_mempool_create_cnt--;
#endif

  return 0;
}

/* Computing the Internet Checksum based on rfc1071 */
uint16_t mt_rf1071_check_sum(uint8_t* p, size_t len, bool convert) {
  uint16_t* u16_in = (uint16_t*)p;
  uint16_t check_sum = 0;
  uint32_t sum = 0;

  if (convert) {
    while (len > 1) {
      sum += ntohs(*u16_in++);
      len -= 2;
    }
  } else {
    while (len > 1) {
      sum += *u16_in++;
      len -= 2;
    }
  }

  if (len == 1) {
    uint16_t left = 0;
    *(uint8_t*)&left = *(uint8_t*)u16_in;
    sum += left;
  }

  /* fold to 16 */
  sum = (sum >> 16) + (sum & 0xffff);
  sum += (sum >> 16);
  check_sum = ~sum;

  return check_sum;
}

struct mt_u64_fifo* mt_u64_fifo_init(int size, int soc_id) {
  struct mt_u64_fifo* fifo = mt_rte_zmalloc_socket(sizeof(*fifo), soc_id);
  if (!fifo) return NULL;
  uint64_t* data = mt_rte_zmalloc_socket(sizeof(*data) * size, soc_id);
  if (!data) {
    mt_rte_free(fifo);
    return NULL;
  }

  fifo->data = data;
  fifo->size = size;
  return fifo;
}

int mt_u64_fifo_uinit(struct mt_u64_fifo* fifo) {
  if (fifo->used > 0) {
    err("%s, still has %d items\n", __func__, fifo->used);
    return -EIO;
  }
  mt_rte_free(fifo->data);
  mt_rte_free(fifo);
  return 0;
}

/* todo: add overflow check */
int mt_u64_fifo_put(struct mt_u64_fifo* fifo, const uint64_t item) {
  if (fifo->used >= fifo->size) {
    dbg("%s, fail as fifo is full(%d)\n", __func__, fifo->size);
    return -EIO;
  }
  fifo->data[fifo->write_idx] = item;
  fifo->write_idx++;
  if (fifo->write_idx >= fifo->size) fifo->write_idx = 0;
  fifo->used++;
  return 0;
}

/* todo: add overflow check */
int mt_u64_fifo_get(struct mt_u64_fifo* fifo, uint64_t* item) {
  if (fifo->used <= 0) {
    dbg("%s, fail as empty\n", __func__);
    return -EIO;
  }
  *item = fifo->data[fifo->read_idx];
  fifo->read_idx++;
  if (fifo->read_idx >= fifo->size) fifo->read_idx = 0;
  fifo->used--;
  return 0;
}

int mt_u64_fifo_put_bulk(struct mt_u64_fifo* fifo, const uint64_t* items, uint32_t n) {
  if (fifo->used + n > fifo->size) {
    dbg("%s, fail as fifo is full(%d)\n", __func__, fifo->size);
    return -EIO;
  }
  uint32_t i = 0;
  for (i = 0; i < n; i++) {
    fifo->data[fifo->write_idx] = items[i];
    fifo->write_idx++;
    if (fifo->write_idx >= fifo->size) fifo->write_idx = 0;
  }
  fifo->used += n;
  return 0;
}

int mt_u64_fifo_get_bulk(struct mt_u64_fifo* fifo, uint64_t* items, uint32_t n) {
  if (fifo->used < n) {
    dbg("%s, fail as no enough item\n", __func__);
    return -EIO;
  }
  uint32_t i = 0;
  for (i = 0; i < n; i++) {
    items[i] = fifo->data[fifo->read_idx];
    fifo->read_idx++;
    if (fifo->read_idx >= fifo->size) fifo->read_idx = 0;
  }
  fifo->used -= n;
  return 0;
}

int mt_u64_fifo_read_back(struct mt_u64_fifo* fifo, uint64_t* item) {
  if (fifo->used <= 0) {
    dbg("%s, fail as empty\n", __func__);
    return -EIO;
  }
  int idx = fifo->write_idx - 1;
  if (idx < 0) idx = fifo->size - 1;
  *item = fifo->data[idx];
  return 0;
}

int mt_u64_fifo_read_front(struct mt_u64_fifo* fifo, uint64_t* item) {
  if (fifo->used <= 0) {
    dbg("%s, fail as empty\n", __func__);
    return -EIO;
  }
  *item = fifo->data[fifo->read_idx];
  return 0;
}

int mt_u64_fifo_read_any(struct mt_u64_fifo* fifo, uint64_t* item, int skip) {
  if (fifo->used <= 0) {
    dbg("%s, fail as empty\n", __func__);
    return -EIO;
  }
  if (skip < 0 || skip >= fifo->used) {
    dbg("%s, fail as idx(%d) is invalid\n", __func__, skip);
    return -EIO;
  }
  int read_idx = fifo->read_idx + skip;
  if (read_idx >= fifo->size) read_idx -= fifo->size;
  *item = fifo->data[read_idx];
  return 0;
}

int mt_u64_fifo_read_any_bulk(struct mt_u64_fifo* fifo, uint64_t* items, uint32_t n,
                              int skip) {
  if (fifo->used < n) {
    dbg("%s, fail as no enough item\n", __func__);
    return -EIO;
  }
  if (skip < 0 || skip + n > fifo->used) {
    dbg("%s, fail as skip(%d)/n(%u) is invalid\n", __func__, skip, n);
    return -EIO;
  }
  int read_idx = fifo->read_idx + skip;
  if (read_idx >= fifo->size) read_idx -= fifo->size;
  uint32_t i = 0;
  for (i = 0; i < n; i++) {
    items[i] = fifo->data[read_idx];
    read_idx++;
    if (read_idx >= fifo->size) read_idx = 0;
  }

  return 0;
}

/* only for the mbuf fifo */
int mt_fifo_mbuf_clean(struct mt_u64_fifo* fifo) {
  struct rte_mbuf* mbuf;

  while (mt_u64_fifo_count(fifo) > 0) {
    mt_u64_fifo_get(fifo, (uint64_t*)&mbuf);
    rte_pktmbuf_free(mbuf);
  }

  return 0;
}

struct mt_cvt_dma_ctx* mt_cvt_dma_ctx_init(int fifo_size, int soc_id, int type_num) {
  struct mt_cvt_dma_ctx* ctx = mt_rte_zmalloc_socket(sizeof(*ctx), soc_id);
  if (!ctx) return NULL;

  ctx->fifo = mt_u64_fifo_init(fifo_size, soc_id);
  if (!ctx->fifo) goto fail;
  ctx->tran = mt_rte_zmalloc_socket(sizeof(*ctx->tran) * type_num, soc_id);
  if (!ctx->tran) goto fail;
  ctx->done = mt_rte_zmalloc_socket(sizeof(*ctx->done) * type_num, soc_id);
  if (!ctx->done) goto fail;

  return ctx;

fail:
  if (ctx->fifo) mt_u64_fifo_uinit(ctx->fifo);
  if (ctx->tran) mt_rte_free(ctx->tran);
  if (ctx->done) mt_rte_free(ctx->done);
  mt_rte_free(ctx);
  return NULL;
}

int mt_cvt_dma_ctx_uinit(struct mt_cvt_dma_ctx* ctx) {
  mt_u64_fifo_uinit(ctx->fifo);
  mt_rte_free(ctx->tran);
  mt_rte_free(ctx->done);
  mt_rte_free(ctx);
  return 0;
}

int mt_cvt_dma_ctx_push(struct mt_cvt_dma_ctx* ctx, int type) {
  int ret = mt_u64_fifo_put(ctx->fifo, type);
  if (ret < 0) return ret;
  ctx->tran[type]++;
  dbg("%s, tran %d for type %d\n", __func__, ctx->tran[type], type);
  return 0;
}

int mt_cvt_dma_ctx_pop(struct mt_cvt_dma_ctx* ctx) {
  uint64_t type = 0;
  int ret = mt_u64_fifo_get(ctx->fifo, &type);
  if (ret < 0) return ret;
  ctx->done[type]++;
  dbg("%s, done %d for type %" PRIu64 "\n", __func__, ctx->done[type], type);
  return 0;
}

int mt_run_cmd(const char* cmd, char* out, size_t out_len) {
  FILE* fp;
  char* ret;

  fp = popen(cmd, "r");
  if (!fp) {
    err("%s, cmd %s run fail\n", __func__, cmd);
    return -EIO;
  }

  if (out) {
    out[0] = 0;
    ret = fgets(out, out_len, fp);
    if (!ret) {
      warn("%s, cmd %s read return fail\n", __func__, cmd);
      pclose(fp);
      return -EIO;
    }
  }

  pclose(fp);
  return 0;
}

int mt_ip_addr_check(uint8_t* ip) {
  for (int i = 0; i < MTL_IP_ADDR_LEN; i++) {
    if (ip[i]) return 0;
  }

  return -EINVAL;
}

int st_tx_dest_info_check(struct st_tx_dest_info* dst, int num_ports) {
  uint8_t* ip;
  int ret;

  for (int i = 0; i < num_ports; i++) {
    ip = dst->dip_addr[i];
    ret = mt_ip_addr_check(ip);
    if (ret < 0) {
      err("%s(%d), invalid ip %d.%d.%d.%d\n", __func__, i, ip[0], ip[1], ip[2], ip[3]);
      return -EINVAL;
    }
  }

  if (num_ports > 1) {
    if (0 == memcmp(dst->dip_addr[0], dst->dip_addr[1], MTL_IP_ADDR_LEN)) {
      err("%s, same %d.%d.%d.%d for both ip\n", __func__, ip[0], ip[1], ip[2], ip[3]);
      return -EINVAL;
    }
  }

  return 0;
}

int st_rx_source_info_check(struct st_rx_source_info* src, int num_ports) {
  uint8_t* ip;
  int ret;

  for (int i = 0; i < num_ports; i++) {
    ip = src->ip_addr[i];
    ret = mt_ip_addr_check(ip);
    if (ret < 0) {
      err("%s(%d), invalid ip %d.%d.%d.%d\n", __func__, i, ip[0], ip[1], ip[2], ip[3]);
      return -EINVAL;
    }
  }

  if (num_ports > 1) {
    if (0 == memcmp(src->ip_addr[0], src->ip_addr[1], MTL_IP_ADDR_LEN)) {
      err("%s, same %d.%d.%d.%d for both ip\n", __func__, ip[0], ip[1], ip[2], ip[3]);
      return -EINVAL;
    }
  }

  return 0;
}

int st_frame_trans_uinit(struct st_frame_trans* frame, void* device) {
  int idx = frame->idx;
  MTL_MAY_UNUSED(device);

  /* check if it's still shared */
  uint16_t sh_info_refcnt = rte_mbuf_ext_refcnt_read(&frame->sh_info);
  if (sh_info_refcnt)
    warn("%s(%d), sh_info still active, refcnt %d\n", __func__, idx, sh_info_refcnt);

  int refcnt = rte_atomic32_read(&frame->refcnt);
  if (refcnt) warn("%s(%d), refcnt not zero %d\n", __func__, idx, refcnt);
  if (frame->addr) {
    if (frame->flags & ST_FT_FLAG_RTE_MALLOC) {
      dbg("%s(%d), free rte mem\n", __func__, idx);
      mt_rte_free(frame->addr);
    }
#ifdef MTL_GPU_DIRECT_ENABLED
    else if (frame->flags & ST_FT_FLAG_GPU_MALLOC) {
      GpuContext* GpuDevice = device;
      gpu_free_buf(GpuDevice, frame->addr);
    }
#endif /* MTL_GPU_DIRECT_ENABLED */

    frame->addr = NULL;
  }
  frame->iova = 0;

  if (frame->page_table) {
    mt_rte_free(frame->page_table);
    frame->page_table = NULL;
    frame->page_table_len = 0;
  }

  if (frame->user_meta) {
    mt_rte_free(frame->user_meta);
    frame->user_meta = NULL;
    frame->user_meta_buffer_size = 0;
  }

  return 0;
}

int st_vsync_calculate(struct mtl_main_impl* impl, struct st_vsync_info* vsync) {
  uint64_t ptp_time = mt_get_ptp_time(impl, MTL_PORT_P);
  uint64_t next_epoch;
  uint64_t to_next_epochs;

  next_epoch = ptp_time / vsync->meta.frame_time + 1;
  if (next_epoch == vsync->meta.epoch) {
    dbg("%s, ptp_time still in current epoch\n", __func__);
    next_epoch++; /* sync to next */
  }
  to_next_epochs = next_epoch * vsync->meta.frame_time - ptp_time;
  vsync->meta.epoch = next_epoch;
  vsync->next_epoch_tsc = mt_get_tsc(impl) + to_next_epochs;

  dbg("%s, to_next_epochs %fms\n", __func__, (float)to_next_epochs / NS_PER_MS);
  return 0;
}

uint16_t mt_random_port(uint16_t base_port) {
  uint16_t port = base_port;

  srand(mt_get_monotonic_time());
  uint8_t r = rand() & 0xFF;

  /* todo: random generation with awareness of other sessions */
  if (r & 0x80) {
    r &= 0x7F;
    port -= r;
  } else {
    port += r;
  }

  return port;
}

static const char* dpdk_afxdp_port_prefix = "dpdk_af_xdp:";
static const char* dpdk_afpkt_port_prefix = "dpdk_af_packet:";
static const char* kernel_port_prefix = "kernel:";
static const char* native_afxdp_port_prefix = "native_af_xdp:";

enum mtl_pmd_type mtl_pmd_by_port_name(const char* port) {
  dbg("%s, port %s\n", __func__, port);
  if (strncmp(port, dpdk_afxdp_port_prefix, strlen(dpdk_afxdp_port_prefix)) == 0)
    return MTL_PMD_DPDK_AF_XDP;
  else if (strncmp(port, dpdk_afpkt_port_prefix, strlen(dpdk_afpkt_port_prefix)) == 0)
    return MTL_PMD_DPDK_AF_PACKET;
  else if (strncmp(port, kernel_port_prefix, strlen(kernel_port_prefix)) == 0)
    return MTL_PMD_KERNEL_SOCKET;
  else if (strncmp(port, native_afxdp_port_prefix, strlen(native_afxdp_port_prefix)) == 0)
    return MTL_PMD_NATIVE_AF_XDP;
  else
    return MTL_PMD_DPDK_USER; /* default */
}

const char* mt_kernel_port2if(const char* port) {
  if (mtl_pmd_by_port_name(port) != MTL_PMD_KERNEL_SOCKET) {
    err("%s, port %s is not a kernel based\n", __func__, port);
    return NULL;
  }
  return port + strlen(kernel_port_prefix);
}

const char* mt_dpdk_afxdp_port2if(const char* port) {
  if (mtl_pmd_by_port_name(port) != MTL_PMD_DPDK_AF_XDP) {
    err("%s, port %s is not dpdk_af_xdp\n", __func__, port);
    return NULL;
  }
  return port + strlen(dpdk_afxdp_port_prefix);
}

const char* mt_dpdk_afpkt_port2if(const char* port) {
  if (mtl_pmd_by_port_name(port) != MTL_PMD_DPDK_AF_PACKET) {
    err("%s, port %s is not a dpdk_af_pkt\n", __func__, port);
    return NULL;
  }
  return port + strlen(dpdk_afpkt_port_prefix);
}

const char* mt_native_afxdp_port2if(const char* port) {
  if (mtl_pmd_by_port_name(port) != MTL_PMD_NATIVE_AF_XDP) {
    err("%s, port %s is not native_af_xdp\n", __func__, port);
    return NULL;
  }
  return port + strlen(native_afxdp_port_prefix);
}

int mt_user_info_init(struct mt_user_info* info) {
  int ret = -EIO;

  info->pid = getpid();

#ifdef WINDOWSENV /* todo */
  MTL_MAY_UNUSED(ret);
  snprintf(info->hostname, sizeof(info->hostname), "%s", "unknow");
  snprintf(info->user, sizeof(info->user), "%s", "unknow");
  snprintf(info->comm, sizeof(info->comm), "%s", "unknow");
#else
  ret = gethostname(info->hostname, sizeof(info->hostname));
  if (ret < 0) {
    warn("%s, gethostname fail %d\n", __func__, ret);
    snprintf(info->hostname, sizeof(info->hostname), "%s", "unknow");
  }
  uid_t uid = getuid();
  struct passwd* user_info = getpwuid(uid);
  snprintf(info->user, sizeof(info->user), "%s",
           user_info ? user_info->pw_name : "unknow");
  char comm_path[128];
  snprintf(comm_path, sizeof(comm_path), "/proc/%d/comm", info->pid);
  int fd = open(comm_path, O_RDONLY);
  if (fd >= 0) {
    ssize_t bytes = read(fd, info->comm, sizeof(info->comm) - 1);
    if (bytes <= 0)
      snprintf(info->comm, sizeof(info->comm), "%s", "unknow");
    else
      info->comm[strcspn(info->comm, "\n")] = '\0';
    close(fd);
  } else {
    snprintf(info->comm, sizeof(info->comm), "%s", "unknow");
  }
  dbg("%s, comm %s\n", __func__, info->comm);
#endif

  return 0;
}

int mt_read_cpu_usage(struct mt_cpu_usage* usages, int* cpu_ids, int num_cpus) {
#ifdef WINDOWSENV /* todo */
  MTL_MAY_UNUSED(usages);
  MTL_MAY_UNUSED(cpu_ids);
  MTL_MAY_UNUSED(num_cpus);
  err("%s, not support on windows\n", __func__);
  return -ENOTSUP;
#else
  FILE* file;
  char line[256];
  int found = 0;

  file = fopen("/proc/stat", "r");
  if (!file) {
    err("%s, open /proc/stat fail\n", __func__);
    return -EIO;
  }

  while (fgets(line, sizeof(line) - 1, file)) {
    struct mt_cpu_usage cur;
    int cpu;
    int parsed = sscanf(line,
                        "cpu%d %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64
                        " %" PRIu64 " %" PRIu64 " %" PRIu64 "",
                        &cpu, &cur.user, &cur.nice, &cur.system, &cur.idle, &cur.iowait,
                        &cur.irq, &cur.softirq, &cur.steal);
    if (parsed != 9) continue;
    /* check if match with any input cpus */
    for (int i = 0; i < num_cpus; i++) {
      if (cpu == cpu_ids[i]) {
        found++;
        usages[i] = cur;
        dbg("%s, get succ for cpu %d at %d\n", __func__, cpu, i);
        break;
      }
    }
  }

  fclose(file);
  return found;
#endif
}

double mt_calculate_cpu_usage(struct mt_cpu_usage* prev, struct mt_cpu_usage* curr) {
  uint64_t prev_idle = prev->idle + prev->iowait;
  uint64_t curr_idle = curr->idle + curr->iowait;
  uint64_t prev_total = prev->user + prev->nice + prev->system + prev->idle +
                        prev->iowait + prev->irq + prev->softirq + prev->steal;
  uint64_t curr_total = curr->user + curr->nice + curr->system + curr->idle +
                        curr->iowait + curr->irq + curr->softirq + curr->steal;
  uint64_t totald = curr_total - prev_total;
  uint64_t idled = curr_idle - prev_idle;

  return 100.0 * (totald - idled) / totald;
}

bool mt_file_exists(const char* filename) {
  FILE* file = fopen(filename, "r");
  if (file) {
    fclose(file);
    return true;
  }
  return false;
}

int mt_sysfs_write_uint32(const char* path, uint32_t value) {
  int fd = open(path, O_WRONLY);
  if (fd < 0) {
    err("%s, open %s fail %d\n", __func__, path, fd);
    return -EIO;
  }

  char buf[64];
  snprintf(buf, sizeof(buf), "%u", value);
  size_t len = strlen(buf);
  ssize_t bytes_written = write(fd, buf, len);
  int ret;
  if (bytes_written != len) {
    warn("%s, write %u to %s fail\n", __func__, value, path);
    ret = -EIO;
  } else {
    ret = 0;
  }

  close(fd);
  return ret;
}

#define MT_HASH_KEY_LENGTH (40)
// clang-format off
static uint8_t mt_rss_hash_key[MT_HASH_KEY_LENGTH] = {
  0x6d, 0x5a, 0x56, 0xda, 0x25, 0x5b, 0x0e, 0xc2,
  0x41, 0x67, 0x25, 0x3d, 0x43, 0xa3, 0x8f, 0xb0,
  0xd0, 0xca, 0x2b, 0xcb, 0xae, 0x7b, 0x30, 0xb4,
  0x77, 0xcb, 0x2d, 0xa3, 0x80, 0x30, 0xf2, 0x0c,
  0x6a, 0x42, 0xb7, 0x3b, 0xbe, 0xac, 0x01, 0xfa,
};
// clang-format on

uint32_t mt_softrss(uint32_t* input_tuple, uint32_t input_len) {
  return rte_softrss(input_tuple, input_len, mt_rss_hash_key);
}