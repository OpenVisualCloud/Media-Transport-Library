/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "mt_main.h"

#include "mt_admin.h"
#include "mt_arp.h"
#include "mt_cni.h"
#include "mt_config.h"
#include "mt_dev.h"
#include "mt_dma.h"
#include "mt_log.h"
#include "mt_mcast.h"
#include "mt_ptp.h"
#include "mt_rss.h"
#include "mt_sch.h"
#include "mt_shared_queue.h"
#include "mt_socket.h"
#include "mt_stat.h"
#include "mt_util.h"
#include "st2110/pipeline/st_plugin.h"
#include "st2110/st_ancillary_transmitter.h"
#include "st2110/st_audio_transmitter.h"
#include "st2110/st_rx_ancillary_session.h"
#include "st2110/st_rx_audio_session.h"
#include "st2110/st_tx_ancillary_session.h"
#include "st2110/st_tx_audio_session.h"

enum mtl_port mt_port_by_id(struct mtl_main_impl* impl, uint16_t port_id) {
  int num_ports = mt_num_ports(impl);
  int i;

  for (i = 0; i < num_ports; i++) {
    if (port_id == mt_port_id(impl, i)) return i;
  }

  err("%s, invalid port_id %d\n", __func__, port_id);
  return MTL_PORT_MAX;
}

