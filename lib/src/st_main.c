/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "st_main.h"

#include "pipeline/st_plugin.h"
#include "st_admin.h"
#include "st_ancillary_transmitter.h"
#include "st_arp.h"
#include "st_audio_transmitter.h"
#include "st_cni.h"
#include "st_config.h"
#include "st_dev.h"
#include "st_dma.h"
#include "st_fmt.h"
#include "st_log.h"
#include "st_mcast.h"
#include "st_ptp.h"
#include "st_rx_ancillary_session.h"
#include "st_rx_audio_session.h"
#include "st_sch.h"
#include "st_socket.h"
#include "st_tx_ancillary_session.h"
#include "st_tx_audio_session.h"
#include "st_util.h"

enum st_port st_port_by_id(struct st_main_impl* impl, uint16_t port_id) {
  int num_ports = st_num_ports(impl);
  int i;

  for (i = 0; i < num_ports; i++) {
    if (port_id == st_port_id(impl, i)) return i;
  }

  err("%s, invalid port_id %d\n", __func__, port_id);
  return ST_PORT_MAX;
}

bool st_is_valid_socket(struct st_main_impl* impl, int soc_id) {
  int num_ports = st_num_ports(impl);
  int i;

  for (i = 0; i < num_ports; i++) {
    if (soc_id == st_socket_id(impl, i)) return true;
  }

  err("%s, invalid soc_id %d\n", __func__, soc_id);
  return false;
}

static int u64_cmp(const void* a, const void* b) {
  const uint64_t* ai = a;
  const uint64_t* bi = b;

  if (*ai < *bi) {
    return -1;
  } else if (*ai > *bi) {
    return 1;
  }
  return 0;
}

static void* st_calibrate_tsc(void* arg) {
  struct st_main_impl* impl = arg;
  int loop = 100;
  int trim = 10;
  uint64_t array[loop];
  uint64_t tsc_hz_sum = 0;

  for (int i = 0; i < loop; i++) {
    uint64_t start, start_tsc, end, end_tsc;

    start = st_get_monotonic_time();
    start_tsc = rte_get_tsc_cycles();

    st_sleep_ms(10);

    end = st_get_monotonic_time();
    end_tsc = rte_get_tsc_cycles();
    array[i] = NS_PER_S * (end_tsc - start_tsc) / (end - start);
  }

  qsort(array, loop, sizeof(uint64_t), u64_cmp);
  for (int i = trim; i < loop - trim; i++) {
    tsc_hz_sum += array[i];
  }
  impl->tsc_hz = tsc_hz_sum / (loop - trim * 2);

  info("%s, tscHz %" PRIu64 "\n", __func__, impl->tsc_hz);
  return NULL;
}

static int st_tx_audio_uinit(struct st_main_impl* impl) {
  if (!impl->tx_a_init) return 0;

  /* free tx audio context */
  st_audio_transmitter_uinit(&impl->a_trs);
  st_tx_audio_sessions_mgr_uinit(&impl->tx_a_mgr);

  impl->tx_a_init = false;
  return 0;
}

static int st_rx_audio_uinit(struct st_main_impl* impl) {
  if (!impl->rx_a_init) return 0;

  st_rx_audio_sessions_mgr_uinit(&impl->rx_a_mgr);

  impl->rx_a_init = false;
  return 0;
}

static int st_tx_anc_uinit(struct st_main_impl* impl) {
  if (!impl->tx_anc_init) return 0;

  /* free tx ancillary context */
  st_ancillary_transmitter_uinit(&impl->anc_trs);
  st_tx_ancillary_sessions_mgr_uinit(&impl->tx_anc_mgr);

  impl->tx_anc_init = false;
  return 0;
}

static int st_rx_anc_uinit(struct st_main_impl* impl) {
  if (!impl->rx_anc_init) return 0;

  st_rx_ancillary_sessions_mgr_uinit(&impl->rx_anc_mgr);

  impl->rx_anc_init = false;
  return 0;
}

