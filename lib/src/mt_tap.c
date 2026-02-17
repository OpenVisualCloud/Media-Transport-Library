/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "mt_tap.h"

// #define DEBUG
#include <iphlpapi.h>

#include "datapath/mt_queue.h"
#include "mt_arp.h"
#include "mt_cni.h"
#include "mt_log.h"
#include "mt_sch.h"
#include "mt_util.h"

static struct mtl_main_impl* tap_main_impl;
static struct rte_ring* tap_tx_ring;

typedef ULONG (*GetAdaptersInfo_type)(PIP_ADAPTER_INFO AdapterInfo, PULONG SizePointer);

static GetAdaptersInfo_type GetAdaptersInfo_ptr = NULL;

static inline void tap_set_global_impl(struct mtl_main_impl* impl) {
  tap_main_impl = impl;
}

static inline bool io_active(struct overlapped_io* io) {
  return io->iostate == IOSTATE_QUEUED || io->iostate == IOSTATE_IMMEDIATE_RETURN;
}

static struct mtl_main_impl* tap_get_global_impl(void) {
  struct mtl_main_impl* impl = tap_main_impl;
  if (!impl) err("%s, global impl not init\n", __func__);
  return impl;
}

static void tap_rxq_pool_free(struct rte_mbuf* pool) {
  struct rte_mbuf* mbuf = pool;
  uint16_t nb_segs = 1;

  if (mbuf == NULL) return;

  while (mbuf->next) {
    mbuf = mbuf->next;
    nb_segs++;
  }
  pool->nb_segs = nb_segs;
  rte_pktmbuf_free(pool);
}

static int tap_put_mbuf(struct rte_ring* packet_ring, void* mbuf) {
  struct rte_mbuf* pkt = (struct rte_mbuf*)mbuf;
  int ret;
  if (!packet_ring) {
    err("%s, tap ring is not created\n", __func__);
    rte_pktmbuf_free(mbuf);
    return -EIO;
  }
  if (rte_ring_full(packet_ring)) {
    err("%s, tap ring is full\n", __func__);
    return -EIO;
  }
  pkt->data_len = pkt->pkt_len;
  ret = rte_ring_sp_enqueue(packet_ring, (void*)pkt);

  if (ret < 0) {
    err("%s, can not enqueue to the tap ring\n", __func__);
    rte_pktmbuf_free(mbuf);
    return -EBUSY;
  }
  return 0;
}

static void* tap_get_mbuf(struct rte_ring* packet_ring, void** usrptr, uint16_t* len) {
  struct rte_mbuf* pkt;
  int ret;
  if (!packet_ring) {
    err("%s, tap ring is not created\n", __func__);
    return NULL;
  }

  ret = rte_ring_sc_dequeue(packet_ring, (void**)&pkt);

  if (ret < 0) {
    info("%s, tap ring is empty\n", __func__);
    return NULL;
  }
  if (len) *len = pkt->data_len;
  if (usrptr) *usrptr = rte_pktmbuf_mtod_offset(pkt, void*, 0);

  return pkt;
}

static int overlapped_result(struct mt_cni_impl* cni, struct overlapped_io* io) {
  int ret = -1;
  BOOL status;
  struct tap_rt_context* tap_ctx = (struct tap_rt_context*)cni->tap_context;
  switch (io->iostate) {
    case IOSTATE_QUEUED:
      status =
          GetOverlappedResult(tap_ctx->tap_handle, &io->overlapped, &io->size, FALSE);
      if (status) {
        ret = io->size;
        io->status = 0;
        io->iostate = IOSTATE_IMMEDIATE_RETURN;
        assert(ResetEvent(io->overlapped.hEvent));
      } else {
        ret = -1;
        if (GetLastError() != ERROR_IO_INCOMPLETE) {
          io->iostate = IOSTATE_INITIAL;
          io->status = -1;
          assert(ResetEvent(io->overlapped.hEvent));
        }
      }
      break;

    case IOSTATE_IMMEDIATE_RETURN:
      io->iostate = IOSTATE_INITIAL;
      assert(ResetEvent(io->overlapped.hEvent));
      if (io->status) {
        SetLastError(io->status);
        ret = -1;
      } else {
        ret = io->size;
      }
      break;

    case IOSTATE_INITIAL:
      SetLastError(ERROR_INVALID_FUNCTION);
      ret = -1;
      err("%s : Overlapped result wrong state\n", __func__);
      break;

    default:
      assert(0);
  }
  return ret;
}

