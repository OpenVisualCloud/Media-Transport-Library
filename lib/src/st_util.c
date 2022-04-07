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

#include "st_util.h"

#include "st_log.h"
#include "st_main.h"

bool st_bitmap_test_and_set(uint8_t* bitmap, int idx) {
  int pos = idx / 8;
  int off = idx % 8;
  uint8_t bits = bitmap[pos];

  /* already set */
  if (bits & (0x1 << off)) return true;

  /* set the bit */
  bitmap[pos] = bits | (0x1 << off);
  return false;
}

int st_ring_dequeue_clean(struct rte_ring* ring) {
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

  return 0;
}

void st_mbuf_sanity_check(struct rte_mbuf** mbufs, uint16_t nb, char* tag) {
  struct rte_mbuf* mbuf;

  for (int i = 0; i < nb; i++) {
    mbuf = mbufs[i];
    if ((mbuf->pkt_len < 60) || (mbuf->nb_segs > 2) || (mbuf->pkt_len > 1514)) {
      err("%s(%s), fail on %d len %d nb_segs %d\n", __func__, tag ? tag : "", i,
          mbuf->pkt_len, mbuf->nb_segs);
    }
  }
}

int st_build_port_map(struct st_main_impl* impl, char** ports, enum st_port* maps,
                      int num_ports) {
  struct st_init_params* p = st_get_user_params(impl);
  int main_num_ports = p->num_ports;

  if (num_ports > main_num_ports) {
    err("%s, invalid num_ports %d\n", __func__, num_ports);
    return -EIO;
  }

  for (int i = 0; i < num_ports; i++) {
    int j;
    for (j = 0; j < main_num_ports; j++) {
      if (0 == strncmp(p->port[j], ports[i], ST_PORT_MAX_LEN)) {
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

int st_pacing_train_result_add(struct st_main_impl* impl, enum st_port port,
                               uint64_t rl_bps, float pad_interval) {
  struct st_pacing_train_result* ptr = &st_if(impl, port)->pt_results[0];

  for (int i = 0; i < ST_MAX_RL_ITEMS; i++) {
    if (ptr[i].rl_bps) continue;
    ptr[i].rl_bps = rl_bps;
    ptr[i].pacing_pad_interval = pad_interval;
    return 0;
  }

  err("%s(%d), no space\n", __func__, port);
  return -ENOMEM;
}

int st_pacing_train_result_search(struct st_main_impl* impl, enum st_port port,
                                  uint64_t rl_bps, float* pad_interval) {
  struct st_pacing_train_result* ptr = &st_if(impl, port)->pt_results[0];

  for (int i = 0; i < ST_MAX_RL_ITEMS; i++) {
    if (rl_bps == ptr[i].rl_bps) {
      *pad_interval = ptr[i].pacing_pad_interval;
      return 0;
    }
  }

  dbg("%s(%d), no entry for %" PRIu64 "\n", __func__, port, rl_bps);
  return -EINVAL;
}

void st_video_rtp_dump(enum st_port port, int idx, char* tag,
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

void st_mbuf_dump(enum st_port port, int idx, char* tag, struct rte_mbuf* m) {
  struct rte_ether_hdr* eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr*);
  size_t hdr_offset = sizeof(struct rte_ether_hdr);
  struct rte_ipv4_hdr* ipv4 = NULL;
  struct rte_udp_hdr* udp = NULL;
  uint16_t ether_type = ntohs(eth->ether_type);
  uint8_t* mac;
  uint8_t* ip;

  if (tag) info("%s(%d,%d), %s\n", __func__, port, idx, tag);
  info("ether_type 0x%x\n", ether_type);
  mac = &st_eth_d_addr(eth)->addr_bytes[0];
  info("d_mac %02x:%02x:%02x:%02x:%02x:%02x\n", mac[0], mac[1], mac[2], mac[3], mac[4],
       mac[5]);
  mac = &st_eth_s_addr(eth)->addr_bytes[0];
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

  rte_pktmbuf_dump(stdout, m, m->data_len);
}

void st_lcore_dump() { rte_lcore_dump(stdout); }

void st_eth_link_dump(uint16_t port_id) {
  struct rte_eth_link eth_link;

  rte_eth_link_get(port_id, &eth_link);

  info("%s(%d), link_speed %dg link_status %d link_duplex %d link_autoneg %d\n", __func__,
       port_id, eth_link.link_speed / 1000, eth_link.link_status, eth_link.link_duplex,
       eth_link.link_autoneg);
}

void st_eth_macaddr_dump(enum st_port port, char* tag, struct rte_ether_addr* mac_addr) {
  if (tag) info("%s(%d), %s\n", __func__, port, tag);

  uint8_t* addr = &mac_addr->addr_bytes[0];
  info("%02x:%02x:%02x:%02x:%02x:%02x\n", addr[0], addr[1], addr[2], addr[3], addr[4],
       addr[5]);
}

struct rte_mbuf* st_build_pad(struct st_main_impl* impl, enum st_port port,
                              uint16_t port_id, uint16_t ether_type, uint16_t len) {
  struct rte_ether_addr src_mac;
  struct rte_mbuf* pad;
  struct rte_ether_hdr* eth_hdr;

  pad = rte_pktmbuf_alloc(st_get_mempool(impl, port));
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
  st_eth_d_addr(eth_hdr)->addr_bytes[0] = 0x01;
  st_eth_d_addr(eth_hdr)->addr_bytes[1] = 0x80;
  st_eth_d_addr(eth_hdr)->addr_bytes[2] = 0xC2;
  st_eth_d_addr(eth_hdr)->addr_bytes[5] = 0x01;
  rte_memcpy(st_eth_s_addr(eth_hdr), &src_mac, RTE_ETHER_ADDR_LEN);

  return pad;
}

struct rte_mempool* st_mempool_create(struct st_main_impl* impl, enum st_port port,
                                      const char* name, unsigned int n,
                                      unsigned int cache_size, uint16_t priv_size,
                                      uint16_t element_size) {
  if (element_size % cache_size) { /* align to cache size */
    element_size = (element_size / cache_size + 1) * cache_size;
  }
  uint16_t data_room_size = element_size + ST_MBUF_HEADROOM_SIZE; /* include head room */
  struct rte_mempool* mbuf_pool = rte_pktmbuf_pool_create_by_ops(
      name, n, cache_size, priv_size, data_room_size, st_socket_id(impl, port), "stack");
  if (!mbuf_pool) {
    err("%s(%d), fail(%s) for %s, n %u\n", __func__, port, rte_strerror(rte_errno), name,
        n);
  } else {
    float size_m = (float)n * (data_room_size + priv_size) / (1024 * 1024);
    info("%s(%d), succ at %p size %fm n %u d %u for %s\n", __func__, port, mbuf_pool,
         size_m, n, element_size, name);
  }
  return mbuf_pool;
}

int st_mempool_free(struct rte_mempool* mp) {
  unsigned int in_use_count = rte_mempool_in_use_count(mp);
  if (in_use_count) {
    /* caused by the mbuf is still in nix tx queues */
    warn("%s, still has %d mbuf in mempool %s\n", __func__, in_use_count, mp->name);
    return -EIO;
  }

  /* no any in-use mbuf */
  rte_mempool_free(mp);
  return 0;
}

/* Computing the Internet Checksum based on rfc1071 */
uint16_t st_rf1071_check_sum(uint8_t* p, size_t len, bool convert) {
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