static int st_main_create(struct st_main_impl* impl) {
  int ret;

  ret = st_dev_create(impl);
  if (ret < 0) {
    err("%s, st_dev_create fail %d\n", __func__, ret);
    return ret;
  }

  st_dma_init(impl);

  ret = st_map_init(impl);
  if (ret < 0) {
    err("%s, st_map_init fail %d\n", __func__, ret);
    return ret;
  }

  ret = st_arp_init(impl);
  if (ret < 0) {
    err("%s, st_arp_init fail %d\n", __func__, ret);
    return ret;
  }

  ret = st_mcast_init(impl);
  if (ret < 0) {
    err("%s, st_mcast_init fail %d\n", __func__, ret);
    return ret;
  }

  ret = st_ptp_init(impl);
  if (ret < 0) {
    err("%s, st_ptp_init fail %d\n", __func__, ret);
    return ret;
  }

  ret = st_cni_init(impl);
  if (ret < 0) {
    err("%s, st_cni_init fail %d\n", __func__, ret);
    return ret;
  }

  ret = st_admin_init(impl);
  if (ret < 0) {
    err("%s, st_admin_init fail %d\n", __func__, ret);
    return ret;
  }

  ret = st_plugins_init(impl);
  if (ret < 0) {
    err("%s, st_plugins_init fail %d\n", __func__, ret);
    return ret;
  }

  ret = st_config_init(impl);
  if (ret < 0) {
    err("%s, st_config_init fail %d\n", __func__, ret);
    return ret;
  }

  pthread_create(&impl->tsc_cal_tid, NULL, st_calibrate_tsc, impl);

  info("%s, succ\n", __func__);
  return 0;
}

static int st_main_free(struct st_main_impl* impl) {
  if (impl->tsc_cal_tid) {
    pthread_join(impl->tsc_cal_tid, NULL);
    impl->tsc_cal_tid = 0;
  }

  st_config_uinit(impl);
  st_plugins_uinit(impl);
  st_admin_uinit(impl);
  st_cni_uinit(impl);
  st_ptp_uinit(impl);
  st_arp_uinit(impl);
  st_mcast_uinit(impl);

  st_map_uinit(impl);
  st_dma_uinit(impl);

  st_dev_free(impl);
  info("%s, succ\n", __func__);
  return 0;
}

static int st_user_params_check(struct st_init_params* p) {
  int num_ports = p->num_ports, ret;
  uint8_t* ip = NULL;
  uint8_t if_ip[ST_IP_ADDR_LEN];

  if ((num_ports > ST_PORT_MAX) || (num_ports <= 0)) {
    err("%s, invalid num_ports %d\n", __func__, num_ports);
    return -EINVAL;
  }

  if ((p->pmd[ST_PORT_P] >= ST_PMD_TYPE_MAX) || (p->pmd[ST_PORT_P] < ST_PMD_DPDK_USER)) {
    err("%s, invalid pmd %d\n", __func__, p->pmd[ST_PORT_P]);
    return -EINVAL;
  }

  if (p->pmd[ST_PORT_P] == ST_PMD_DPDK_AF_XDP) {
    if (p->xdp_info[ST_PORT_P].queue_count <= 0) {
      err("%s, invalid queue_count %u for P port\n", __func__,
          p->xdp_info[ST_PORT_P].queue_count);
      return -EINVAL;
    }
    if (p->xdp_info[ST_PORT_P].start_queue <= 0) {
      err("%s, invalid start_queue %u for P port\n", __func__,
          p->xdp_info[ST_PORT_P].start_queue);
      return -EINVAL;
    }
    ret = st_socket_get_if_ip(p->port[ST_PORT_P], if_ip);
    if (ret < 0) {
      err("%s, get ip fail, if %s for P port\n", __func__, p->port[ST_PORT_P]);
      return ret;
    }
  }

  if (p->tx_sessions_cnt_max < 0) {
    err("%s, invalid tx_sessions_cnt_max %u\n", __func__, p->tx_sessions_cnt_max);
    return -EINVAL;
  }

  if (p->rx_sessions_cnt_max < 0) {
    err("%s, invalid rx_sessions_cnt_max %u\n", __func__, p->rx_sessions_cnt_max);
    return -EINVAL;
  }

  if (p->nb_rx_hdr_split_queues > p->rx_sessions_cnt_max) {
    err("%s, too large nb_rx_hdr_split_queues %u, max %u\n", __func__,
        p->nb_rx_hdr_split_queues, p->rx_sessions_cnt_max);
    return -EINVAL;
  }

  if (num_ports > 1) {
    if (0 == strncmp(st_p_port(p), st_r_port(p), ST_PORT_MAX_LEN)) {
      err("%s, same %s for both port\n", __func__, st_p_port(p));
      return -EINVAL;
    }

    if ((p->pmd[ST_PORT_R] >= ST_PMD_TYPE_MAX) ||
        (p->pmd[ST_PORT_R] < ST_PMD_DPDK_USER)) {
      err("%s, invalid pmd %d for r port\n", __func__, p->pmd[ST_PORT_R]);
      return -EINVAL;
    }

    if (p->pmd[ST_PORT_R] == ST_PMD_DPDK_AF_XDP) {
      if (p->xdp_info[ST_PORT_R].queue_count <= 0) {
        err("%s, invalid queue_count %u for R port\n", __func__,
            p->xdp_info[ST_PORT_R].queue_count);
        return -EINVAL;
      }
      if (p->xdp_info[ST_PORT_R].start_queue <= 0) {
        err("%s, invalid start_queue %u for R port\n", __func__,
            p->xdp_info[ST_PORT_R].start_queue);
        return -EINVAL;
      }
      ret = st_socket_get_if_ip(p->port[ST_PORT_R], if_ip);
      if (ret < 0) {
        err("%s, get ip fail, if %s for R port\n", __func__, p->port[ST_PORT_R]);
        return ret;
      }
    }
  }

  for (int i = 0; i < num_ports; i++) {
    if (p->pmd[i] != ST_PMD_DPDK_USER) continue;
    ip = p->sip_addr[i];
    ret = st_ip_addr_check(ip);
    if (ret < 0) {
      err("%s(%d), invalid ip %d.%d.%d.%d\n", __func__, i, ip[0], ip[1], ip[2], ip[3]);
      return -EINVAL;
    }
  }

  if ((num_ports > 1) && (p->pmd[0] == ST_PMD_DPDK_USER) &&
      (p->pmd[1] == ST_PMD_DPDK_USER)) {
    if (0 == memcmp(p->sip_addr[0], p->sip_addr[1], ST_IP_ADDR_LEN)) {
      ip = p->sip_addr[0];
      err("%s, same %d.%d.%d.%d for both ip\n", __func__, ip[0], ip[1], ip[2], ip[3]);
      return -EINVAL;
    }
  }

  return 0;
}