static long readv(struct mt_cni_impl* cni, struct iovec* iov, int count) {
  struct tap_rt_context* tap_ctx = (struct tap_rt_context*)cni->tap_context;
  long rlen = -1, total = 0;
  BOOL status;
  int err;

  while (count) {
    rlen = 0;
    assert(ResetEvent(tap_ctx->reads.overlapped.hEvent));
    status = ReadFile(tap_ctx->tap_handle, iov->iov_base, iov->iov_len,
                      &tap_ctx->reads.size, &tap_ctx->reads.overlapped);
    if (status) {
      assert(SetEvent(tap_ctx->reads.overlapped.hEvent));
      tap_ctx->reads.iostate = IOSTATE_IMMEDIATE_RETURN;
      tap_ctx->reads.status = 0;
      rlen = tap_ctx->reads.size;
    } else {
      err = GetLastError();
      if (err == ERROR_IO_PENDING) {
        tap_ctx->reads.iostate = IOSTATE_QUEUED;
        tap_ctx->reads.status = err;
        rlen = tap_ctx->reads.size;
      } else {
        assert(SetEvent(tap_ctx->reads.overlapped.hEvent));
        tap_ctx->reads.iostate = IOSTATE_IMMEDIATE_RETURN;
        tap_ctx->reads.status = err;
        rlen = 0;
      }
    }
    if (rlen <= 0) return rlen;
    total += rlen;
    iov++;
    count--;
  }
  return total;
}

static long writev(struct mt_cni_impl* cni, struct iovec* iov, int count) {
  long totallen = 0, wlen = -1;
  BOOL status;
  int err;
  struct tap_rt_context* tap_ctx = (struct tap_rt_context*)cni->tap_context;
  while (count) {
    wlen = 0;
    assert(ResetEvent(tap_ctx->writes.overlapped.hEvent));
    status = WriteFile(tap_ctx->tap_handle, (const char*)iov->iov_base, iov->iov_len,
                       &tap_ctx->writes.size, &tap_ctx->writes.overlapped);
    if (status) {
      tap_ctx->writes.iostate = IOSTATE_IMMEDIATE_RETURN;
      assert(SetEvent(tap_ctx->writes.overlapped.hEvent));
      tap_ctx->writes.status = 0;
      wlen = tap_ctx->writes.size;
    } else {
      err = GetLastError();
      if (err == ERROR_IO_PENDING) {
        tap_ctx->writes.iostate = IOSTATE_QUEUED;
        tap_ctx->writes.status = err;
      } else {
        assert(SetEvent(tap_ctx->writes.overlapped.hEvent));
        tap_ctx->writes.iostate = IOSTATE_IMMEDIATE_RETURN;
        tap_ctx->writes.status = err;
      }
    }
    if (wlen <= 0) return totallen;
    totallen += wlen;
    iov++;
    count--;
  }
  return totallen;
}

static uint16_t tap_tx_packet(struct mt_cni_impl* cni, struct rte_mbuf** bufs,
                              uint16_t nb_pkts) {
  int ret = 0;
  struct tap_rt_context* tap_ctx = (struct tap_rt_context*)cni->tap_context;
  if (unlikely(nb_pkts == 0)) return 0;
  if (io_active(&tap_ctx->writes)) ret = overlapped_result(cni, &tap_ctx->writes);

  if (tap_ctx->writes.iostate == IOSTATE_INITIAL) {
    struct rte_mbuf* mbuf = bufs[0];
    static struct iovec iovecs[2];
    struct rte_mbuf* seg = mbuf;

    iovecs[0].iov_len = rte_pktmbuf_data_len(seg);
    iovecs[0].iov_base = rte_pktmbuf_mtod(seg, void*);
    ret = writev(cni, iovecs, 1);
    if (ret == -1) {
      err("%s write buffer error\n", __func__);
    }
  }
  if (ret > 0)
    return 1;
  else
    return 0;
}

static uint16_t tap_rx_packet(struct mt_cni_impl* cni, struct rte_mbuf** bufs,
                              uint16_t nb_pkts) {
  int len;
  uint16_t num_rx = 0;
  unsigned long num_rx_bytes = 0;
  struct tap_rt_context* tap_ctx = (struct tap_rt_context*)cni->tap_context;
  if (!nb_pkts) return 0;
  if (io_active(&tap_ctx->reads)) overlapped_result(cni, &tap_ctx->reads);

  if (tap_ctx->reads.iostate == IOSTATE_INITIAL) readv(cni, *tap_ctx->iovecs, 1);

  len = tap_ctx->reads.size;
  if (len > 0) {
    struct rte_mbuf* mbuf = tap_ctx->pool;
    struct rte_mbuf* seg = NULL;
    struct rte_mbuf* new_tail = NULL;
    uint16_t data_off = rte_pktmbuf_headroom(mbuf);
    mbuf->pkt_len = len;
    mbuf->nb_segs = 0;
    while (1) {
      struct rte_mbuf* buf = rte_pktmbuf_alloc(tap_ctx->mp);
      if (unlikely(!buf)) {
        if (!new_tail || !seg) goto end;
        seg->next = NULL;
        tap_rxq_pool_free(mbuf);
        goto end;
      }
      seg = seg ? seg->next : mbuf;
      if (tap_ctx->pool == mbuf) tap_ctx->pool = buf;
      if (new_tail) new_tail->next = buf;
      new_tail = buf;
      new_tail->next = seg->next;

      (*tap_ctx->iovecs)[mbuf->nb_segs].iov_len = buf->buf_len - data_off;
      (*tap_ctx->iovecs)[mbuf->nb_segs].iov_base = (char*)buf->buf_addr + data_off;

      seg->data_len = RTE_MIN(seg->buf_len - data_off, len);
      seg->data_off = data_off;

      len -= seg->data_len;
      if (len <= 0) break;
      mbuf->nb_segs++;
      /* First segment has headroom, not the others */
      data_off = 0;
    }
    seg->next = NULL;
    bufs[num_rx++] = mbuf;
    num_rx_bytes += mbuf->pkt_len;
  }
end:
  return num_rx;
}