bool mt_is_valid_socket(struct mtl_main_impl* impl, int soc_id) {
  int num_ports = mt_num_ports(impl);
  int i;

  for (i = 0; i < num_ports; i++) {
    if (soc_id == mt_socket_id(impl, i)) return true;
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

static void* mt_calibrate_tsc(void* arg) {
  struct mtl_main_impl* impl = arg;
  int loop = 100;
  int trim = 10;
  uint64_t array[loop];
  uint64_t tsc_hz_sum = 0;

  for (int i = 0; i < loop; i++) {
    uint64_t start, start_tsc, end, end_tsc;

    start = mt_get_monotonic_time();
    start_tsc = rte_get_tsc_cycles();

    mt_sleep_ms(10);

    end = mt_get_monotonic_time();
    end_tsc = rte_get_tsc_cycles();
    array[i] = NS_PER_S * (end_tsc - start_tsc) / (end - start);
  }

  qsort(array, loop, sizeof(uint64_t), u64_cmp);
  for (int i = trim; i < loop - trim; i++) {
    tsc_hz_sum += array[i];
  }
  impl->tsc_hz = tsc_hz_sum / (loop - trim * 2);
  mt_dev_tsc_done_action(impl);

  info("%s, tscHz %" PRIu64 "\n", __func__, impl->tsc_hz);
  return NULL;
}

static int st_tx_audio_uinit(struct mtl_main_impl* impl) {
  if (!impl->tx_a_init) return 0;

  /* free tx audio context */
  st_audio_transmitter_uinit(&impl->a_trs);
  st_tx_audio_sessions_mgr_uinit(&impl->tx_a_mgr);

  impl->tx_a_init = false;
  return 0;
}

static int st_rx_audio_uinit(struct mtl_main_impl* impl) {
  if (!impl->rx_a_init) return 0;

  st_rx_audio_sessions_mgr_uinit(&impl->rx_a_mgr);

  impl->rx_a_init = false;
  return 0;
}

static int st_tx_anc_uinit(struct mtl_main_impl* impl) {
  if (!impl->tx_anc_init) return 0;

  /* free tx ancillary context */
  st_ancillary_transmitter_uinit(&impl->anc_trs);
  st_tx_ancillary_sessions_mgr_uinit(&impl->tx_anc_mgr);

  impl->tx_anc_init = false;
  return 0;
}

static int st_rx_anc_uinit(struct mtl_main_impl* impl) {
  if (!impl->rx_anc_init) return 0;

  st_rx_ancillary_sessions_mgr_uinit(&impl->rx_anc_mgr);

  impl->rx_anc_init = false;
  return 0;
}

static int mt_main_create(struct mtl_main_impl* impl) {
  int ret;

  ret = mt_stat_init(impl);
  if (ret < 0) {
    err("%s, mt stat init fail %d\n", __func__, ret);
    return ret;
  }

  ret = mt_dev_create(impl);
  if (ret < 0) {
    err("%s, mt_dev_create fail %d\n", __func__, ret);
    return ret;
  }

  mt_dma_init(impl);

  ret = mt_rss_init(impl);
  if (ret < 0) {
    err("%s, mt_rss_init fail %d\n", __func__, ret);
    return ret;
  }

  ret = mt_rsq_init(impl);
  if (ret < 0) {
    err("%s, mt_rsq_init fail %d\n", __func__, ret);
    return ret;
  }
  ret = mt_tsq_init(impl);
  if (ret < 0) {
    err("%s, mt_tsq_init fail %d\n", __func__, ret);
    return ret;
  }

  ret = mt_dev_if_post_init(impl);
  if (ret < 0) {
    err("%s, if post init fail %d\n", __func__, ret);
    return ret;
  }

  ret = mt_map_init(impl);
  if (ret < 0) {
    err("%s, mt_map_init fail %d\n", __func__, ret);
    return ret;
  }

  ret = mt_arp_init(impl);
  if (ret < 0) {
    err("%s, mt_arp_init fail %d\n", __func__, ret);
    return ret;
  }

  ret = mt_mcast_init(impl);
  if (ret < 0) {
    err("%s, mt_mcast_init fail %d\n", __func__, ret);
    return ret;
  }

  ret = mt_ptp_init(impl);
  if (ret < 0) {
    err("%s, mt_ptp_init fail %d\n", __func__, ret);
    return ret;
  }

  ret = mt_cni_init(impl);
  if (ret < 0) {
    err("%s, mt_cni_init fail %d\n", __func__, ret);
    return ret;
  }

  ret = mt_admin_init(impl);
  if (ret < 0) {
    err("%s, mt_admin_init fail %d\n", __func__, ret);
    return ret;
  }

  ret = st_plugins_init(impl);
  if (ret < 0) {
    err("%s, st_plugins_init fail %d\n", __func__, ret);
    return ret;
  }

  ret = mt_config_init(impl);
  if (ret < 0) {
    err("%s, mt_config_init fail %d\n", __func__, ret);
    return ret;
  }

  pthread_create(&impl->tsc_cal_tid, NULL, mt_calibrate_tsc, impl);

  info("%s, succ\n", __func__);
  return 0;
}

static int mt_main_free(struct mtl_main_impl* impl) {
  if (impl->tsc_cal_tid) {
    pthread_join(impl->tsc_cal_tid, NULL);
    impl->tsc_cal_tid = 0;
  }

  mt_config_uinit(impl);
  st_plugins_uinit(impl);
  mt_admin_uinit(impl);
  mt_cni_uinit(impl);
  mt_ptp_uinit(impl);
  mt_arp_uinit(impl);
  mt_mcast_uinit(impl);

  mt_map_uinit(impl);
  mt_dma_uinit(impl);
  mt_dev_if_pre_uinit(impl);
  mt_rss_uinit(impl);
  mt_rsq_uinit(impl);
  mt_tsq_uinit(impl);

  mt_dev_free(impl);
  mt_stat_uinit(impl);
  info("%s, succ\n", __func__);
  return 0;
}

static int mt_user_params_check(struct mtl_init_params* p) {
  int num_ports = p->num_ports, ret;
  uint8_t* ip = NULL;
  uint8_t if_ip[MTL_IP_ADDR_LEN];
  uint8_t if_netmask[MTL_IP_ADDR_LEN];

  /* hdr split queues check */
  if (p->nb_rx_hdr_split_queues > p->rx_sessions_cnt_max) {
    err("%s, too large nb_rx_hdr_split_queues %u, max %u\n", __func__,
        p->nb_rx_hdr_split_queues, p->rx_sessions_cnt_max);
    return -EINVAL;
  }

  /* num_ports check */
  if ((num_ports > MTL_PORT_MAX) || (num_ports <= 0)) {
    err("%s, invalid num_ports %d\n", __func__, num_ports);
    return -EINVAL;
  }
  /* info check for each port */
  for (int i = 0; i < num_ports; i++) {
    enum mtl_pmd_type pmd = p->pmd[i];

    /* type check */
    if ((pmd >= MTL_PMD_TYPE_MAX) || (pmd < 0)) {
      err("%s(%d), invalid pmd type %d\n", __func__, i, pmd);
      return -EINVAL;
    }

    /* af xdp check */
    if (pmd == MTL_PMD_DPDK_AF_XDP) {
      if (p->xdp_info[i].queue_count <= 0) {
        err("%s(%d), invalid queue_count %u\n", __func__, i, p->xdp_info[i].queue_count);
        return -EINVAL;
      }
      if (p->xdp_info[i].start_queue <= 0) {
        err("%s(%d), invalid start_queue %u\n", __func__, i, p->xdp_info[i].start_queue);
        return -EINVAL;
      }
      ret = mt_socket_get_if_ip(p->port[i], if_ip, if_netmask);
      if (ret < 0) {
        err("%s(%d), get ip fail from if %s for P port\n", __func__, i, p->port[i]);
        return ret;
      }
    }
    if (p->pmd[i] == MTL_PMD_DPDK_USER) {
      ip = p->sip_addr[i];
      ret = mt_ip_addr_check(ip);
      if (ret < 0) {
        err("%s(%d), invalid ip %d.%d.%d.%d\n", __func__, i, ip[0], ip[1], ip[2], ip[3]);
        return -EINVAL;
      }
    }

    if (i > 0) {
      for (int j = 0; j < i; j++) {
        /* check if duplicate port name */
        if (0 == strncmp(p->port[i], p->port[j], MTL_PORT_MAX_LEN)) {
          err("%s, same name %s for port %d and %d\n", __func__, p->port[i], i, j);
          return -EINVAL;
        }
        /* check if duplicate ip */
        if ((p->pmd[i] == MTL_PMD_DPDK_USER) && (p->pmd[j] == MTL_PMD_DPDK_USER)) {
          if (0 == memcmp(p->sip_addr[i], p->sip_addr[j], MTL_IP_ADDR_LEN)) {
            ip = p->sip_addr[j];
            err("%s, same ip %d.%d.%d.%d for port %d and %d\n", __func__, ip[0], ip[1],
                ip[2], ip[3], i, j);
            return -EINVAL;
          }
        }
      }
    }
  }

  return 0;
}

static int _mt_start(struct mtl_main_impl* impl) {
  int ret;

  if (mt_started(impl)) {
    dbg("%s, started already\n", __func__);
    return 0;
  }

  /* wait tsc calibrate done, pacing need fine tuned TSC */
  mt_wait_tsc_stable(impl);

  ret = mt_dev_start(impl);
  if (ret < 0) {
    err("%s, mt_dev_start fail %d\n", __func__, ret);
    return ret;
  }

  rte_atomic32_set(&impl->instance_started, 1);

  info("%s, succ, avail ports %d\n", __func__, rte_eth_dev_count_avail());
  return 0;
}

static int _mt_stop(struct mtl_main_impl* impl) {
  if (!mt_started(impl)) {
    dbg("%s, not started\n", __func__);
    return 0;
  }

  mt_dev_stop(impl);
  rte_atomic32_set(&impl->instance_started, 0);
  info("%s, succ\n", __func__);
  return 0;
}

mtl_handle mtl_init(struct mtl_init_params* p) {
  struct mtl_main_impl* impl = NULL;
  int socket[MTL_PORT_MAX], ret;
  int num_ports = p->num_ports;
  struct mt_kport_info kport_info;

#ifdef MTL_HAS_ASAN
  mt_asan_init();
#endif

  RTE_BUILD_BUG_ON(MTL_SESSION_PORT_MAX > (int)MTL_PORT_MAX);
  RTE_BUILD_BUG_ON(sizeof(struct mt_udp_hdr) != 42);

  ret = mt_user_params_check(p);
  if (ret < 0) {
    err("%s, mt_user_params_check fail %d\n", __func__, ret);
    return NULL;
  }

  ret = mt_dev_init(p, &kport_info);
  if (ret < 0) {
    err("%s, mt_dev_eal_init fail %d\n", __func__, ret);
    return NULL;
  }
  info("st version: %s, dpdk version: %s\n", mtl_version(), rte_version());

  for (int i = 0; i < num_ports; i++) {
    if (p->pmd[i] != MTL_PMD_DPDK_USER)
      socket[i] = mt_dev_get_socket(kport_info.port[i]);
    else
      socket[i] = mt_dev_get_socket(p->port[i]);
    if (socket[i] < 0) {
      err("%s, get socket fail %d\n", __func__, socket[i]);
      goto err_exit;
    }
  }

#ifndef WINDOWSENV
  int numa_nodes = 0;
  if (numa_available() >= 0) numa_nodes = numa_max_node() + 1;
  if ((p->flags & MTL_FLAG_BIND_NUMA) && (numa_nodes > 1)) {
    /* bind current thread and its children to socket node */
    struct bitmask* mask = numa_bitmask_alloc(numa_nodes);

    info("%s, bind to socket %d, numa_nodes %d\n", __func__, socket[MTL_PORT_P],
         numa_nodes);
    numa_bitmask_setbit(mask, socket[MTL_PORT_P]);
    numa_bind(mask);
    numa_bitmask_free(mask);
  }
#endif

  impl = mt_rte_zmalloc_socket(sizeof(*impl), socket[MTL_PORT_P]);
  if (!impl) goto err_exit;

  rte_memcpy(&impl->user_para, p, sizeof(*p));
  impl->var_para.sch_default_sleep_us = 1 * US_PER_MS; /* default 1ms */
  /* use sleep zero if sleep us is smaller than this thresh */
  impl->var_para.sch_zero_sleep_threshold_us = 200;

  rte_memcpy(&impl->kport_info, &kport_info, sizeof(kport_info));
  impl->type = MT_HANDLE_MAIN;
  for (int i = 0; i < num_ports; i++) {
    if (p->pmd[i] != MTL_PMD_DPDK_USER) {
      uint8_t if_ip[MTL_IP_ADDR_LEN];
      uint8_t if_netmask[MTL_IP_ADDR_LEN];
      uint8_t if_gateway[MTL_IP_ADDR_LEN];
      ret = mt_socket_get_if_ip(impl->user_para.port[i], if_ip, if_netmask);
      if (ret < 0) {
        err("%s(%d), get IP fail\n", __func__, i);
        goto err_exit;
      }
      /* update the sip and net mask */
      rte_memcpy(impl->user_para.sip_addr[i], if_ip, MTL_IP_ADDR_LEN);
      rte_memcpy(impl->user_para.netmask[i], if_netmask, MTL_IP_ADDR_LEN);
      if (!mt_ip_to_u32(impl->user_para.gateway[i])) {
        /* try to fetch gateway */
        ret = mt_socket_get_if_gateway(impl->user_para.port[i], if_gateway);
        if (ret >= 0) {
          info("%s(%d), get gateway succ from if\n", __func__, i);
          rte_memcpy(impl->user_para.gateway[i], if_gateway, MTL_IP_ADDR_LEN);
        }
      }
    } else { /* MTL_PMD_DPDK_USER */
      uint32_t netmask = mt_ip_to_u32(impl->user_para.netmask[i]);
      if (!netmask) { /* set to default if user not set a netmask */
        impl->user_para.netmask[i][0] = 255;
        impl->user_para.netmask[i][1] = 255;
        impl->user_para.netmask[i][2] = 255;
        impl->user_para.netmask[i][3] = 0;
      }
    }
    /* update socket */
    impl->inf[i].socket_id = socket[i];
    info("%s(%d), socket_id %d\n", __func__, i, socket[i]);
  }
  rte_atomic32_set(&impl->instance_started, 0);
  rte_atomic32_set(&impl->instance_aborted, 0);
  rte_atomic32_set(&impl->instance_in_reset, 0);
  impl->lcore_lock_fd = -1;
  if (p->transport == MTL_TRANSPORT_ST2110) {
    if (p->tx_sessions_cnt_max)
      impl->user_tx_queues_cnt = RTE_MIN(180, p->tx_sessions_cnt_max);
    if (p->rx_sessions_cnt_max)
      impl->user_rx_queues_cnt = RTE_MIN(180, p->rx_sessions_cnt_max);
  } else if (p->transport == MTL_TRANSPORT_UDP) {
    if (p->tx_queues_cnt_max)
      impl->user_tx_queues_cnt = p->tx_queues_cnt_max;
    else
      impl->user_tx_queues_cnt = 64;
    if (p->rx_queues_cnt_max)
      impl->user_rx_queues_cnt = p->rx_queues_cnt_max;
    else
      impl->user_rx_queues_cnt = 64;
  } else {
    err("%s, invalid transport %d\n", __func__, p->transport);
    ret = -EINVAL;
    goto err_exit;
  }
  info("%s, max user queues tx %d rx %d, flags 0x%" PRIx64 "\n", __func__,
       impl->user_tx_queues_cnt, impl->user_rx_queues_cnt,
       mt_get_user_params(impl)->flags);
  /* parse rss config, not use now */
  impl->rss_mode = p->rss_mode;
  impl->pkt_udp_suggest_max_size = MTL_PKT_MAX_RTP_BYTES;
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
  impl->sch_schedule_ns = 200 * NS_PER_US; /* max schedule ns for mt_sleep_ms(0) */

  /* init mgr lock for audio and anc */
  mt_pthread_mutex_init(&impl->tx_a_mgr_mutex, NULL);
  mt_pthread_mutex_init(&impl->rx_a_mgr_mutex, NULL);
  mt_pthread_mutex_init(&impl->tx_anc_mgr_mutex, NULL);
  mt_pthread_mutex_init(&impl->rx_anc_mgr_mutex, NULL);

  impl->tsc_hz = rte_get_tsc_hz();

  impl->iova_mode = rte_eal_iova_mode();
#ifdef WINDOWSENV /* todo, fix for Win */
  impl->page_size = 4096;
#else
  impl->page_size = sysconf(_SC_PAGESIZE);
#endif

  /* init interface */
  ret = mt_dev_if_init(impl);
  if (ret < 0) {
    err("%s, st dev if init fail %d\n", __func__, ret);
    goto err_exit;
  }

  ret = mt_main_create(impl);
  if (ret < 0) {
    err("%s, st main create fail %d\n", __func__, ret);
    goto err_exit;
  }

  if (mt_has_auto_start_stop(impl)) {
    ret = _mt_start(impl);
    if (ret < 0) {
      err("%s, st start fail %d\n", __func__, ret);
      goto err_exit;
    }
  }

  info("%s, succ, tsc_hz %" PRIu64 "\n", __func__, impl->tsc_hz);
  info("%s, simd level %s\n", __func__, mtl_get_simd_level_name(mtl_get_simd_level()));
  return impl;

err_exit:
  if (impl) mtl_uninit(impl);
  return NULL;
}

int mtl_uninit(mtl_handle mt) {
  struct mtl_main_impl* impl = mt;
  struct mtl_init_params* p = mt_get_user_params(impl);

  if (impl->type != MT_HANDLE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return -EIO;
  }

  _mt_stop(impl);

  st_tx_audio_uinit(impl);
  st_rx_audio_uinit(impl);
  st_tx_anc_uinit(impl);
  st_rx_anc_uinit(impl);

  mt_main_free(impl);

  mt_dev_if_uinit(impl);
  mt_rte_free(impl);

  mt_dev_uinit(p);

#ifdef MTL_HAS_ASAN
  mt_asan_check();
#endif

  info("%s, succ\n", __func__);
  return 0;
}

int mtl_start(mtl_handle mt) {
  struct mtl_main_impl* impl = mt;

  if (impl->type != MT_HANDLE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return -EIO;
  }

  return _mt_start(impl);
}

int mtl_stop(mtl_handle mt) {
  struct mtl_main_impl* impl = mt;

  if (impl->type != MT_HANDLE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return -EIO;
  }

  if (mt_has_auto_start_stop(impl)) return 0;

  return _mt_stop(impl);
}

int mtl_get_lcore(mtl_handle mt, unsigned int* lcore) {
  struct mtl_main_impl* impl = mt;

  if (impl->type != MT_HANDLE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return -EIO;
  }

  return mt_dev_get_lcore(impl, lcore);
}

int mtl_put_lcore(mtl_handle mt, unsigned int lcore) {
  struct mtl_main_impl* impl = mt;

  if (impl->type != MT_HANDLE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return -EIO;
  }

  return mt_dev_put_lcore(impl, lcore);
}

int mtl_bind_to_lcore(mtl_handle mt, pthread_t thread, unsigned int lcore) {
  struct mtl_main_impl* impl = mt;

  if (impl->type != MT_HANDLE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return -EIO;
  }

  if (!mt_dev_lcore_valid(impl, lcore)) {
    err("%s, invalid lcore %d\n", __func__, lcore);
    return -EINVAL;
  }

  cpu_set_t mask;
  CPU_ZERO(&mask);
  CPU_SET(lcore, &mask);
  pthread_setaffinity_np(thread, sizeof(mask), &mask);

  return 0;
}

int mtl_abort(mtl_handle mt) {
  struct mtl_main_impl* impl = mt;

  if (impl->type != MT_HANDLE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return -EIO;
  }

  rte_atomic32_set(&impl->instance_aborted, 1);

  return 0;
}

void* mtl_memcpy(void* dest, const void* src, size_t n) {
  return rte_memcpy(dest, src, n);
}

void* mtl_hp_malloc(mtl_handle mt, size_t size, enum mtl_port port) {
  struct mtl_main_impl* impl = mt;
  int num_ports = mt_num_ports(impl);

  if (impl->type != MT_HANDLE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return NULL;
  }

  if (port < 0 || port >= num_ports) {
    err("%s, invalid port %d\n", __func__, port);
    return NULL;
  }

  return mt_rte_malloc_socket(size, mt_socket_id(impl, port));
}

void* mtl_hp_zmalloc(mtl_handle mt, size_t size, enum mtl_port port) {
  struct mtl_main_impl* impl = mt;
  int num_ports = mt_num_ports(impl);

  if (impl->type != MT_HANDLE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return NULL;
  }

  if (port < 0 || port >= num_ports) {
    err("%s, invalid port %d\n", __func__, port);
    return NULL;
  }

  return mt_rte_zmalloc_socket(size, mt_socket_id(impl, port));
}

void mtl_hp_free(mtl_handle mt, void* ptr) { return mt_rte_free(ptr); }

mtl_iova_t mtl_hp_virt2iova(mtl_handle mt, const void* vaddr) {
  return rte_malloc_virt2iova(vaddr);
}

size_t mtl_page_size(mtl_handle mt) {
  struct mtl_main_impl* impl = mt;

  if (impl->type != MT_HANDLE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return 4096;
  }

  return impl->page_size;
}

mtl_iova_t mtl_dma_map(mtl_handle mt, const void* vaddr, size_t size) {
  struct mtl_main_impl* impl = mt;
  int ret;
  mtl_iova_t iova;
  size_t page_size = mtl_page_size(impl);

  if (impl->type != MT_HANDLE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return MTL_BAD_IOVA;
  }

  if (!rte_is_aligned((void*)vaddr, page_size)) {
    err("%s, vaddr %p not align to page size\n", __func__, vaddr);
    return MTL_BAD_IOVA;
  }

  if (!size || (size % page_size)) {
    err("%s, bad size %" PRIu64 "\n", __func__, size);
    return MTL_BAD_IOVA;
  }

  if (impl->iova_mode != RTE_IOVA_VA) {
    err("%s, invalid iova_mode %d\n", __func__, impl->iova_mode);
    return MTL_BAD_IOVA;
  }

  struct mt_map_item item;
  item.vaddr = (void*)vaddr;
  item.size = size;
  item.iova = MTL_BAD_IOVA; /* let map to find one suitable iova for us */
  ret = mt_map_add(impl, &item);
  if (ret < 0) return MTL_BAD_IOVA;
  iova = item.iova;

  ret = rte_extmem_register((void*)vaddr, size, NULL, 0, page_size);
  if (ret < 0) {
    err("%s, fail(%d,%s) to register extmem %p\n", __func__, ret, rte_strerror(rte_errno),
        vaddr);
    goto fail_extmem;
  }

  /* only map for MTL_PORT_P now */
  ret = rte_dev_dma_map(mt_port_device(impl, MTL_PORT_P), (void*)vaddr, iova, size);
  if (ret < 0) {
    err("%s, dma map fail(%d,%s) for add(%p,%" PRIu64 ")\n", __func__, ret,
        rte_strerror(rte_errno), vaddr, size);
    goto fail_map;
  }

  return iova;

fail_map:
  rte_extmem_unregister((void*)vaddr, size);
fail_extmem:
  mt_map_remove(impl, &item);
  return MTL_BAD_IOVA;
}

int mtl_dma_unmap(mtl_handle mt, const void* vaddr, mtl_iova_t iova, size_t size) {
  struct mtl_main_impl* impl = mt;
  int ret;
  size_t page_size = mtl_page_size(impl);

  if (impl->type != MT_HANDLE_MAIN) {
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

  struct mt_map_item item;
  item.vaddr = (void*)vaddr;
  item.size = size;
  item.iova = iova;
  ret = mt_map_remove(impl, &item);
  if (ret < 0) return ret;

  /* only unmap for MTL_PORT_P now */
  ret = rte_dev_dma_unmap(mt_port_device(impl, MTL_PORT_P), (void*)vaddr, iova, size);
  if (ret < 0) {
    err("%s, dma unmap fail(%d,%s) for add(%p,%" PRIu64 ")\n", __func__, ret,
        rte_strerror(rte_errno), vaddr, size);
  }

  rte_extmem_unregister((void*)vaddr, size);

  return 0;
}

mtl_dma_mem_handle mtl_dma_mem_alloc(mtl_handle mt, size_t size) {
  struct mtl_main_impl* impl = mt;
  struct mtl_dma_mem* mem;

  if (impl->type != MT_HANDLE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return NULL;
  }

  mem = mt_rte_zmalloc_socket(sizeof(*mem), mt_socket_id(impl, MTL_PORT_P));
  if (!mem) {
    err("%s, dma mem malloc fail\n", __func__);
    return NULL;
  }

  size_t page_size = mtl_page_size(impl);
  size_t iova_size = mtl_size_page_align(size, page_size);
  size_t alloc_size = iova_size + page_size;
  void* alloc_addr = mt_zmalloc(alloc_size);
  if (!alloc_addr) {
    err("%s, dma mem alloc fail\n", __func__);
    mt_rte_free(mem);
    return NULL;
  }

  void* addr = (void*)MTL_ALIGN((uint64_t)alloc_addr, page_size);
  mtl_iova_t iova = mtl_dma_map(impl, addr, iova_size);
  if (iova == MTL_BAD_IOVA) {
    err("%s, dma mem %p map fail\n", __func__, addr);
    mt_free(alloc_addr);
    mt_rte_free(mem);
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

void mtl_dma_mem_free(mtl_handle mt, mtl_dma_mem_handle handle) {
  struct mtl_dma_mem* mem = handle;
  mtl_dma_unmap(mt, mem->addr, mem->iova, mem->iova_size);
  mt_free(mem->alloc_addr);
  mt_rte_free(mem);
}

void* mtl_dma_mem_addr(mtl_dma_mem_handle handle) {
  struct mtl_dma_mem* mem = handle;

  return mem->addr;
}

mtl_iova_t mtl_dma_mem_iova(mtl_dma_mem_handle handle) {
  struct mtl_dma_mem* mem = handle;

  return mem->iova;
}

const char* mtl_version(void) {
  static char version[128];
  if (version[0] != 0) return version;

  snprintf(version, sizeof(version), "%d.%d.%d %s %s %s", MTL_VERSION_MAJOR,
           MTL_VERSION_MINOR, MTL_VERSION_LAST, __TIMESTAMP__, __MTL_GIT__, MTL_COMPILER);

  return version;
}

int mtl_get_cap(mtl_handle mt, struct mtl_cap* cap) {
  struct mtl_main_impl* impl = mt;

  if (impl->type != MT_HANDLE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return -EIO;
  }

  cap->tx_sessions_cnt_max = impl->user_tx_queues_cnt;
  cap->rx_sessions_cnt_max = impl->user_rx_queues_cnt;
  cap->dma_dev_cnt_max = impl->dma_mgr.num_dma_dev;
  cap->init_flags = mt_get_user_params(impl)->flags;
  return 0;
}

int mtl_get_stats(mtl_handle mt, struct mtl_stats* stats) {
  struct mtl_main_impl* impl = mt;
  struct mt_dma_mgr* mgr = mt_get_dma_mgr(impl);

  if (impl->type != MT_HANDLE_MAIN) {
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
  stats->sch_cnt = rte_atomic32_read(&mt_sch_get_mgr(impl)->sch_cnt);
  stats->lcore_cnt = rte_atomic32_read(&impl->lcore_cnt);
  stats->dma_dev_cnt = rte_atomic32_read(&mgr->num_dma_dev_active);
  if (mt_started(impl))
    stats->dev_started = 1;
  else
    stats->dev_started = 0;
  return 0;
}

int mtl_sch_enable_sleep(mtl_handle mt, int sch_idx, bool enable) {
  struct mtl_main_impl* impl = mt;

  if (sch_idx > MT_MAX_SCH_NUM) {
    err("%s, invalid sch_idx %d\n", __func__, sch_idx);
    return -EIO;
  }
  if (impl->type != MT_HANDLE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return -EIO;
  }

  struct mt_sch_impl* sch = mt_sch_instance(impl, sch_idx);
  if (!sch) {
    err("%s(%d), sch instance null\n", __func__, sch_idx);
    return -EIO;
  }
  if (!mt_sch_is_active(sch)) {
    err("%s(%d), not allocated\n", __func__, sch_idx);
    return -EIO;
  }

  mt_sch_enable_allow_sleep(sch, enable);
  info("%s(%d), %s allow sleep\n", __func__, sch_idx, enable ? "enable" : "disable");
  return 0;
}

int mtl_sch_set_sleep_us(mtl_handle mt, uint64_t us) {
  struct mtl_main_impl* impl = mt;

  if (impl->type != MT_HANDLE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return -EIO;
  }

  impl->var_para.sch_force_sleep_us = us;
  info("%s, us %" PRIu64 "\n", __func__, us);
  return 0;
}

uint64_t mtl_ptp_read_time(mtl_handle mt) {
  struct mtl_main_impl* impl = mt;

  if (impl->type != MT_HANDLE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return 0;
  }

  return mt_get_ptp_time(impl, MTL_PORT_P);
}

mtl_udma_handle mtl_udma_create(mtl_handle mt, uint16_t nb_desc, enum mtl_port port) {
  struct mtl_main_impl* impl = mt;
  struct mt_dma_request_req req;

  if (impl->type != MT_HANDLE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return NULL;
  }

  req.nb_desc = nb_desc;
  req.max_shared = 1;
  req.sch_idx = 0;
  req.socket_id = mt_socket_id(impl, port);
  req.priv = impl;
  req.drop_mbuf_cb = NULL;
  struct mtl_dma_lender_dev* dev = mt_dma_request_dev(impl, &req);
  if (dev) dev->type = MT_HANDLE_UDMA;
  return dev;
}

int mtl_udma_free(mtl_udma_handle handle) {
  struct mtl_dma_lender_dev* dev = handle;
  struct mtl_main_impl* impl = dev->priv;

  if (dev->type != MT_HANDLE_UDMA) {
    err("%s, invalid type %d\n", __func__, dev->type);
    return -EIO;
  }

  return mt_dma_free_dev(impl, dev);
}

int mtl_udma_copy(mtl_udma_handle handle, mtl_iova_t dst, mtl_iova_t src,
                  uint32_t length) {
  struct mtl_dma_lender_dev* dev = handle;

  if (dev->type != MT_HANDLE_UDMA) {
    err("%s, invalid type %d\n", __func__, dev->type);
    return -EIO;
  }

  return mt_dma_copy(dev, dst, src, length);
}

int mtl_udma_fill(mtl_udma_handle handle, mtl_iova_t dst, uint64_t pattern,
                  uint32_t length) {
  struct mtl_dma_lender_dev* dev = handle;

  if (dev->type != MT_HANDLE_UDMA) {
    err("%s, invalid type %d\n", __func__, dev->type);
    return -EIO;
  }

  return mt_dma_fill(dev, dst, pattern, length);
}

int mtl_udma_submit(mtl_udma_handle handle) {
  struct mtl_dma_lender_dev* dev = handle;

  if (dev->type != MT_HANDLE_UDMA) {
    err("%s, invalid type %d\n", __func__, dev->type);
    return -EIO;
  }

  return mt_dma_submit(dev);
}

uint16_t mtl_udma_completed(mtl_udma_handle handle, const uint16_t nb_cpls) {
  struct mtl_dma_lender_dev* dev = handle;

  if (dev->type != MT_HANDLE_UDMA) {
    err("%s, invalid type %d\n", __func__, dev->type);
    return -EIO;
  }

  return mt_dma_completed(dev, nb_cpls, NULL, NULL);
}

enum mtl_simd_level mtl_get_simd_level(void) {
  if (rte_cpu_get_flag_enabled(RTE_CPUFLAG_AVX512VBMI2))
    return MTL_SIMD_LEVEL_AVX512_VBMI2;
  if (rte_cpu_get_flag_enabled(RTE_CPUFLAG_AVX512VL)) return MTL_SIMD_LEVEL_AVX512;
  if (rte_cpu_get_flag_enabled(RTE_CPUFLAG_AVX2)) return MTL_SIMD_LEVEL_AVX2;
  /* no simd */
  return MTL_SIMD_LEVEL_NONE;
}

static const char* mt_simd_level_names[MTL_SIMD_LEVEL_MAX] = {
    "none",
    "avx2",
    "avx512",
    "avx512_vbmi",
};

const char* mtl_get_simd_level_name(enum mtl_simd_level level) {
  if ((level >= MTL_SIMD_LEVEL_MAX) || (level < 0)) {
    err("%s, invalid level %d\n", __func__, level);
    return "unknown";
  }

  return mt_simd_level_names[level];
}