static int _st_start(struct st_main_impl* impl) {
  int ret;

  if (rte_atomic32_read(&impl->started)) {
    dbg("%s, started already\n", __func__);
    return 0;
  }

  /* wait tsc calibrate done, pacing need fine tuned TSC */
  st_wait_tsc_stable(impl);

  ret = st_dev_start(impl);
  if (ret < 0) {
    err("%s, st_dev_start fail %d\n", __func__, ret);
    return ret;
  }

  rte_atomic32_set(&impl->started, 1);

  info("%s, succ, avail ports %d\n", __func__, rte_eth_dev_count_avail());
  return 0;
}

static int _st_stop(struct st_main_impl* impl) {
  if (!rte_atomic32_read(&impl->started)) {
    dbg("%s, not started\n", __func__);
    return 0;
  }

  st_dev_stop(impl);
  rte_atomic32_set(&impl->started, 0);
  info("%s, succ\n", __func__);
  return 0;
}

st_handle st_init(struct st_init_params* p) {
  struct st_main_impl* impl = NULL;
  int socket[ST_PORT_MAX], ret;
  int num_ports = p->num_ports;
  struct st_kport_info kport_info;

  RTE_BUILD_BUG_ON(ST_SESSION_PORT_MAX > (int)ST_PORT_MAX);

  ret = st_user_params_check(p);
  if (ret < 0) {
    err("%s, st_user_params_check fail %d\n", __func__, ret);
    return NULL;
  }

  ret = st_dev_init(p, &kport_info);
  if (ret < 0) {
    err("%s, st_dev_eal_init fail %d\n", __func__, ret);
    return NULL;
  }
  info("st version: %s, dpdk version: %s\n", st_version(), rte_version());

  for (int i = 0; i < num_ports; i++) {
    if (p->pmd[i] != ST_PMD_DPDK_USER)
      socket[i] = st_dev_get_socket(kport_info.port[i]);
    else
      socket[i] = st_dev_get_socket(p->port[i]);
    if (socket[i] < 0) {
      err("%s, get socket fail %d\n", __func__, socket[i]);
      goto err_exit;
    }
  }

#ifndef WINDOWSENV
  int numa_nodes = 0;
  if (numa_available() >= 0) numa_nodes = numa_max_node() + 1;
  if ((p->flags & ST_FLAG_BIND_NUMA) && (numa_nodes > 1)) {
    /* bind current thread and its children to socket node */
    struct bitmask* mask = numa_bitmask_alloc(numa_nodes);

    info("%s, bind to socket %d, numa_nodes %d\n", __func__, socket[ST_PORT_P],
         numa_nodes);
    numa_bitmask_setbit(mask, socket[ST_PORT_P]);
    numa_bind(mask);
    numa_bitmask_free(mask);
  }
#endif

  impl = st_rte_zmalloc_socket(sizeof(*impl), socket[ST_PORT_P]);
  if (!impl) goto err_exit;

  rte_memcpy(&impl->user_para, p, sizeof(*p));
  impl->var_para.sch_default_sleep_us = 1 * US_PER_MS; /* default 1ms */
  /* use sleep zero if sleep us is smaller than this thresh */
  impl->var_para.sch_zero_sleep_threshold_us = 200;

  rte_memcpy(&impl->kport_info, &kport_info, sizeof(kport_info));
  impl->type = ST_SESSION_TYPE_MAIN;
  for (int i = 0; i < num_ports; i++) {
    if (p->pmd[i] != ST_PMD_DPDK_USER) {
      uint8_t if_ip[ST_IP_ADDR_LEN];
      ret = st_socket_get_if_ip(impl->user_para.port[i], if_ip);
      if (ret < 0) {
        err("%s(%d), get IP fail\n", __func__, i);
        goto err_exit;
      }
      /* update the sip */
      rte_memcpy(impl->user_para.sip_addr[i], if_ip, ST_IP_ADDR_LEN);
    }
    /* update socket */
    impl->inf[i].socket_id = socket[i];
    info("%s(%d), socket_id %d\n", __func__, i, socket[i]);
  }
  rte_atomic32_set(&impl->started, 0);
  rte_atomic32_set(&impl->request_exit, 0);
  rte_atomic32_set(&impl->dev_in_reset, 0);
  impl->lcore_lock_fd = -1;
  impl->tx_sessions_cnt_max = RTE_MIN(180, p->tx_sessions_cnt_max);
  impl->rx_sessions_cnt_max = RTE_MIN(180, p->rx_sessions_cnt_max);
  info("%s, max sessions tx %d rx %d, flags 0x%" PRIx64 "\n", __func__,
       impl->tx_sessions_cnt_max, impl->rx_sessions_cnt_max,
       st_get_user_params(impl)->flags);
  impl->pkt_udp_suggest_max_size = ST_PKT_MAX_RTP_BYTES;
  if (p->pkt_udp_suggest_max_size) {
    if ((p->pkt_udp_suggest_max_size > 1000) &&
        (p->pkt_udp_suggest_max_size < (1460 - 8))) {
      impl->pkt_udp_suggest_max_size = p->pkt_udp_suggest_max_size;
      info("%s, new pkt_udp_suggest_max_size %u\n", __func__,
           impl->pkt_udp_suggest_max_size);
    } else {
      warn("%s, invalid pkt_udp_suggest_max_size %u\n", __func__,
           p->pkt_udp_suggest_max_size);
    }
  }
  impl->rx_pool_data_size = 0;
  if (p->rx_pool_data_size) {
    if (p->rx_pool_data_size >= RTE_ETHER_MIN_LEN) {
      impl->rx_pool_data_size = p->rx_pool_data_size;
      info("%s, new rx_pool_data_size %u\n", __func__, impl->rx_pool_data_size);
    } else {
      warn("%s, invalid rx_pool_data_size %u\n", __func__, p->rx_pool_data_size);
    }
  }
  impl->sch_schedule_ns = 200 * NS_PER_US; /* max schedule ns for st_sleep_ms(0) */

  /* init mgr lock for audio and anc */
  st_pthread_mutex_init(&impl->tx_a_mgr_mutex, NULL);
  st_pthread_mutex_init(&impl->rx_a_mgr_mutex, NULL);
  st_pthread_mutex_init(&impl->tx_anc_mgr_mutex, NULL);
  st_pthread_mutex_init(&impl->rx_anc_mgr_mutex, NULL);

  impl->tsc_hz = rte_get_tsc_hz();

  impl->iova_mode = rte_eal_iova_mode();
#ifdef WINDOWSENV /* todo, fix for Win */
  impl->page_size = 4096;
#else
  impl->page_size = sysconf(_SC_PAGESIZE);
#endif

  /* init interface */
  ret = st_dev_if_init(impl);
  if (ret < 0) {
    err("%s, st dev if init fail %d\n", __func__, ret);
    goto err_exit;
  }

  ret = st_main_create(impl);
  if (ret < 0) {
    err("%s, st main create fail %d\n", __func__, ret);
    goto err_exit;
  }

  if (st_has_auto_start_stop(impl)) {
    ret = _st_start(impl);
    if (ret < 0) {
      err("%s, st start fail %d\n", __func__, ret);
      goto err_exit;
    }
  }

  info("%s, succ, tsc_hz %" PRIu64 "\n", __func__, impl->tsc_hz);
  info("%s, simd level %s\n", __func__, st_get_simd_level_name(st_get_simd_level()));
  return impl;

err_exit:
  if (impl) {
    st_dev_if_uinit(impl);
    st_rte_free(impl);
  }
  st_dev_uinit(p);
  return NULL;
}