static struct rte_flow* tap_create_flow(struct mt_cni_impl* cni, uint16_t port_id,
                                        uint16_t q) {
  struct rte_flow_attr attr;
  struct rte_flow_item pattern[4];
  struct rte_flow_action action[2];
  struct rte_flow_action_queue queue;
  struct rte_flow_item_eth eth_spec;
  struct rte_flow_item_eth eth_mask;
  struct rte_flow_item_ipv4 ipv4_spec;
  struct rte_flow_item_ipv4 ipv4_mask;
  struct rte_flow_item_raw spec = {0};
  struct rte_flow_item_raw mask = {0};
  char pkt_buf[90];
  char msk_buf[90];
  struct rte_flow_error error;
  struct rte_flow* r_flow;
  int ret;
  struct tap_rt_context* tap_ctx = (struct tap_rt_context*)cni->tap_context;

  memset(&error, 0, sizeof(error));

  /* queue */
  queue.index = q;

  /* nothing for eth flow */
  memset(&eth_spec, 0, sizeof(eth_spec));
  memset(&eth_mask, 0, sizeof(eth_mask));

  /* ipv4 flow */
  memset(&ipv4_spec, 0, sizeof(ipv4_spec));
  memset(&ipv4_mask, 0, sizeof(ipv4_mask));

  memset(&ipv4_mask.hdr.dst_addr, 0xFF, MTL_IP_ADDR_LEN);
  rte_memcpy(&ipv4_spec.hdr.dst_addr, tap_ctx->ip_addr, MTL_IP_ADDR_LEN);
  info("Flow bind to ip address inet %02x %02x %02x %02x \n", tap_ctx->ip_addr[0],
       tap_ctx->ip_addr[1], tap_ctx->ip_addr[2], tap_ctx->ip_addr[3]);

  memset(&attr, 0, sizeof(attr));
  attr.ingress = 1;

  memset(action, 0, sizeof(action));
  action[0].type = RTE_FLOW_ACTION_TYPE_QUEUE;
  action[0].conf = &queue;
  action[1].type = RTE_FLOW_ACTION_TYPE_END;

  // All dest ip address equal tap ip to the tap flow
  memset(pattern, 0, sizeof(pattern));
  pattern[0].type = RTE_FLOW_ITEM_TYPE_ETH;
  pattern[0].spec = &eth_spec;
  pattern[0].mask = &eth_mask;
  pattern[1].type = RTE_FLOW_ITEM_TYPE_IPV4;
  pattern[1].spec = &ipv4_spec;
  pattern[1].mask = &ipv4_mask;
  pattern[2].type = RTE_FLOW_ITEM_TYPE_END;

  ret = rte_flow_validate(port_id, &attr, pattern, action, &error);
  if (ret < 0) {
    err("%s(%d), rte_flow_validate fail %d for queue %d, %s\n", __func__, port_id, ret, q,
        mt_string_safe(error.message));
    return NULL;
  }

  r_flow = rte_flow_create(port_id, &attr, pattern, action, &error);
  if (!r_flow) {
    err("%s(%d), rte_flow_create fail for queue %d, %s\n", __func__, port_id, q,
        mt_string_safe(error.message));
    return NULL;
  }

  // ARP flow direct to TAP MAC address
  memset(&attr, 0, sizeof(attr));
  attr.ingress = 1;

  memset(action, 0, sizeof(action));
  action[0].type = RTE_FLOW_ACTION_TYPE_QUEUE;
  action[0].conf = &queue;
  action[1].type = RTE_FLOW_ACTION_TYPE_END;
  memset(pkt_buf, 0, sizeof(pkt_buf));
  memset(msk_buf, 0, sizeof(msk_buf));
  snprintf(pkt_buf, 84, "%s",
           "00000000000100000000000208060001080006040000000000000001010101010000000000020"
           "2020202");
  snprintf(pkt_buf, 12, "%02x%02x%02x%02x%02x%02x", tap_ctx->mac_addr.addr_bytes[0],
           tap_ctx->mac_addr.addr_bytes[1], tap_ctx->mac_addr.addr_bytes[2],
           tap_ctx->mac_addr.addr_bytes[3], tap_ctx->mac_addr.addr_bytes[4],
           tap_ctx->mac_addr.addr_bytes[5]);
  info("Flow bind to mac address %12.12s \n", pkt_buf);
  snprintf(msk_buf, 84, "%s",
           "FFFFFFFFFFFF000000000000FFFF0000000000000000000000000000000000000000000000000"
           "0000000");
  memset(pattern, 0, sizeof(pattern));

  spec.pattern = (void*)pkt_buf;
  spec.length = 42;
  mask.pattern = (void*)msk_buf;
  mask.length = 42;

  memset(pattern, 0, sizeof(pattern));
  pattern[0].type = RTE_FLOW_ITEM_TYPE_RAW;
  pattern[0].spec = &spec;
  pattern[0].mask = &mask;
  pattern[1].type = RTE_FLOW_ITEM_TYPE_END;

  r_flow = rte_flow_create(port_id, &attr, pattern, action, &error);
  if (!r_flow) {
    err("%s(%d), rte_flow_create 2 fail for queue %d, %s\n", __func__, port_id, q,
        mt_string_safe(error.message));
    return NULL;
  }
  return r_flow;
}

