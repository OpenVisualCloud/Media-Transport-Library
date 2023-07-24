/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "mt_util.h"

#include "mt_log.h"
#include "mt_main.h"

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
    rte_panic("%s, detect not free mempool, leak cnt %d\n", __func__,
              g_mt_mempool_create_cnt);
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

int mt_pacing_train_result_add(struct mtl_main_impl* impl, enum mtl_port port,
                               uint64_t rl_bps, float pad_interval) {
  struct mt_pacing_train_result* ptr = &mt_if(impl, port)->pt_results[0];

  for (int i = 0; i < MT_MAX_RL_ITEMS; i++) {
    if (ptr[i].rl_bps) continue;
    ptr[i].rl_bps = rl_bps;
    ptr[i].pacing_pad_interval = pad_interval;
    return 0;
  }

  err("%s(%d), no space\n", __func__, port);
  return -ENOMEM;
}

int mt_pacing_train_result_search(struct mtl_main_impl* impl, enum mtl_port port,
                                  uint64_t rl_bps, float* pad_interval) {
  struct mt_pacing_train_result* ptr = &mt_if(impl, port)->pt_results[0];

  for (int i = 0; i < MT_MAX_RL_ITEMS; i++) {
    if (rl_bps == ptr[i].rl_bps) {
      *pad_interval = ptr[i].pacing_pad_interval;
      return 0;
    }
  }

  dbg("%s(%d), no entry for %" PRIu64 "\n", __func__, port, rl_bps);
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

void mt_lcore_dump() { rte_lcore_dump(stdout); }

void mt_eth_link_dump(uint16_t port_id) {
  struct rte_eth_link eth_link;

  rte_eth_link_get_nowait(port_id, &eth_link);

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
                              uint16_t port_id, uint16_t ether_type, uint16_t len) {
  struct rte_ether_addr src_mac;
  struct rte_mbuf* pad;
  struct rte_ether_hdr* eth_hdr;

  pad = rte_pktmbuf_alloc(mempool);
  if (unlikely(pad == NULL)) {
    err("%s, fail to allocate pad pktmbuf\n", __func__);
    return NULL;
  }

  rte_eth_macaddr_get(port_id, &src_mac);
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

struct rte_mempool* mt_mempool_create_by_ops(struct mtl_main_impl* impl,
                                             enum mtl_port port, const char* name,
                                             unsigned int n, unsigned int cache_size,
                                             uint16_t priv_size, uint16_t element_size,
                                             const char* ops_name) {
  if (cache_size && (element_size % cache_size)) { /* align to cache size */
    element_size = (element_size / cache_size + 1) * cache_size;
  }
  uint16_t data_room_size = element_size + MT_MBUF_HEADROOM_SIZE; /* include head room */
  struct rte_mempool* mbuf_pool = rte_pktmbuf_pool_create_by_ops(
      name, n, cache_size, priv_size, data_room_size, mt_socket_id(impl, port), ops_name);
  if (!mbuf_pool) {
    err("%s(%d), fail(%s) for %s, n %u\n", __func__, port, rte_strerror(rte_errno), name,
        n);
  } else {
    float size_m = (float)n * (data_room_size + priv_size) / (1024 * 1024);
    info("%s(%d), succ at %p size %fm n %u d %u for %s\n", __func__, port, mbuf_pool,
         size_m, n, element_size, name);
#ifdef MTL_HAS_ASAN
    g_mt_mempool_create_cnt++;
#endif
  }
  return mbuf_pool;
}

int mt_mempool_free(struct rte_mempool* mp) {
  unsigned int in_use_count = rte_mempool_in_use_count(mp);
  if (in_use_count) {
    /* caused by the mbuf is still in nix tx queues? */
    err("%s, still has %d mbuf in mempool %s\n", __func__, in_use_count, mp->name);
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
int mt_u64_fifo_put(struct mt_u64_fifo* fifo, uint64_t item) {
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
  mt_u64_fifo_put(ctx->fifo, type);
  ctx->tran[type]++;
  dbg("%s, tran %d for type %d\n", __func__, ctx->tran[type], type);
  return 0;
}

int mt_cvt_dma_ctx_pop(struct mt_cvt_dma_ctx* ctx) {
  uint64_t type = 0;
  mt_u64_fifo_get(ctx->fifo, &type);
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
      err("%s, cmd %s read return fail\n", __func__, cmd);
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
    ip = src->sip_addr[i];
    ret = mt_ip_addr_check(ip);
    if (ret < 0) {
      err("%s(%d), invalid ip %d.%d.%d.%d\n", __func__, i, ip[0], ip[1], ip[2], ip[3]);
      return -EINVAL;
    }
  }

  if (num_ports > 1) {
    if (0 == memcmp(src->sip_addr[0], src->sip_addr[1], MTL_IP_ADDR_LEN)) {
      err("%s, same %d.%d.%d.%d for both ip\n", __func__, ip[0], ip[1], ip[2], ip[3]);
      return -EINVAL;
    }
  }

  return 0;
}

int st_frame_trans_uinit(struct st_frame_trans* frame) {
  int idx = frame->idx;

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