int st_uninit(st_handle st) {
  struct st_main_impl* impl = st;
  struct st_init_params* p = st_get_user_params(impl);

  if (impl->type != ST_SESSION_TYPE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return -EIO;
  }

  _st_stop(impl);

  st_tx_audio_uinit(impl);
  st_rx_audio_uinit(impl);
  st_tx_anc_uinit(impl);
  st_rx_anc_uinit(impl);

  st_main_free(impl);

  st_dev_if_uinit(impl);
  st_rte_free(impl);

  st_dev_uinit(p);

  info("%s, succ\n", __func__);
  return 0;
}

int st_start(st_handle st) {
  struct st_main_impl* impl = st;

  if (impl->type != ST_SESSION_TYPE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return -EIO;
  }

  return _st_start(impl);
}

int st_stop(st_handle st) {
  struct st_main_impl* impl = st;

  if (impl->type != ST_SESSION_TYPE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return -EIO;
  }

  if (st_has_auto_start_stop(impl)) return 0;

  return _st_stop(impl);
}

int st_get_lcore(st_handle st, unsigned int* lcore) {
  struct st_main_impl* impl = st;

  if (impl->type != ST_SESSION_TYPE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return -EIO;
  }

  return st_dev_get_lcore(impl, lcore);
}

int st_put_lcore(st_handle st, unsigned int lcore) {
  struct st_main_impl* impl = st;

  if (impl->type != ST_SESSION_TYPE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return -EIO;
  }

  return st_dev_put_lcore(impl, lcore);
}