static int tap_get_ipaddress(struct mt_cni_impl* cni) {
  PIP_ADAPTER_INFO pAdapterInfo;
  PIP_ADAPTER_INFO pAdapter = NULL;
  DWORD dwRetVal = 0;
  static const char library_name[] = "IPHLPAPI.dll";
  static const char function[] = "GetAdaptersInfo";
  ULONG ulOutBufLen = sizeof(IP_ADAPTER_INFO);
  struct tap_rt_context* tap_ctx = (struct tap_rt_context*)cni->tap_context;
  HMODULE library = NULL;

  library = LoadLibraryA(library_name);
  if (library == NULL) {
    err("LoadLibraryA(\"%s\")", library_name);
    return 1;
  }

  GetAdaptersInfo_ptr = (GetAdaptersInfo_type)((void*)GetProcAddress(library, function));
  if (GetAdaptersInfo_ptr == NULL) {
    err("GetProcAddress(\"%s\", \"%s\")\n", library_name, function);
    return 1;
  }

  pAdapterInfo = (IP_ADAPTER_INFO*)malloc(sizeof(IP_ADAPTER_INFO));
  if (pAdapterInfo == NULL) {
    err("Error allocating memory needed to call GetAdaptersinfo\n");
    return 1;
  }

  if (GetAdaptersInfo_ptr(pAdapterInfo, &ulOutBufLen) == ERROR_BUFFER_OVERFLOW) {
    free(pAdapterInfo);
    pAdapterInfo = (IP_ADAPTER_INFO*)malloc(ulOutBufLen);
    if (pAdapterInfo == NULL) {
      err("Error allocating memory needed to call GetAdaptersinfo\n");
      return 1;
    }
  }

  if ((dwRetVal = GetAdaptersInfo_ptr(pAdapterInfo, &ulOutBufLen)) == NO_ERROR) {
    pAdapter = pAdapterInfo;
    while (pAdapter) {
      dbg("\tAdapter Name: \t%s\n", pAdapter->AdapterName);
      if (strcmpi(tap_ctx->tap_name, pAdapter->AdapterName) == 0) {
        dbg("Found ip address %s\n", pAdapter->IpAddressList.IpAddress.String);
        inet_pton(AF_INET, pAdapter->IpAddressList.IpAddress.String, tap_ctx->ip_addr);
      }
      dbg("\tAdapter Desc: \t%s\n", pAdapter->Description);
      dbg("\tIP Address: \t%s\n", pAdapter->IpAddressList.IpAddress.String);
      pAdapter = pAdapter->Next;
    }
  }
  return 0;
}

static int tap_uninit_lcore(struct mtl_main_impl* impl) {
  struct mt_cni_impl* cni = mt_get_cni(impl);
  struct tap_rt_context* tap_ctx = (struct tap_rt_context*)cni->tap_context;

  while (mt_atomic32_read_acquire(&cni->stop_tap) == 0) {
    mt_sleep_ms(10);
  }
  if (tap_ctx->has_lcore) {
    rte_eal_wait_lcore(tap_ctx->lcore);
    mt_sch_put_lcore(impl, tap_ctx->lcore);
  }
  return 0;
}