int st_bind_to_lcore(st_handle st, pthread_t thread, unsigned int lcore) {
  struct st_main_impl* impl = st;

  if (impl->type != ST_SESSION_TYPE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return -EIO;
  }

  if (!st_dev_lcore_valid(impl, lcore)) {
    err("%s, invalid lcore %d\n", __func__, lcore);
    return -EINVAL;
  }

  cpu_set_t mask;
  CPU_ZERO(&mask);
  CPU_SET(lcore, &mask);
  pthread_setaffinity_np(thread, sizeof(mask), &mask);

  return 0;
}

int st_request_exit(st_handle st) {
  struct st_main_impl* impl = st;

  if (impl->type != ST_SESSION_TYPE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return -EIO;
  }

  rte_atomic32_set(&impl->request_exit, 1);

  return 0;
}

void* st_memcpy(void* dest, const void* src, size_t n) {
  return rte_memcpy(dest, src, n);
}

void* st_hp_malloc(st_handle st, size_t size, enum st_port port) {
  struct st_main_impl* impl = st;
  int num_ports = st_num_ports(impl);

  if (impl->type != ST_SESSION_TYPE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return NULL;
  }

  if (port < 0 || port >= num_ports) {
    err("%s, invalid port %d\n", __func__, port);
    return NULL;
  }

  return st_rte_malloc_socket(size, st_socket_id(impl, port));
}

void* st_hp_zmalloc(st_handle st, size_t size, enum st_port port) {
  struct st_main_impl* impl = st;
  int num_ports = st_num_ports(impl);

  if (impl->type != ST_SESSION_TYPE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return NULL;
  }

  if (port < 0 || port >= num_ports) {
    err("%s, invalid port %d\n", __func__, port);
    return NULL;
  }

  return st_rte_zmalloc_socket(size, st_socket_id(impl, port));
}

void st_hp_free(st_handle st, void* ptr) { return st_rte_free(ptr); }

st_iova_t st_hp_virt2iova(st_handle st, const void* vaddr) {
  return rte_malloc_virt2iova(vaddr);
}

size_t st_page_size(st_handle st) {
  struct st_main_impl* impl = st;

  if (impl->type != ST_SESSION_TYPE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return 4096;
  }

  return st->page_size;
}

st_iova_t st_dma_map(st_handle st, const void* vaddr, size_t size) {
  struct st_main_impl* impl = st;
  int ret;
  st_iova_t iova;
  size_t page_size = st_page_size(st);

  if (impl->type != ST_SESSION_TYPE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return ST_BAD_IOVA;
  }

  if (!rte_is_aligned((void*)vaddr, page_size)) {
    err("%s, vaddr %p not align to page size\n", __func__, vaddr);
    return ST_BAD_IOVA;
  }

  if (!size || (size % page_size)) {
    err("%s, bad size %" PRIu64 "\n", __func__, size);
    return ST_BAD_IOVA;
  }

  if (impl->iova_mode != RTE_IOVA_VA) {
    err("%s, invalid iova_mode %d\n", __func__, impl->iova_mode);
    return ST_BAD_IOVA;
  }

  struct st_map_item item;
  item.vaddr = (void*)vaddr;
  item.size = size;
  item.iova = ST_BAD_IOVA; /* let map to find one suitable iova for us */
  ret = st_map_add(impl, &item);
  if (ret < 0) return ST_BAD_IOVA;
  iova = item.iova;

  ret = rte_extmem_register((void*)vaddr, size, NULL, 0, page_size);
  if (ret < 0) {
    err("%s, fail(%d,%s) to register extmem %p\n", __func__, ret, rte_strerror(rte_errno),
        vaddr);
    goto fail_extmem;
  }

  /* only map for ST_PORT_P now */
  ret = rte_dev_dma_map(st_port_device(impl, ST_PORT_P), (void*)vaddr, iova, size);
  if (ret < 0) {
    err("%s, dma map fail(%d,%s) for add(%p,%" PRIu64 ")\n", __func__, ret,
        rte_strerror(rte_errno), vaddr, size);
    goto fail_map;
  }

  return iova;

fail_map:
  rte_extmem_unregister((void*)vaddr, size);
fail_extmem:
  st_map_remove(impl, &item);
  return ST_BAD_IOVA;
}

int st_dma_unmap(st_handle st, const void* vaddr, st_iova_t iova, size_t size) {
  struct st_main_impl* impl = st;
  int ret;
  size_t page_size = st_page_size(st);

  if (impl->type != ST_SESSION_TYPE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return -EIO;
  }

  if (!rte_is_aligned((void*)vaddr, page_size)) {
    err("%s, vaddr %p not align to page size\n", __func__, vaddr);
    return -EINVAL;
  }

  if (!size || (size % page_size)) {
    err("%s, bad size %" PRIu64 "\n", __func__, size);
    return -EINVAL;
  }

  if (impl->iova_mode != RTE_IOVA_VA) {
    err("%s, invalid iova_mode %d\n", __func__, impl->iova_mode);
    return -EINVAL;
  }

  struct st_map_item item;
  item.vaddr = (void*)vaddr;
  item.size = size;
  item.iova = iova;
  ret = st_map_remove(impl, &item);
  if (ret < 0) return ret;

  /* only unmap for ST_PORT_P now */
  ret = rte_dev_dma_unmap(st_port_device(impl, ST_PORT_P), (void*)vaddr, iova, size);
  if (ret < 0) {
    err("%s, dma unmap fail(%d,%s) for add(%p,%" PRIu64 ")\n", __func__, ret,
        rte_strerror(rte_errno), vaddr, size);
  }

  rte_extmem_unregister((void*)vaddr, size);

  return 0;
}

st_dma_mem_handle st_dma_mem_alloc(st_handle st, size_t size) {
  struct st_main_impl* impl = st;
  struct st_dma_mem* mem;

  if (impl->type != ST_SESSION_TYPE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return NULL;
  }

  mem = st_rte_zmalloc_socket(sizeof(*mem), st_socket_id(impl, ST_PORT_P));
  if (!mem) {
    err("%s, dma mem malloc fail\n", __func__);
    return NULL;
  }

  size_t page_size = st_page_size(st);
  size_t iova_size = st_size_page_align(size, page_size);
  size_t alloc_size = iova_size + page_size;
  void* alloc_addr = st_zmalloc(alloc_size);
  if (!alloc_addr) {
    err("%s, dma mem alloc fail\n", __func__);
    st_rte_free(mem);
    return NULL;
  }

  void* addr = (void*)ST_ALIGN((uint64_t)alloc_addr, page_size);
  st_iova_t iova = st_dma_map(st, addr, iova_size);
  if (iova == ST_BAD_IOVA) {
    err("%s, dma mem %p map fail\n", __func__, addr);
    st_free(alloc_addr);
    st_rte_free(mem);
    return NULL;
  }

  mem->alloc_addr = alloc_addr;
  mem->alloc_size = alloc_size;
  mem->addr = addr;
  mem->valid_size = size;
  mem->iova = iova;
  mem->iova_size = iova_size;

  info("%s, succ\n", __func__);
  return mem;
}