// Thread to handle rx packets in ring buffer from NIC card
// Write packet to TAP from ring buffer
// Read packets from TAP driver and directly transfer to NIC card
static int tap_bkg_thread(void* arg) {
  struct mtl_main_impl* impl = arg;
  struct mt_cni_impl* cni = mt_get_cni(impl);
  int num_ports = mt_num_ports(impl);
  int i;
  void* data = NULL;
  uint16_t rx, tx, count;
  struct rte_mbuf* pkts_rx[1];
  struct rte_mbuf* pkts_tx[1];

  pkts_tx[0] = NULL;
  pkts_rx[0] = NULL;
  info("%s, start\n", __func__);

  while (mt_atomic32_read_acquire(&cni->stop_tap) == 0) {
    for (i = 0; i < num_ports; i++) {
      count = rte_ring_count(tap_tx_ring);
      while (count) {
        if (!pkts_tx[0]) pkts_tx[0] = tap_get_mbuf(tap_tx_ring, &data, &tx);
        if (pkts_tx[0]) {
          tx = tap_tx_packet(cni, pkts_tx, 1);
          if (tx > 0) {
            mt_free_mbufs(pkts_tx, 1);
            pkts_tx[0] = NULL;
            count--;
          }
        }
      }
    }
    rx = tap_rx_packet(cni, pkts_rx, 1);
    for (i = 0; i < num_ports; i++) {
      if (rx > 0 && pkts_rx[0]) {
        cni->tap_rx_cnt[i] += 1;
        mt_txq_burst(cni->tap_tx_q[i], pkts_rx, 1);
      }
    }
    if (rx) {
      mt_free_mbufs(pkts_rx, 1);
      pkts_rx[0] = NULL;
    }
  }

  info("%s, stop\n", __func__);
  return 0;
}

static int tap_queues_uinit(struct mtl_main_impl* impl) {
  int num_ports = mt_num_ports(impl);
  struct mt_cni_impl* cni = mt_get_cni(impl);
  struct tap_rt_context* tap_ctx = (struct tap_rt_context*)cni->tap_context;
  for (int i = 0; i < num_ports; i++) {
    if (cni->tap_tx_q[i]) {
      struct rte_mbuf* pad = mt_get_pad(impl, i);
      if (pad) mt_txq_flush(cni->tap_tx_q[i], pad);
      mt_txq_put(cni->tap_tx_q[i]);
      cni->tap_tx_q[i] = NULL;
    }
    if (cni->tap_rx_q[i]) {
      mt_rxq_put(cni->tap_rx_q[i]);
      cni->tap_rx_q[i] = NULL;
    }
  }
  if (tap_ctx->iovecs) rte_free(tap_ctx->iovecs);
  if (tap_ctx->pool) tap_rxq_pool_free(tap_ctx->pool);
  if (tap_ctx->mp) mt_mempool_free(tap_ctx->mp);
  return 0;
}

static int configure_tap() {
  int ret, i;
  char ring_name[32];
  unsigned int flags, count;
  struct mtl_main_impl* impl = tap_get_global_impl();
  struct mt_cni_impl* cni = mt_get_cni(impl);
  struct mt_interface* inf = mt_if(impl, 0);
  struct iovec(*iovecs)[inf->nb_rx_desc + 1];
  int data_off = RTE_PKTMBUF_HEADROOM;
  struct tap_rt_context* tap_ctx = (struct tap_rt_context*)cni->tap_context;
  struct rte_mbuf** tmp = &tap_ctx->pool;
  struct rte_mempool* mbuf_pool = mt_mempool_create(
      impl, 0, "tap", inf->nb_rx_desc + ST_TX_VIDEO_SESSIONS_RING_SIZE,
      MT_MBUF_CACHE_SIZE, sizeof(struct mt_muf_priv_data), ST_PKT_MAX_ETHER_BYTES);
  if (!mbuf_pool) {
    err("%s tap mempool create fail\n", __func__);
    return -ENOMEM;
  }
  tap_ctx->mp = mbuf_pool;

  iovecs = rte_zmalloc_socket("TAP", sizeof(*iovecs), 0, mt_socket_id(impl, 0));
  if (!iovecs) {
    err("%s: Couldn't allocate %d RX descriptors", "TAP", inf->nb_rx_desc);
    return -ENOMEM;
  }
  tap_ctx->iovecs = iovecs;

  for (i = 0; i < inf->nb_rx_desc; i++) {
    *tmp = rte_pktmbuf_alloc(mbuf_pool);
    if (!*tmp) {
      err("%s: couldn't allocate memory", "TAP");
      ret = -ENOMEM;
      return ret;
    }
    (*tap_ctx->iovecs)[i].iov_len = (*tmp)->buf_len - data_off;
    (*tap_ctx->iovecs)[i].iov_base = (char*)(*tmp)->buf_addr + data_off;
    data_off = 0;
    tmp = &(*tmp)->next;
  }
  flags = RING_F_SP_ENQ | RING_F_SC_DEQ;
  count = ST_TX_VIDEO_SESSIONS_RING_SIZE;
  snprintf(ring_name, 32, "TX-TAP-PACKET-%d", 0);
  tap_tx_ring = rte_ring_create(ring_name, count, mt_socket_id(impl, 0), flags);
  if (!tap_tx_ring) {
    err("%s, tx rte_ring_create fail\n", __func__);
    return -ENOMEM;
  }
  return 0;
}