void st_dma_mem_free(st_handle st, st_dma_mem_handle handle) {
  struct st_dma_mem* mem = handle;
  st_dma_unmap(st, mem->addr, mem->iova, mem->iova_size);
  st_free(mem->alloc_addr);
  st_rte_free(mem);
}

void* st_dma_mem_addr(st_dma_mem_handle handle) {
  struct st_dma_mem* mem = handle;

  return mem->addr;
}

st_iova_t st_dma_mem_iova(st_dma_mem_handle handle) {
  struct st_dma_mem* mem = handle;

  return mem->iova;
}

const char* st_version(void) {
  static char version[128];
  if (version[0] != 0) return version;

  snprintf(version, sizeof(version), "%d.%d.%d %s %s %s", ST_VERSION_MAJOR,
           ST_VERSION_MINOR, ST_VERSION_LAST, __TIMESTAMP__, __ST_GIT__, ST_COMPILER);

  return version;
}

int st_get_cap(st_handle st, struct st_cap* cap) {
  struct st_main_impl* impl = st;

  if (impl->type != ST_SESSION_TYPE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return -EIO;
  }

  cap->tx_sessions_cnt_max = impl->tx_sessions_cnt_max;
  cap->rx_sessions_cnt_max = impl->rx_sessions_cnt_max;
  cap->dma_dev_cnt_max = impl->dma_mgr.num_dma_dev;
  cap->init_flags = st_get_user_params(impl)->flags;
  return 0;
}

int st_get_stats(st_handle st, struct st_stats* stats) {
  struct st_main_impl* impl = st;
  struct st_dma_mgr* mgr = st_get_dma_mgr(impl);

  if (impl->type != ST_SESSION_TYPE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return -EIO;
  }

  stats->st20_tx_sessions_cnt = rte_atomic32_read(&impl->st20_tx_sessions_cnt);
  stats->st22_tx_sessions_cnt = rte_atomic32_read(&impl->st22_tx_sessions_cnt);
  stats->st30_tx_sessions_cnt = rte_atomic32_read(&impl->st30_tx_sessions_cnt);
  stats->st40_tx_sessions_cnt = rte_atomic32_read(&impl->st40_tx_sessions_cnt);
  stats->st20_rx_sessions_cnt = rte_atomic32_read(&impl->st20_rx_sessions_cnt);
  stats->st22_rx_sessions_cnt = rte_atomic32_read(&impl->st22_rx_sessions_cnt);
  stats->st30_rx_sessions_cnt = rte_atomic32_read(&impl->st30_rx_sessions_cnt);
  stats->st40_rx_sessions_cnt = rte_atomic32_read(&impl->st40_rx_sessions_cnt);
  stats->sch_cnt = rte_atomic32_read(&st_sch_get_mgr(impl)->sch_cnt);
  stats->lcore_cnt = rte_atomic32_read(&impl->lcore_cnt);
  stats->dma_dev_cnt = rte_atomic32_read(&mgr->num_dma_dev_active);
  if (rte_atomic32_read(&impl->started))
    stats->dev_started = 1;
  else
    stats->dev_started = 0;
  return 0;
}

int st_sch_enable_sleep(st_handle st, int sch_idx, bool enable) {
  struct st_main_impl* impl = st;

  if (sch_idx > ST_MAX_SCH_NUM) {
    err("%s, invalid sch_idx %d\n", __func__, sch_idx);
    return -EIO;
  }
  if (impl->type != ST_SESSION_TYPE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return -EIO;
  }

  struct st_sch_impl* sch = st_sch_instance(impl, sch_idx);
  if (!sch) {
    err("%s(%d), sch instance null\n", __func__, sch_idx);
    return -EIO;
  }
  if (!st_sch_is_active(sch)) {
    err("%s(%d), not allocated\n", __func__, sch_idx);
    return -EIO;
  }

  st_sch_enable_allow_sleep(sch, enable);
  info("%s(%d), %s allow sleep\n", __func__, sch_idx, enable ? "enable" : "disable");
  return 0;
}