static bool tap_open_device(struct mt_cni_impl* cni,
                            PSP_DEVICE_INTERFACE_DETAIL_DATA dev_ifx_detail) {
  struct mtl_main_impl* impl = tap_get_global_impl();
  struct tap_rt_context* tap_ctx = (struct tap_rt_context*)cni->tap_context;
  char* path;
  char tap_device_path[MAX_PATH];

  path = strrchr(dev_ifx_detail->DevicePath, '\\');  // find the last character '\'
  if (path)                                          // remove the character '\'
    path++;
  else
    return false;
  snprintf(tap_ctx->tap_name, sizeof(tap_ctx->tap_name), "%s", path);
  snprintf(tap_device_path, sizeof(tap_device_path), "%s%s%s", USERMODEDEVICEDIR, path,
           TAP_WIN_SUFFIX);
  info("%s create file path %s\n", __func__, tap_device_path);

  tap_ctx->tap_handle =
      CreateFile(tap_device_path, GENERIC_READ | GENERIC_WRITE, 0, 0, OPEN_EXISTING,
                 FILE_ATTRIBUTE_SYSTEM | FILE_FLAG_OVERLAPPED, 0);
  if (tap_ctx->tap_handle == NULL) {
    err("CreateFile failed on device: %s\n", tap_device_path);
    return false;
  }
  ULONG mtu = 0;
  DWORD len;
  if (DeviceIoControl(tap_ctx->tap_handle, ST_IOCTL_GET_TAPMTU, &mtu, sizeof(mtu), &mtu,
                      sizeof(mtu), &len, NULL)) {
    info("TAP-Windows MTU=%d\n", (int)mtu);
  }
  ULONG info[3];
  memset(info, 0, sizeof(info));
  if (DeviceIoControl(tap_ctx->tap_handle, ST_IOCTL_GET_TAPVER, &info, sizeof(info),
                      &info, sizeof(info), &len, NULL)) {
    info("TAP-Windows Driver Version %d.%d %s\n", (int)info[0], (int)info[1],
         (info[2] ? "(DEBUG)" : ""));
  }

  unsigned char mac[6];
  memset(mac, 0, sizeof(mac));
  if (DeviceIoControl(tap_ctx->tap_handle, ST_IOCTL_GET_TAPMAC, &mac, sizeof(mac), &mac,
                      sizeof(mac), &len, NULL)) {
    rte_memcpy(tap_ctx->mac_addr.addr_bytes, mac, 6);
    info("TAP-Windows Mac address %02x-%02x-%02x-%02x-%02x-%02x\n",
         tap_ctx->mac_addr.addr_bytes[0], tap_ctx->mac_addr.addr_bytes[1],
         tap_ctx->mac_addr.addr_bytes[2], tap_ctx->mac_addr.addr_bytes[3],
         tap_ctx->mac_addr.addr_bytes[4], tap_ctx->mac_addr.addr_bytes[5]);
  }

  ULONG status = TRUE;
  if (!DeviceIoControl(tap_ctx->tap_handle, ST_IOCTL_SET_TAPSTATUS, &status,
                       sizeof(status), &status, sizeof(status), &len, NULL)) {
    info(
        "WARNING: The TAP-Windows driver rejected a TAP_WIN_IOCTL_SET_MEDIA_STATUS "
        "DeviceIoControl call.\n");
  }

  memset(&tap_ctx->writes, 0, sizeof(tap_ctx->writes));
  tap_ctx->writes.overlapped.hEvent = CreateEvent(NULL, TRUE, TRUE, NULL);
  if (tap_ctx->writes.overlapped.hEvent == NULL) {
    err("Error: overlapped_io_init: CreateEvent failed");
  }
  memset(&tap_ctx->reads, 0, sizeof(tap_ctx->reads));
  tap_ctx->reads.overlapped.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
  if (tap_ctx->reads.overlapped.hEvent == NULL) {
    err("Error: overlapped_io_init: CreateEvent failed");
  }
  tap_get_ipaddress(cni);
  int num_ports = mt_num_ports(impl);
  if (tap_ctx->flow_control) {
    for (int i = 0; i < num_ports; i++) {
      if (rte_eth_dev_mac_addr_add(mt_port_id(impl, i), &tap_ctx->mac_addr, 0))
        err("%s bind to mac failed \n", __func__);
      tap_create_flow(cni, mt_port_id(impl, i), mt_rxq_queue_id(cni->tap_rx_q[i]));
    }
  }
  return true;
}

static HDEVINFO get_tap_device_information_set(HDEVINFO tapinfo,
                                               PSP_DEVINFO_DATA tapdata) {
  BOOL ret;
  TCHAR tap_id[MAX_PATH];
  DWORD size = 0;
  HDEVINFO tapset;

  /* obtain the driver interface for this device */
  ret = SetupDiGetDeviceInstanceId(tapinfo, tapdata, tap_id, sizeof(tap_id), &size);
  if (ret) {
    dbg("%s tap device id %s\n", __func__, tap_id);
    tapset = SetupDiGetClassDevs(&GUID_NDIS_LAN_CLASS, tap_id, NULL,
                                 DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (tapset == INVALID_HANDLE_VALUE) {
      err("tapdeviceset get fail");
    }
    return tapset;

  } else {
    err("Can not get device instance id");
    return 0;
  }
}

static PSP_DEVICE_INTERFACE_DETAIL_DATA get_tap_device_interface_detail(HDEVINFO tapset) {
  BOOL ret;
  DWORD size = 0;
  SP_DEVICE_INTERFACE_DATA tap_interface_data;
  PSP_DEVICE_INTERFACE_DETAIL_DATA tap_interface_detail;

  memset(&tap_interface_data, 0, sizeof(SP_DEVICE_INTERFACE_DATA));
  tap_interface_data.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

  ret = SetupDiEnumDeviceInterfaces(tapset, 0, &GUID_NDIS_LAN_CLASS, 0,
                                    &tap_interface_data);
  if (ret) {
    size = 0;
    ret = SetupDiGetDeviceInterfaceDetail(tapset, &tap_interface_data, NULL, 0, &size,
                                          NULL);
    if (!ret) {
      if (GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
        tap_interface_detail = calloc(size, sizeof(char));
        if (tap_interface_detail) {
          tap_interface_detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);
          ret = SetupDiGetDeviceInterfaceDetail(tapset, &tap_interface_data,
                                                tap_interface_detail, size, NULL, NULL);
          if (!ret) {
            err("Error get tap interface detail");
            free(tap_interface_detail);
            tap_interface_detail = NULL;
          }
          return tap_interface_detail;
        } else {
          err("Could not allocate memory for dev interface.\n");
        }

      } else {
        err("Get interfacedetail unexpected error");
      }
    }
  } else {
    err("No ndis interface device enumerate");
  }
  return NULL;
}