int st_sch_set_sleep_us(st_handle st, uint64_t us) {
  struct st_main_impl* impl = st;

  if (impl->type != ST_SESSION_TYPE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return -EIO;
  }

  impl->var_para.sch_force_sleep_us = us;
  info("%s, us %" PRIu64 "\n", __func__, us);
  return 0;
}

uint64_t st_ptp_read_time(st_handle st) {
  struct st_main_impl* impl = st;

  if (impl->type != ST_SESSION_TYPE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return 0;
  }

  return st_get_ptp_time(impl, ST_PORT_P);
}

st_udma_handle st_udma_create(st_handle st, uint16_t nb_desc, enum st_port port) {
  struct st_main_impl* impl = st;
  struct st_dma_request_req req;

  if (impl->type != ST_SESSION_TYPE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return NULL;
  }

  req.nb_desc = nb_desc;
  req.max_shared = 1;
  req.sch_idx = 0;
  req.socket_id = st_socket_id(impl, port);
  req.priv = impl;
  req.drop_mbuf_cb = NULL;
  struct st_dma_lender_dev* dev = st_dma_request_dev(impl, &req);
  if (dev) dev->type = ST_SESSION_TYPE_UDMA;
  return dev;
}

int st_udma_free(st_udma_handle handle) {
  struct st_dma_lender_dev* dev = handle;
  struct st_main_impl* impl = dev->priv;

  if (dev->type != ST_SESSION_TYPE_UDMA) {
    err("%s, invalid type %d\n", __func__, dev->type);
    return -EIO;
  }

  return st_dma_free_dev(impl, dev);
}

int st_udma_copy(st_udma_handle handle, st_iova_t dst, st_iova_t src, uint32_t length) {
  struct st_dma_lender_dev* dev = handle;

  if (dev->type != ST_SESSION_TYPE_UDMA) {
    err("%s, invalid type %d\n", __func__, dev->type);
    return -EIO;
  }

  return st_dma_copy(dev, dst, src, length);
}

int st_udma_fill(st_udma_handle handle, st_iova_t dst, uint64_t pattern,
                 uint32_t length) {
  struct st_dma_lender_dev* dev = handle;

  if (dev->type != ST_SESSION_TYPE_UDMA) {
    err("%s, invalid type %d\n", __func__, dev->type);
    return -EIO;
  }

  return st_dma_fill(dev, dst, pattern, length);
}

int st_udma_submit(st_udma_handle handle) {
  struct st_dma_lender_dev* dev = handle;

  if (dev->type != ST_SESSION_TYPE_UDMA) {
    err("%s, invalid type %d\n", __func__, dev->type);
    return -EIO;
  }

  return st_dma_submit(dev);
}

uint16_t st_udma_completed(st_udma_handle handle, const uint16_t nb_cpls) {
  struct st_dma_lender_dev* dev = handle;

  if (dev->type != ST_SESSION_TYPE_UDMA) {
    err("%s, invalid type %d\n", __func__, dev->type);
    return -EIO;
  }

  return st_dma_completed(dev, nb_cpls, NULL, NULL);
}

enum st_simd_level st_get_simd_level(void) {
  if (rte_cpu_get_flag_enabled(RTE_CPUFLAG_AVX512VBMI2))
    return ST_SIMD_LEVEL_AVX512_VBMI2;
  if (rte_cpu_get_flag_enabled(RTE_CPUFLAG_AVX512VL)) return ST_SIMD_LEVEL_AVX512;
  if (rte_cpu_get_flag_enabled(RTE_CPUFLAG_AVX2)) return ST_SIMD_LEVEL_AVX2;
  /* no simd */
  return ST_SIMD_LEVEL_NONE;
}

static const char* st_simd_level_names[ST_SIMD_LEVEL_MAX] = {
    "none",
    "avx2",
    "avx512",
    "avx512_vbmi",
};

const char* st_get_simd_level_name(enum st_simd_level level) {
  if ((level >= ST_SIMD_LEVEL_MAX) || (level < 0)) {
    err("%s, invalid level %d\n", __func__, level);
    return "unknown";
  }

  return st_simd_level_names[level];
}