static int tap_device_init(struct mt_cni_impl* cni) {
  DWORD device_index = 0;
  HDEVINFO dev_info;
  SP_DEVINFO_DATA device_info_data;
  char sz_buffer[MAX_PATH];
  HDEVINFO di_set = INVALID_HANDLE_VALUE;
  PSP_DEVICE_INTERFACE_DETAIL_DATA dev_ifx_detail = NULL;
  struct tap_rt_context* tap_ctx = (struct tap_rt_context*)cni->tap_context;

  dev_info = SetupDiGetClassDevs(&GUID_DEVCLASS_NET, NULL, NULL, DIGCF_PRESENT);
  if (dev_info == INVALID_HANDLE_VALUE) {
    err("SetupDiGetClassDevs(pci_scan)");
    return -EIO;
  }

  device_info_data.cbSize = sizeof(SP_DEVINFO_DATA);
  device_index = 0;
  while (SetupDiEnumDeviceInfo(dev_info, device_index, &device_info_data)) {
    device_index++;
    /* we only want to enumerate net class devices */
    memset(sz_buffer, 0, sizeof(sz_buffer));
    SetupDiGetDeviceRegistryProperty(dev_info, &device_info_data, SPDRP_DEVICEDESC, NULL,
                                     (PBYTE)&sz_buffer, sizeof(sz_buffer), NULL);
    if (strstr(sz_buffer, TAP_WIN_DRIVERNAME) != NULL) {
      di_set = get_tap_device_information_set(dev_info, &device_info_data);
      if (di_set == INVALID_HANDLE_VALUE) continue;
      dev_ifx_detail = get_tap_device_interface_detail(di_set);
      if (!dev_ifx_detail) {
        if (di_set != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(di_set);
        continue;
      } else {
        if (tap_open_device(cni, dev_ifx_detail)) {
          if (dev_ifx_detail) free(dev_ifx_detail);
          if (di_set != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(di_set);

          if (dev_info != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(dev_info);

          break;
        }
      }
    }
    memset(&device_info_data, 0, sizeof(SP_DEVINFO_DATA));
    device_info_data.cbSize = sizeof(SP_DEVINFO_DATA);
  }
  if (dev_info != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(dev_info);
  if (tap_ctx->tap_handle)
    return 0;
  else
    return -EIO;
}

static int tap_device_uninit(struct mtl_main_impl* impl) {
  struct rte_mbuf* pkts_rx;
  void* data = NULL;
  uint16_t tx;
  struct mt_cni_impl* cni = mt_get_cni(impl);
  struct tap_rt_context* tap_ctx = (struct tap_rt_context*)cni->tap_context;

  pkts_rx = tap_get_mbuf(tap_tx_ring, &data, &tx);
  while (pkts_rx) {
    mt_free_mbufs(&pkts_rx, 1);
    pkts_rx = tap_get_mbuf(tap_tx_ring, &data, &tx);
  }
  if (tap_ctx->tap_handle) CloseHandle(tap_ctx->tap_handle);
  return 0;
}

static const struct rte_eth_txconf dev_tx_port_conf = {.tx_rs_thresh = 1,
                                                       .tx_free_thresh = 1};

static int tap_queues_init(struct mtl_main_impl* impl, struct mt_cni_impl* cni) {
  int num_ports = mt_num_ports(impl);

  uint16_t nb_tx_desc;
  int socket_id, ret, i;

  ret = configure_tap();
  if (ret < 0) {
    err("%s, tap configure fail\n", __func__);
    tap_queues_uinit(impl);
    return ret;
  }
  for (i = 0; i < num_ports; i++) {
    struct mt_txq_flow flow;
    memset(&flow, 0, sizeof(flow));
    cni->tap_tx_q[i] = mt_txq_get(impl, i, &flow);
    if (!cni->tap_tx_q[i]) {
      err("%s(%d), tap_tx_q create fail\n", __func__, i);
      tap_queues_uinit(impl);
      return -EIO;
    }
    ret = rte_eth_dev_stop((mt_port_id(impl, i)));
    if (ret < 0) {
      err("%s(%d), rte_eth_tx_queue_stop fail %d\n", __func__, i, ret);
      return ret;
    }
    nb_tx_desc = mt_if_nb_tx_desc(impl, i);
    socket_id = rte_eth_dev_socket_id(mt_port_id(impl, i));
    ret = rte_eth_tx_queue_setup(mt_port_id(impl, i), mt_txq_queue_id(cni->tap_tx_q[i]),
                                 nb_tx_desc, socket_id, &dev_tx_port_conf);
    if (ret < 0) {
      err("%s(%d), rte_eth_tx_queue_setup fail %d\n", __func__, i, ret);
      return ret;
    }
    ret = rte_eth_dev_start((mt_port_id(impl, i)));
    if (ret < 0) {
      err("%s(%d), rte_eth_tx_queue_start fail %d\n", __func__, i, ret);
      return ret;
    }
    info("%s(%d), tx q %d\n", __func__, i, mt_txq_queue_id(cni->tap_tx_q[i]));
  }
  for (i = 0; i < num_ports; i++) {
    struct mt_rxq_flow flow;
    memset(&flow, 0, sizeof(flow));
    cni->tap_rx_q[i] = mt_rxq_get(impl, i, &flow);
    if (!cni->tap_rx_q[i]) {
      err("%s(%d), tap_rx_q create fail\n", __func__, i);
      tap_queues_uinit(impl);
      return -EIO;
    }
    info("%s(%d), rx q %d\n", __func__, i, mt_rxq_queue_id(cni->tap_rx_q[i]));
  }

  return 0;
}

int mt_tap_handle(struct mtl_main_impl* impl, enum mtl_port port) {
  struct mt_cni_impl* cni = mt_get_cni(impl);
  struct rte_mbuf* pkts_rx[ST_CNI_RX_BURST_SIZE];
  uint16_t rx;

  if (mt_atomic32_read_acquire(&cni->stop_tap)) {
    return -EBUSY;
  }

  if (cni->tap_rx_q[port]) {
    rx = mt_rxq_burst(cni->tap_rx_q[port], pkts_rx, ST_CNI_RX_BURST_SIZE);

    if (rx > 0) {
      cni->entries[port].eth_rx_cnt += rx;
      for (int i = 0; i < rx; i++) {
        tap_put_mbuf(tap_tx_ring, pkts_rx[i]);
      }
    }
  }
  return 0;
}

int mt_tap_init(struct mtl_main_impl* impl) {
  int ret;
  struct mt_cni_impl* cni = mt_get_cni(impl);
  unsigned int lcore;
  struct tap_rt_context* tap_ctx;

  tap_set_global_impl(impl);

  cni->tap_context = calloc(sizeof(struct tap_rt_context), sizeof(char));
  tap_ctx = (struct tap_rt_context*)cni->tap_context;
  tap_ctx->flow_control =
      true;  // if do not need flow control, should set NIC to promiscuous mode
  ret = tap_queues_init(impl, cni);
  if (ret < 0) return ret;

  ret = tap_device_init(cni);
  if (ret < 0) return ret;

  mt_atomic32_set(&cni->stop_tap, 0);
  tap_ctx->has_lcore = false;
  ret = mt_sch_get_lcore(impl, &lcore, MT_LCORE_TYPE_TAP, mt_socket_id(impl, MTL_PORT_P));
  if (ret < 0) {
    err("%s, get lcore fail %d\n", __func__, ret);
    mt_tap_uinit(impl);
    return ret;
  }
  tap_ctx->lcore = lcore;
  tap_ctx->has_lcore = true;
  ret = rte_eal_remote_launch(tap_bkg_thread, impl, lcore);
  if (ret < 0) {
    err("%s, launch thread fail %d\n", __func__, ret);
    mt_tap_uinit(impl);
    return ret;
  }

  return 0;
}

int mt_tap_uinit(struct mtl_main_impl* impl) {
  struct mt_cni_impl* cni = mt_get_cni(impl);

  mt_atomic32_set_release(&cni->stop_tap, 1);
  if (cni->tap_bkg_tid) {
    pthread_join(cni->tap_bkg_tid, NULL);
    cni->tap_bkg_tid = 0;
  }
  tap_uninit_lcore(impl);
  tap_queues_uinit(impl);
  tap_device_uninit(impl);
  tap_set_global_impl(NULL);
  if (cni->tap_context) free(cni->tap_context);
  info("%s, succ\n", __func__);
  return 0;
}
