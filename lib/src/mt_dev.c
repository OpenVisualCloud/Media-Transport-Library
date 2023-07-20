/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "mt_dev.h"

#include "mt_arp.h"
#include "mt_cni.h"
#include "mt_dhcp.h"
#include "mt_log.h"
#include "mt_mcast.h"
#include "mt_queue.h"
#include "mt_sch.h"
#include "mt_socket.h"
#include "mt_stat.h"
#include "mt_util.h"

static struct mt_rx_flow_rsp* dev_if_create_rx_flow(struct mt_interface* inf, uint16_t q,
                                                    struct mt_rxq_flow* flow);
static int dev_if_free_rx_flow(struct mt_interface* inf, struct mt_rx_flow_rsp* rsp);

struct mt_dev_driver_info {
  char* name;
  enum mt_port_type port_type;
  enum mt_driver_type drv_type;
  enum mt_flow_type flow_type;
};

static const struct mt_dev_driver_info dev_drvs[] = {
    {
        .name = "net_ice",
        .port_type = MT_PORT_PF,
        .drv_type = MT_DRV_ICE,
        .flow_type = MT_FLOW_ALL,
    },
    {
        .name = "net_i40e",
        .port_type = MT_PORT_PF,
        .drv_type = MT_DRV_I40E,
        .flow_type = MT_FLOW_ALL,
    },
    {
        .name = "net_iavf",
        .port_type = MT_PORT_VF,
        .drv_type = MT_DRV_IAVF,
        .flow_type = MT_FLOW_ALL,
    },
    {
        .name = "net_af_xdp",
        .port_type = MT_PORT_AF_XDP,
        .drv_type = MT_DRV_AF_XDP,
        .flow_type = MT_FLOW_ALL,
    },
    {
        .name = "net_e1000_igb",
        .port_type = MT_PORT_PF,
        .drv_type = MT_DRV_E1000_IGB,
        .flow_type = MT_FLOW_ALL,
    },
    {
        .name = "net_igc",
        .port_type = MT_PORT_PF,
        .drv_type = MT_DRV_IGC,
        .flow_type = MT_FLOW_NO_IP,
    },
    {
        .name = "net_ena",
        .port_type = MT_PORT_VF,
        .drv_type = MT_DRV_ENA,
        .flow_type = MT_FLOW_NONE,
    },
};

static int parse_driver_info(const char* driver, enum mt_port_type* port,
                             enum mt_driver_type* drv, enum mt_flow_type* flow) {
  for (int i = 0; i < MTL_ARRAY_SIZE(dev_drvs); i++) {
    if (!strcmp(dev_drvs[i].name, driver)) {
      *port = dev_drvs[i].port_type;
      *drv = dev_drvs[i].drv_type;
      *flow = dev_drvs[i].flow_type;
      return 0;
    }
  }

  err("%s, unknown nic driver %s\n", __func__, driver);
  return -EIO;
}

static void dev_eth_xstat(uint16_t port_id) {
  /* Get count */
  int cnt = rte_eth_xstats_get_names(port_id, NULL, 0);
  if (cnt < 0) {
    err("%s(%u), get names fail\n", __func__, port_id);
    return;
  }

  /* Get id-name lookup table */
  struct rte_eth_xstat_name names[cnt];
  memset(names, 0, cnt * sizeof(names[0]));
  if (cnt != rte_eth_xstats_get_names(port_id, &names[0], cnt)) {
    err("%s(%u), get cnt names fail\n", __func__, port_id);
    return;
  }

  /* Get stats themselves */
  struct rte_eth_xstat xstats[cnt];
  memset(xstats, 0, cnt * sizeof(xstats[0]));
  if (cnt != rte_eth_xstats_get(port_id, &xstats[0], cnt)) {
    err("%s(%u), cnt mismatch\n", __func__, port_id);
    return;
  }

  /* Display xstats, err level since this called only with error case */
  for (int i = 0; i < cnt; i++) {
    if (xstats[i].value) {
      err("%s: %" PRIu64 "\n", names[i].name, xstats[i].value);
    }
  }
}

static inline void diff_and_update(uint64_t* new, uint64_t* old) {
  uint64_t temp = *new;
  *new -= *old;
  *old = temp;
}

static int dev_inf_stat(void* pri) {
  struct mt_interface* inf = pri;
  enum mtl_port port = inf->port;
  uint16_t port_id = inf->port_id;
  enum mt_port_type port_type = inf->port_type;
  struct rte_eth_stats stats;

  if (rte_eth_stats_get(port_id, &stats) == 0) {
    struct mt_dev_stats* dev_stats = inf->dev_stats;
    if (dev_stats) {
      diff_and_update(&stats.ipackets, &dev_stats->rx_pkts);
      diff_and_update(&stats.opackets, &dev_stats->tx_pkts);
      diff_and_update(&stats.ibytes, &dev_stats->rx_bytes);
      diff_and_update(&stats.obytes, &dev_stats->tx_bytes);
      diff_and_update(&stats.ierrors, &dev_stats->rx_errors);
      diff_and_update(&stats.oerrors, &dev_stats->tx_errors);
      diff_and_update(&stats.imissed, &dev_stats->rx_missed);
      diff_and_update(&stats.rx_nombuf, &dev_stats->rx_nombuf);
    }

    double orate_m =
        (double)stats.obytes * 8 / MT_DEV_STAT_INTERVAL_S / MT_DEV_STAT_M_UNIT;
    double irate_m =
        (double)stats.ibytes * 8 / MT_DEV_STAT_INTERVAL_S / MT_DEV_STAT_M_UNIT;

    notice("DEV(%d): Avr rate, tx: %f Mb/s, rx: %f Mb/s, pkts, tx: %" PRIu64
           ", rx: %" PRIu64 "\n",
           port, orate_m, irate_m, stats.opackets, stats.ipackets);
    if (stats.imissed || stats.ierrors || stats.rx_nombuf ||
        (stats.oerrors && (MT_PORT_VF != port_type))) {
      err("DEV(%d): Status: imissed %" PRIu64 " ierrors %" PRIu64 " oerrors %" PRIu64
          " rx_nombuf %" PRIu64 "\n",
          port, stats.imissed, stats.ierrors, stats.oerrors, stats.rx_nombuf);
      dev_eth_xstat(port_id);
    }

    if (!dev_stats) {
      rte_eth_stats_reset(port_id);
      rte_eth_xstats_reset(port_id);
    }
  }

  return 0;
}

static void dev_stat(struct mtl_main_impl* impl) {
  struct mtl_init_params* p = mt_get_user_params(impl);

  if (mt_in_reset(impl)) {
    notice("* *    M T    D E V   I N   R E S E T   * * \n");
    return;
  }

  notice("* *    M T    D E V   S T A T E   * * \n");
  if (p->stat_dump_cb_fn) p->stat_dump_cb_fn(p->priv);
  mt_stat_dump(impl);
  notice("* *    E N D    S T A T E   * * \n\n");
}

static void dev_stat_wakeup_thread(struct mtl_main_impl* impl) {
  mt_pthread_mutex_lock(&impl->stat_wake_mutex);
  mt_pthread_cond_signal(&impl->stat_wake_cond);
  mt_pthread_mutex_unlock(&impl->stat_wake_mutex);
}

static void dev_stat_alarm_handler(void* param) {
  struct mtl_main_impl* impl = param;

  if (impl->stat_tid)
    dev_stat_wakeup_thread(impl);
  else
    dev_stat(impl);

  rte_eal_alarm_set(MT_DEV_STAT_INTERVAL_US(mt_get_user_params(impl)->dump_period_s),
                    dev_stat_alarm_handler, impl);
}

static void* dev_stat_thread(void* arg) {
  struct mtl_main_impl* impl = arg;

  info("%s, start\n", __func__);
  while (rte_atomic32_read(&impl->stat_stop) == 0) {
    mt_pthread_mutex_lock(&impl->stat_wake_mutex);
    if (!rte_atomic32_read(&impl->stat_stop))
      mt_pthread_cond_wait(&impl->stat_wake_cond, &impl->stat_wake_mutex);
    mt_pthread_mutex_unlock(&impl->stat_wake_mutex);

    if (!rte_atomic32_read(&impl->stat_stop)) dev_stat(impl);
  }
  info("%s, stop\n", __func__);

  return NULL;
}

struct dev_eal_init_args {
  int argc;
  char** argv;
  int result;
};

static void* dev_eal_init_thread(void* arg) {
  struct dev_eal_init_args* init = arg;

  dbg("%s, start\n", __func__);
  init->result = rte_eal_init(init->argc, init->argv);
  return NULL;
}

static int dev_eal_init(struct mtl_init_params* p, struct mt_kport_info* kport_info) {
  char* argv[MT_EAL_MAX_ARGS];
  int argc, ret;
  int num_ports = RTE_MIN(p->num_ports, MTL_PORT_MAX);
  static bool eal_initted = false; /* eal cann't re-enter in one process */
  bool has_afxdp = false;
  char port_params[MTL_PORT_MAX][2 * MTL_PORT_MAX_LEN];
  char* port_param;
  int pci_ports = 0;

  argc = 0;

  argv[argc] = MT_DPDK_LIB_NAME;
  argc++;
#ifndef WINDOWSENV
  argv[argc] = "--file-prefix";
  argc++;
  argv[argc] = MT_DPDK_LIB_NAME;
  argc++;
  argv[argc] = "--match-allocations";
  argc++;
#endif
  argv[argc] = "--in-memory";
  argc++;

  for (int i = 0; i < num_ports; i++) {
    if (p->pmd[i] == MTL_PMD_DPDK_AF_XDP) {
      argv[argc] = "--vdev";
      has_afxdp = true;
    } else {
      argv[argc] = "-a";
      pci_ports++;
    }
    argc++;
    port_param = port_params[i];
    memset(port_param, 0, 2 * MTL_PORT_MAX_LEN);
    if (p->pmd[i] == MTL_PMD_DPDK_AF_XDP) {
      snprintf(port_param, 2 * MTL_PORT_MAX_LEN,
               "net_af_xdp%d,iface=%s,start_queue=%u,queue_count=%u", i, p->port[i],
               p->xdp_info[i].start_queue, p->xdp_info[i].queue_count);
      /* save port name */
      snprintf(kport_info->port[i], MTL_PORT_MAX_LEN, "net_af_xdp%d", i);
    } else {
      snprintf(port_param, 2 * MTL_PORT_MAX_LEN, "%s", p->port[i]);
    }
    info("%s(%d), port_param: %s\n", __func__, i, port_param);
    argv[argc] = port_param;
    argc++;
  }

  /* amend dma dev port */
  uint8_t num_dma_dev_port = RTE_MIN(p->num_dma_dev_port, MTL_DMA_DEV_MAX);
  dbg("%s, dma dev no %u\n", __func__, p->num_dma_dev_port);
  for (uint8_t i = 0; i < num_dma_dev_port; i++) {
    argv[argc] = "-a";
    pci_ports++;
    argc++;
    argv[argc] = p->dma_dev_port[i];
    argc++;
  }

  if (p->lcores) {
    argv[argc] = "-l";
    argc++;
    info("%s, lcores: %s\n", __func__, p->lcores);
    argv[argc] = p->lcores;
    argc++;
  }

  if (!pci_ports) {
    argv[argc] = "--no-pci";
    argc++;
  }

  if (p->iova_mode > MTL_IOVA_MODE_AUTO && p->iova_mode < MTL_IOVA_MODE_MAX) {
    argv[argc] = "--iova-mode";
    argc++;
    if (p->iova_mode == MTL_IOVA_MODE_VA)
      argv[argc] = "va";
    else if (p->iova_mode == MTL_IOVA_MODE_PA)
      argv[argc] = "pa";
    argc++;
  }

  argv[argc] = "--log-level";
  argc++;
  if (p->log_level == MTL_LOG_LEVEL_DEBUG) {
    argv[argc] = "user,debug";
  } else if (p->log_level == MTL_LOG_LEVEL_INFO) {
    if (has_afxdp)
      argv[argc] = "pmd.net.af_xdp,info";
    else
      argv[argc] = "info";
  } else if (p->log_level == MTL_LOG_LEVEL_NOTICE) {
    argv[argc] = "notice";
  } else if (p->log_level == MTL_LOG_LEVEL_WARNING) {
    argv[argc] = "warning";
  } else if (p->log_level == MTL_LOG_LEVEL_ERROR) {
    argv[argc] = "error";
  } else {
    argv[argc] = "info";
  }
  argc++;

  if (p->flags & MTL_FLAG_RXTX_SIMD_512) {
    argv[argc] = "--force-max-simd-bitwidth=512";
    argc++;
  }

  argv[argc] = "--";
  argc++;

  if (eal_initted) {
    info("%s, eal not support re-init\n", __func__);
    return -EIO;
  }

  bool init_use_thread = true;
  if (init_use_thread) {
    /* dpdk default pin CPU to main lcore in the call of rte_eal_init */
    struct dev_eal_init_args i_args;
    memset(&i_args, 0, sizeof(i_args));
    i_args.argc = argc;
    i_args.argv = argv;
    pthread_t eal_init_thread = 0;
    pthread_create(&eal_init_thread, NULL, dev_eal_init_thread, &i_args);
    info("%s, wait eal_init_thread done\n", __func__);
    pthread_join(eal_init_thread, NULL);
    ret = i_args.result;
  } else {
    ret = rte_eal_init(argc, argv);
  }
  if (ret < 0) return ret;

  eal_initted = true;

  return 0;
}

int dev_rx_runtime_queue_start(struct mtl_main_impl* impl, enum mtl_port port) {
  struct mt_interface* inf = mt_if(impl, port);
  int ret;
  struct mt_rx_queue* rx_queue;

  for (uint16_t q = 0; q < inf->max_rx_queues; q++) {
    rx_queue = &inf->rx_queues[q];
    if (rx_queue->active) {
      ret = rte_eth_dev_rx_queue_start(inf->port_id, q);
      if (ret < 0)
        err("%s(%d), start runtime rx queue %d fail %d\n", __func__, port, q, ret);
    }
  }

  return 0;
}

/* flush all the old bufs in the rx queue already */
static int dev_flush_rx_queue(struct mt_interface* inf, struct mt_rx_queue* queue) {
  int mbuf_size = 128;
  int loop = inf->nb_rx_desc / mbuf_size;
  struct rte_mbuf* mbuf[mbuf_size];
  uint16_t rv;

  for (int i = 0; i < loop; i++) {
    rv = mt_dev_rx_burst(queue, &mbuf[0], mbuf_size);
    if (rv) rte_pktmbuf_free_bulk(&mbuf[0], rv);
  }

  return 0;
}

#define ST_SHAPER_PROFILE_ID 1
#define ST_ROOT_NODE_ID 256
#define ST_DEFAULT_NODE_ID 246
#define ST_DEFAULT_RL_BPS (1024 * 1024 * 1024 / 8) /* 1g bit per second */

static int dev_rl_init_root(struct mt_interface* inf, uint32_t shaper_profile_id) {
  uint16_t port_id = inf->port_id;
  enum mtl_port port = inf->port;
  int ret;
  struct rte_tm_error error;
  struct rte_tm_node_params np;

  if (inf->tx_rl_root_active) return 0;

  memset(&error, 0, sizeof(error));
  memset(&np, 0, sizeof(np));

  /* root node */
  np.shaper_profile_id = shaper_profile_id;
  np.nonleaf.n_sp_priorities = 1;
  ret = rte_tm_node_add(port_id, ST_ROOT_NODE_ID, -1, 0, 1, 0, &np, &error);
  if (ret < 0) {
    err("%s(%d), root add error: (%d)%s\n", __func__, port, ret,
        mt_msg_safe(error.message));
    return ret;
  }

  /* nonleaf node based on root */
  ret =
      rte_tm_node_add(port_id, ST_DEFAULT_NODE_ID, ST_ROOT_NODE_ID, 0, 1, 1, &np, &error);
  if (ret < 0) {
    err("%s(%d), node add error: (%d)%s\n", __func__, port, ret,
        mt_msg_safe(error.message));
    return ret;
  }

  inf->tx_rl_root_active = true;
  return 0;
}

static struct mt_rl_shaper* dev_rl_shaper_add(struct mt_interface* inf, uint64_t bps) {
  struct mt_rl_shaper* shapers = &inf->tx_rl_shapers[0];
  uint16_t port_id = inf->port_id;
  enum mtl_port port = inf->port;
  int ret;
  struct rte_tm_error error;
  struct rte_tm_shaper_params sp;
  uint32_t shaper_profile_id;

  memset(&error, 0, sizeof(error));

  for (int i = 0; i < MT_MAX_RL_ITEMS; i++) {
    if (shapers[i].rl_bps) continue;

    shaper_profile_id = ST_SHAPER_PROFILE_ID + i;

    /* shaper profile with bandwidth */
    memset(&sp, 0, sizeof(sp));
    sp.peak.rate = bps;
    ret = rte_tm_shaper_profile_add(port_id, shaper_profile_id, &sp, &error);
    if (ret < 0) {
      err("%s(%d), shaper add error: (%d)%s\n", __func__, port, ret,
          mt_msg_safe(error.message));
      return NULL;
    }

    ret = dev_rl_init_root(inf, shaper_profile_id);
    if (ret < 0) {
      err("%s(%d), root init error %d\n", __func__, port, ret);
      rte_tm_shaper_profile_delete(port_id, shaper_profile_id, &error);
      return NULL;
    }

    info("%s(%d), bps %" PRIu64 " on shaper %d\n", __func__, port, bps,
         shaper_profile_id);
    shapers[i].rl_bps = bps;
    shapers[i].shaper_profile_id = shaper_profile_id;
    shapers[i].idx = i;
    return &shapers[i];
  }

  err("%s(%d), no space\n", __func__, port);
  return NULL;
}

static struct mt_rl_shaper* dev_rl_shaper_get(struct mt_interface* inf, uint64_t bps) {
  struct mt_rl_shaper* shapers = &inf->tx_rl_shapers[0];

  for (int i = 0; i < MT_MAX_RL_ITEMS; i++) {
    if (bps == shapers[i].rl_bps) return &shapers[i];
  }

  return dev_rl_shaper_add(inf, bps);
}

static int dev_init_ratelimit_vf(struct mt_interface* inf) {
  uint16_t port_id = inf->port_id;
  enum mtl_port port = inf->port;
  int ret;
  struct rte_tm_error error;
  struct rte_tm_node_params qp;
  struct mt_rl_shaper* shaper;
  uint64_t bps = ST_DEFAULT_RL_BPS;

  memset(&error, 0, sizeof(error));

  struct mt_tx_queue* tx_queue;
  for (uint16_t q = 0; q < inf->max_tx_queues; q++) {
    tx_queue = &inf->tx_queues[q];

    shaper = dev_rl_shaper_get(inf, bps);
    if (!shaper) {
      err("%s(%d), rl shaper get fail for q %d\n", __func__, port, q);
      return -EIO;
    }
    memset(&qp, 0, sizeof(qp));
    qp.shaper_profile_id = shaper->shaper_profile_id;
    qp.leaf.cman = RTE_TM_CMAN_TAIL_DROP;
    qp.leaf.wred.wred_profile_id = RTE_TM_WRED_PROFILE_ID_NONE;
    ret = rte_tm_node_add(port_id, q, ST_DEFAULT_NODE_ID, 0, 1, 2, &qp, &error);
    if (ret < 0) {
      err("%s(%d), q %d add fail %d(%s)\n", __func__, port, q, ret,
          mt_msg_safe(error.message));
      return ret;
    }
    tx_queue->rl_shapers_mapping = shaper->idx;
    tx_queue->bps = bps;
    info("%s(%d), q %d link to shaper id %d\n", __func__, port, q,
         shaper->shaper_profile_id);
  }

  ret = rte_tm_hierarchy_commit(port_id, 1, &error);
  if (ret < 0)
    err("%s(%d), commit error (%d)%s\n", __func__, port, ret, mt_msg_safe(error.message));

  dbg("%s(%d), succ\n", __func__, port);
  return ret;
}

static int dev_tx_queue_set_rl_rate(struct mt_interface* inf, uint16_t queue,
                                    uint64_t bytes_per_sec) {
  uint16_t port_id = inf->port_id;
  enum mtl_port port = inf->port;
  struct mt_tx_queue* tx_queue = &inf->tx_queues[queue];
  uint64_t bps = bytes_per_sec;
  int ret;
  struct rte_tm_error error;
  struct rte_tm_node_params qp;
  struct mt_rl_shaper* shaper;

  memset(&error, 0, sizeof(error));

  if (!bps) { /* default */
    bps = ST_DEFAULT_RL_BPS;
  }

  /* not changed */
  if (bps == tx_queue->bps) return 0;

  /* delete old queue node */
  if (tx_queue->rl_shapers_mapping >= 0) {
    ret = rte_tm_node_delete(port_id, queue, &error);
    if (ret < 0) {
      err("%s(%d), node %d delete fail %d(%s)\n", __func__, port, queue, ret,
          mt_msg_safe(error.message));
      return ret;
    }
    tx_queue->rl_shapers_mapping = -1;
  }

  if (bps) {
    shaper = dev_rl_shaper_get(inf, bps);
    if (!shaper) {
      err("%s(%d), rl shaper get fail for q %d\n", __func__, port, queue);
      return -EIO;
    }
    memset(&qp, 0, sizeof(qp));
    qp.shaper_profile_id = shaper->shaper_profile_id;
    qp.leaf.cman = RTE_TM_CMAN_TAIL_DROP;
    qp.leaf.wred.wred_profile_id = RTE_TM_WRED_PROFILE_ID_NONE;
    ret = rte_tm_node_add(port_id, queue, ST_DEFAULT_NODE_ID, 0, 1, 2, &qp, &error);
    if (ret < 0) {
      err("%s(%d), q %d add fail %d(%s)\n", __func__, port, queue, ret,
          mt_msg_safe(error.message));
      return ret;
    }
    tx_queue->rl_shapers_mapping = shaper->idx;
    info("%s(%d), q %d link to shaper id %d(%" PRIu64 ")\n", __func__, port, queue,
         shaper->shaper_profile_id, shaper->rl_bps);
  }

  mt_pthread_mutex_lock(&inf->vf_cmd_mutex);
  ret = rte_tm_hierarchy_commit(port_id, 1, &error);
  mt_pthread_mutex_unlock(&inf->vf_cmd_mutex);
  if (ret < 0) {
    err("%s(%d), commit error (%d)%s\n", __func__, port, ret, mt_msg_safe(error.message));
    return ret;
  }

  tx_queue->bps = bps;

  return 0;
}

static struct rte_flow* dev_rx_queue_create_flow_raw(struct mt_interface* inf, uint16_t q,
                                                     struct mt_rxq_flow* flow) {
  struct rte_flow_error error;
  struct rte_flow* r_flow;

  struct rte_flow_attr attr = {0};
  struct rte_flow_item pattern[2];
  struct rte_flow_action action[2];
  struct rte_flow_item_raw spec = {0};
  struct rte_flow_item_raw mask = {0};
  struct rte_flow_action_queue to_queue = {0};

  uint16_t port_id = inf->port_id;
  char pkt_buf[] =
      "0000000000010000000000020800450000300000000000110000010101010202020200001B3A001C00"
      "008000000000000000000000000000000000000000";
  char msk_buf[] =
      "000000000000000000000000000000000000000000000000000000000000000000000000FFFF000000"
      "000000000000000000000000000000000000000000";

  attr.ingress = 1;

  memset(&error, 0, sizeof(error));
  memset(pattern, 0, sizeof(pattern));
  memset(action, 0, sizeof(action));

  spec.pattern = (const void*)pkt_buf;
  spec.length = 62;
  mask.pattern = (const void*)msk_buf;
  mask.length = 62;

  pattern[0].type = RTE_FLOW_ITEM_TYPE_RAW;
  pattern[0].spec = &spec;
  pattern[0].mask = &mask;
  pattern[1].type = RTE_FLOW_ITEM_TYPE_END;

  to_queue.index = q;
  action[0].type = RTE_FLOW_ACTION_TYPE_QUEUE;
  action[0].conf = &to_queue;
  action[1].type = RTE_FLOW_ACTION_TYPE_END;

  mt_pthread_mutex_lock(&inf->vf_cmd_mutex);
  r_flow = rte_flow_create(port_id, &attr, pattern, action, &error);
  mt_pthread_mutex_unlock(&inf->vf_cmd_mutex);
  if (!r_flow) {
    err("%s(%d), rte_flow_create fail for queue %d, %s\n", __func__, port_id, q,
        mt_msg_safe(error.message));
    return NULL;
  }

  info("%s(%d), queue %u succ\n", __func__, inf->port, q);
  return r_flow;
}

static struct rte_flow* dev_rx_queue_create_flow(struct mt_interface* inf, uint16_t q,
                                                 struct mt_rxq_flow* flow) {
  struct rte_flow_attr attr;
  struct rte_flow_item pattern[4];
  struct rte_flow_action action[2];
  struct rte_flow_action_queue queue;
  struct rte_flow_item_eth eth_spec;
  struct rte_flow_item_eth eth_mask;
  struct rte_flow_item_ipv4 ipv4_spec;
  struct rte_flow_item_ipv4 ipv4_mask;
  struct rte_flow_item_udp udp_spec;
  struct rte_flow_item_udp udp_mask;
  struct rte_flow_error error;
  struct rte_flow* r_flow;
  int ret;
  bool has_ip_flow = true;
  bool has_port_flow = true;

  uint16_t port_id = inf->port_id;

  memset(&error, 0, sizeof(error));

  /* drv not support ip flow */
  if (inf->flow_type == MT_FLOW_NO_IP) has_ip_flow = false;
  /* no ip flow requested */
  if (flow->no_ip_flow) has_ip_flow = false;
  /* no port flow requested */
  if (flow->no_port_flow) has_port_flow = false;

  /* only raw flow can be applied on the hdr split queue */
  if (mt_if_hdr_split_pool(inf, q)) {
    return dev_rx_queue_create_flow_raw(inf, q, flow);
  }

  /* queue */
  queue.index = q;

  /* nothing for eth flow */
  memset(&eth_spec, 0, sizeof(eth_spec));
  memset(&eth_mask, 0, sizeof(eth_mask));

  /* ipv4 flow */
  memset(&ipv4_spec, 0, sizeof(ipv4_spec));
  memset(&ipv4_mask, 0, sizeof(ipv4_mask));
  ipv4_spec.hdr.next_proto_id = IPPROTO_UDP;

  if (has_ip_flow) {
    memset(&ipv4_mask.hdr.dst_addr, 0xFF, MTL_IP_ADDR_LEN);
    if (mt_is_multicast_ip(flow->dip_addr)) {
      rte_memcpy(&ipv4_spec.hdr.dst_addr, flow->dip_addr, MTL_IP_ADDR_LEN);
    } else {
      rte_memcpy(&ipv4_spec.hdr.src_addr, flow->dip_addr, MTL_IP_ADDR_LEN);
      rte_memcpy(&ipv4_spec.hdr.dst_addr, flow->sip_addr, MTL_IP_ADDR_LEN);
      memset(&ipv4_mask.hdr.src_addr, 0xFF, MTL_IP_ADDR_LEN);
    }
  }

  /* udp port flow */
  if (has_port_flow) {
    memset(&udp_spec, 0, sizeof(udp_spec));
    memset(&udp_mask, 0, sizeof(udp_mask));
    udp_spec.hdr.dst_port = htons(flow->dst_port);
    udp_mask.hdr.dst_port = htons(0xFFFF);
  }

  memset(&attr, 0, sizeof(attr));
  attr.ingress = 1;

  memset(action, 0, sizeof(action));
  action[0].type = RTE_FLOW_ACTION_TYPE_QUEUE;
  action[0].conf = &queue;
  action[1].type = RTE_FLOW_ACTION_TYPE_END;

  memset(pattern, 0, sizeof(pattern));
  pattern[0].type = RTE_FLOW_ITEM_TYPE_ETH;
  pattern[0].spec = has_ip_flow ? &eth_spec : NULL;
  pattern[0].mask = has_ip_flow ? &eth_mask : NULL;
  pattern[1].type = RTE_FLOW_ITEM_TYPE_IPV4;
  pattern[1].spec = &ipv4_spec;
  pattern[1].mask = &ipv4_mask;
  if (has_port_flow) {
    pattern[2].type = RTE_FLOW_ITEM_TYPE_UDP;
    pattern[2].spec = &udp_spec;
    pattern[2].mask = &udp_mask;
    pattern[3].type = RTE_FLOW_ITEM_TYPE_END;
  } else {
    pattern[2].type = RTE_FLOW_ITEM_TYPE_END;
  }

  ret = rte_flow_validate(port_id, &attr, pattern, action, &error);
  if (ret < 0) {
    err("%s(%d), rte_flow_validate fail %d for queue %d, %s\n", __func__, port_id, ret, q,
        mt_msg_safe(error.message));
    return NULL;
  }

  mt_pthread_mutex_lock(&inf->vf_cmd_mutex);
  r_flow = rte_flow_create(port_id, &attr, pattern, action, &error);
  mt_pthread_mutex_unlock(&inf->vf_cmd_mutex);
  if (!r_flow) {
    err("%s(%d), rte_flow_create fail for queue %d, %s\n", __func__, port_id, q,
        mt_msg_safe(error.message));
    return NULL;
  }

  uint8_t* ip = flow->dip_addr;
  info("%s(%d), queue %u succ, ip %u.%u.%u.%u port %u\n", __func__, inf->port, q, ip[0],
       ip[1], ip[2], ip[3], flow->dst_port);
  return r_flow;
}

static int dev_stop_port(struct mt_interface* inf) {
  int ret;
  uint16_t port_id = inf->port_id;
  enum mtl_port port = inf->port;

  if (!(inf->status & MT_IF_STAT_PORT_STARTED)) {
    info("%s(%d), port not started\n", __func__, port);
    return 0;
  }

  ret = rte_eth_dev_stop(port_id);
  if (ret < 0) err("%s(%d), rte_eth_dev_stop fail %d\n", __func__, port, ret);

  inf->status &= ~MT_IF_STAT_PORT_STARTED;
  info("%s(%d), succ\n", __func__, port);
  return 0;
}

static int dev_close_port(struct mt_interface* inf) {
  int ret;
  uint16_t port_id = inf->port_id;
  enum mtl_port port = inf->port;

  if (!(inf->status & MT_IF_STAT_PORT_CONFIGURED)) {
    info("%s(%d), port not started\n", __func__, port);
    return 0;
  }

  ret = rte_eth_dev_close(port_id);
  if (ret < 0) err("%s(%d), rte_eth_dev_close fail %d\n", __func__, port, ret);

  inf->status &= ~MT_IF_STAT_PORT_CONFIGURED;
  info("%s(%d), succ\n", __func__, port);
  return 0;
}

static int dev_detect_link(struct mt_interface* inf) {
  /* get link speed for the port */
  struct rte_eth_link eth_link;
  uint16_t port_id = inf->port_id;
  enum mtl_port port = inf->port;

  memset(&eth_link, 0, sizeof(eth_link));

  for (int i = 0; i < 100; i++) {
    rte_eth_link_get_nowait(port_id, &eth_link);
    if (eth_link.link_status) {
      inf->link_speed = eth_link.link_speed;
      mt_eth_link_dump(port_id);
      return 0;
    }
    mt_sleep_ms(100); /* only happen on CVL PF */
  }

  mt_eth_link_dump(port_id);
  err("%s(%d), link not connected for %s\n", __func__, port,
      mt_get_user_params(inf->parent)->port[port]);
  return -EIO;
}

static int dev_start_timesync(struct mt_interface* inf) {
  int ret, i = 0, max_retry = 10;
  uint16_t port_id = inf->port_id;
  enum mtl_port port = inf->port;
  struct timespec spec;

  for (i = 0; i < max_retry; i++) {
    ret = rte_eth_timesync_enable(port_id);
    if (ret < 0) {
      err("%s(%d), rte_eth_timesync_enable fail %d\n", __func__, port, ret);
      return ret;
    }

    memset(&spec, 0, sizeof(spec));
    ret = rte_eth_timesync_read_time(port_id, &spec);
    if (ret < 0) {
      err("%s(%d), rte_eth_timesync_read_time fail %d\n", __func__, port, ret);
      return ret;
    }
    if (spec.tv_sec || spec.tv_nsec) {
      /* read and print time */
      rte_eth_timesync_read_time(port_id, &spec);
      struct tm t;
      char date_time[64];
      localtime_r(&spec.tv_sec, &t);
      strftime(date_time, sizeof(date_time), "%Y-%m-%d %H:%M:%S", &t);
      info("%s(%d), init ptp time %s, i %d\n", __func__, port, date_time, i);
      break;
    }
    dbg("%s(%d), tv_sec %" PRIu64 " tv_nsec %" PRIu64 ", i %d\n", __func__, port,
        spec.tv_sec, spec.tv_nsec, i);
    mt_sleep_ms(10);
  }
  if (i >= max_retry) {
    err("%s(%d), fail to get read time\n", __func__, port);
    return -EIO;
  }

  return 0;
}

static const struct rte_eth_conf dev_port_conf = {.txmode = {
                                                      .offloads = 0,
                                                  }};

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

/* 1:1 map with hash % reta_size % max_rx_queues */
static int dev_config_rss_reta(struct mt_interface* inf) {
  enum mtl_port port = inf->port;
  uint16_t reta_size = inf->dev_info.reta_size;
  int reta_group_size = reta_size / RTE_ETH_RETA_GROUP_SIZE;
  struct rte_eth_rss_reta_entry64 entries[reta_group_size];
  int ret;

  memset(entries, 0, sizeof(entries));
  for (int i = 0; i < reta_group_size; i++) {
    entries[i].mask = UINT64_MAX;
    for (int j = 0; j < RTE_ETH_RETA_GROUP_SIZE; j++) {
      entries[i].reta[j] = (i * RTE_ETH_RETA_GROUP_SIZE + j) % inf->max_rx_queues;
    }
  }
  ret = rte_eth_dev_rss_reta_update(inf->port_id, entries, reta_size);
  if (ret < 0) {
    err("%s(%d), rss reta update fail %d\n", __func__, port, ret);
    return ret;
  }

  info("%s(%d), reta size %u\n", __func__, port, reta_size);
  return 0;
}

static int dev_config_port(struct mt_interface* inf) {
  struct mtl_main_impl* impl = inf->parent;
  uint16_t port_id = inf->port_id;
  enum mtl_port port = inf->port;
  uint16_t nb_rx_desc = MT_DEV_RX_DESC, nb_tx_desc = MT_DEV_TX_DESC;
  int ret;
  struct mtl_init_params* p = mt_get_user_params(impl);
  uint16_t nb_rx_q = inf->max_rx_queues, nb_tx_q = inf->max_tx_queues;
  struct rte_eth_conf port_conf = dev_port_conf;

  if (inf->feature & MT_IF_FEATURE_TX_MULTI_SEGS) {
#if RTE_VERSION >= RTE_VERSION_NUM(22, 3, 0, 0)
    port_conf.txmode.offloads |= RTE_ETH_TX_OFFLOAD_MULTI_SEGS;
#else
    port_conf.txmode.offloads |= DEV_TX_OFFLOAD_MULTI_SEGS;
#endif
  }

  if (inf->feature & MT_IF_FEATURE_TX_OFFLOAD_IPV4_CKSUM) {
#if RTE_VERSION >= RTE_VERSION_NUM(22, 3, 0, 0)
    port_conf.txmode.offloads |= RTE_ETH_TX_OFFLOAD_IPV4_CKSUM;
#else
    port_conf.txmode.offloads |= DEV_TX_OFFLOAD_IPV4_CKSUM;
#endif
  }

  if (inf->feature & MT_IF_FEATURE_RX_OFFLOAD_TIMESTAMP) {
#if RTE_VERSION >= RTE_VERSION_NUM(22, 3, 0, 0)
    port_conf.rxmode.offloads |= RTE_ETH_RX_OFFLOAD_TIMESTAMP;
#else
    port_conf.rxmode.offloads |= DEV_RX_OFFLOAD_TIMESTAMP;
#endif
  }

  dbg("%s(%d), rss mode %d\n", __func__, port, inf->rss_mode);
  if (mt_has_srss(impl, port)) {
    struct rte_eth_rss_conf* rss_conf;
    rss_conf = &port_conf.rx_adv_conf.rss_conf;

    rss_conf->rss_key = mt_rss_hash_key;
    rss_conf->rss_key_len = MT_HASH_KEY_LENGTH;
    if (inf->rss_mode == MTL_RSS_MODE_L3) {
      rss_conf->rss_hf = RTE_ETH_RSS_IPV4;
    } else if (inf->rss_mode == MTL_RSS_MODE_L3_L4) {
      rss_conf->rss_hf = RTE_ETH_RSS_NONFRAG_IPV4_UDP;
    } else {
      err("%s(%d), not support rss_mode %d\n", __func__, port, inf->rss_mode);
      return -EIO;
    }
    if (rss_conf->rss_hf != (inf->dev_info.flow_type_rss_offloads & rss_conf->rss_hf)) {
      err("%s(%d), not support rss offload %" PRIx64 ", mode %d\n", __func__, port,
          rss_conf->rss_hf, inf->rss_mode);
      return -EIO;
    }
    port_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_RSS;
  }

  ret = rte_eth_dev_configure(port_id, nb_rx_q, nb_tx_q, &port_conf);
  if (ret < 0) {
    err("%s(%d), rte_eth_dev_configure fail %d\n", __func__, port, ret);
    return ret;
  }

  /* apply if user has rx_tx_desc config */
  if (p->nb_tx_desc) nb_tx_desc = p->nb_tx_desc;
  if (p->nb_rx_desc) nb_rx_desc = p->nb_rx_desc;

  ret = rte_eth_dev_adjust_nb_rx_tx_desc(port_id, &nb_rx_desc, &nb_tx_desc);
  if (ret < 0) {
    err("%s(%d), rte_eth_dev_adjust_nb_rx_tx_desc fail %d\n", __func__, port, ret);
    return ret;
  }
  inf->nb_tx_desc = nb_tx_desc;
  inf->nb_rx_desc = nb_rx_desc;

  if (!mt_pmd_is_kernel(impl, port)) {
    /* enable PTYPE for packet classification by NIC */
    uint32_t ptypes[16];
    uint32_t set_ptypes[16];
    uint32_t ptype_mask = RTE_PTYPE_L2_ETHER_TIMESYNC | RTE_PTYPE_L2_ETHER_ARP |
                          RTE_PTYPE_L2_ETHER_VLAN | RTE_PTYPE_L2_ETHER_QINQ |
                          RTE_PTYPE_L4_ICMP | RTE_PTYPE_L3_IPV4 | RTE_PTYPE_L4_UDP |
                          RTE_PTYPE_L4_FRAG;
    int num_ptypes =
        rte_eth_dev_get_supported_ptypes(port_id, ptype_mask, ptypes, RTE_DIM(ptypes));
    for (int i = 0; i < num_ptypes; i++) {
      set_ptypes[i] = ptypes[i];
    }
    if (num_ptypes >= 5) {
      ret = rte_eth_dev_set_ptypes(port_id, ptype_mask, set_ptypes, num_ptypes);
      if (ret < 0) {
        err("%s(%d), rte_eth_dev_set_ptypes fail %d\n", __func__, port, ret);
        return ret;
      }
    } else {
      warn("%s(%d), failed to setup all ptype, only %d supported\n", __func__, port,
           num_ptypes);
    }
  }
  inf->net_proto = p->net_proto[port];

  inf->status |= MT_IF_STAT_PORT_CONFIGURED;

  info("%s(%d), tx_q(%d with %d desc) rx_q (%d with %d desc)\n", __func__, port, nb_tx_q,
       nb_tx_desc, nb_rx_q, nb_rx_desc);
  return 0;
}

static bool dev_pkt_valid(struct mt_interface* inf, uint16_t queue,
                          struct rte_mbuf* pkt) {
  uint32_t pkt_len = pkt->pkt_len;
  enum mtl_port port = inf->port;

  if ((pkt_len <= 16) || (pkt_len > MTL_MTU_MAX_BYTES)) {
    err("%s(%d:%u), invalid pkt_len %u at %p\n", __func__, port, queue, pkt_len, pkt);
    return false;
  }
  if (pkt->nb_segs > 2) {
    err("%s(%d:%u), invalid nb_segs %u at %p\n", __func__, port, queue, pkt->nb_segs,
        pkt);
    return false;
  }

  return true;
}

static uint16_t dev_tx_pkt_check(uint16_t port, uint16_t queue, struct rte_mbuf** pkts,
                                 uint16_t nb_pkts, void* priv) {
  struct mt_interface* inf = priv;

  for (uint16_t i = 0; i < nb_pkts; i++) {
    if (!dev_pkt_valid(inf, queue, pkts[i])) {
      /* should never happen, replace with dummy pkt */
      rte_pktmbuf_free(pkts[i]);
      pkts[i] = inf->pad;
    }
  }

  return nb_pkts;
}

static int dev_start_port(struct mt_interface* inf) {
  int ret;
  struct mtl_main_impl* impl = inf->parent;
  uint16_t port_id = inf->port_id;
  enum mtl_port port = inf->port;
  int socket_id = inf->socket_id;
  uint16_t nb_rx_q = inf->max_rx_queues, nb_tx_q = inf->max_tx_queues;
  uint16_t nb_rx_desc = mt_if_nb_rx_desc(impl, port);
  uint16_t nb_tx_desc = mt_if_nb_tx_desc(impl, port);
  uint8_t rx_deferred_start = 0;
  struct rte_eth_txconf tx_port_conf;
  struct rte_eth_rxconf rx_port_conf;

  if (inf->feature & MT_IF_FEATURE_RUNTIME_RX_QUEUE) rx_deferred_start = 1;
  rx_port_conf.rx_deferred_start = rx_deferred_start;

  struct rte_mempool* mbuf_pool;
  for (uint16_t q = 0; q < nb_rx_q; q++) {
    mbuf_pool = inf->rx_queues[q].mbuf_pool ? inf->rx_queues[q].mbuf_pool
                                            : mt_get_rx_mempool(impl, port);
    if (!mbuf_pool) {
      err("%s(%d), no mbuf_pool for queue %d\n", __func__, port, q);
      return -ENOMEM;
    }

    rx_port_conf = inf->dev_info.default_rxconf;

    rx_port_conf.offloads = 0;
    rx_port_conf.rx_nseg = 0;
    rx_port_conf.rx_seg = NULL;
    if (mt_if_hdr_split_pool(inf, q) && mt_if_has_hdr_split(impl, port)) {
#ifdef ST_HAS_DPDK_HDR_SPLIT
      rx_port_conf.offloads = RTE_ETH_RX_OFFLOAD_BUFFER_SPLIT;
      info("%s(%d), enable hdr split for queue %d\n", __func__, port, q);
      /* two segments */
      union rte_eth_rxseg rx_usegs[2] = {};
      struct rte_eth_rxseg_split* rx_seg;

      rx_seg = &rx_usegs[0].split;
#if RTE_VERSION >= RTE_VERSION_NUM(22, 11, 0, 0)
      rx_seg->proto_hdr =
          RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4_EXT_UNKNOWN | RTE_PTYPE_L4_UDP;
#else
      rx_seg->proto_hdr = RTE_PTYPE_L4_UDP;
#endif

      rx_seg->offset = 0;
      rx_seg->length = 0;
      rx_seg->mp = mbuf_pool;

      rx_seg = &rx_usegs[1].split;
      rx_seg->proto_hdr = 0;
      rx_seg->offset = 0;
      rx_seg->length = 0;
      rx_seg->mp = mt_if_hdr_split_pool(inf, q);

      rx_port_conf.rx_nseg = 2;
      rx_port_conf.rx_seg = rx_usegs;

      ret =
          rte_eth_rx_queue_setup(port_id, q, nb_rx_desc, socket_id, &rx_port_conf, NULL);
#else
      err("%s, no hdr split support for this dpdk build\n", __func__);
      return -ENOTSUP;
#endif
    } else {
      ret = rte_eth_rx_queue_setup(port_id, q, nb_rx_desc, socket_id, &rx_port_conf,
                                   mbuf_pool);
    }
    if (ret < 0) {
      err("%s(%d), rte_eth_rx_queue_setup fail %d for queue %d\n", __func__, port, ret,
          q);
      return ret;
    }
  }

  for (uint16_t q = 0; q < nb_tx_q; q++) {
    tx_port_conf = inf->dev_info.default_txconf;
    ret = rte_eth_tx_queue_setup(port_id, q, nb_tx_desc, socket_id, &tx_port_conf);
    if (ret < 0) {
      err("%s(%d), rte_eth_tx_queue_setup fail %d for queue %d\n", __func__, port, ret,
          q);
      return ret;
    }
  }
  if (mt_get_user_params(impl)->flags & MTL_FLAG_TX_NO_BURST_CHK) {
    info("%s(%d), no tx burst check\n", __func__, port);
  } else {
    for (uint16_t q = 0; q < nb_tx_q; q++)
      rte_eth_add_tx_callback(port_id, q, dev_tx_pkt_check, inf);
  }

  ret = rte_eth_dev_start(port_id);
  if (ret < 0) {
    err("%s(%d), rte_eth_dev_start fail %d\n", __func__, port, ret);
    return ret;
  }
  inf->status |= MT_IF_STAT_PORT_STARTED;

  if (mt_has_srss(impl, port)) {
    ret = dev_config_rss_reta(inf);
    if (ret < 0) {
      err("%s(%d), rss reta config fail %d\n", __func__, port, ret);
      return ret;
    }
  }

  if (mt_get_user_params(impl)->flags & MTL_FLAG_NIC_RX_PROMISCUOUS) {
    /* Enable RX in promiscuous mode if it's required. */
    warn("%s(%d), enable promiscuous\n", __func__, port);
    rte_eth_promiscuous_enable(port_id);
  }
  rte_eth_stats_reset(port_id); /* reset stats */

  info("%s(%d), rx_defer %d\n", __func__, port, rx_deferred_start);
  return 0;
}

int dev_reset_port(struct mtl_main_impl* impl, enum mtl_port port) {
  int ret;
  uint16_t port_id = mt_port_id(impl, port);
  struct mt_interface* inf = mt_if(impl, port);

  if (mt_started(impl)) {
    err("%s, only allowed when instance is in stop state\n", __func__);
    return -EIO;
  }

  rte_atomic32_set(&impl->instance_in_reset, 1);

  mt_cni_stop(impl);

  rte_eth_dev_reset(port_id);

  ret = dev_config_port(inf);
  if (ret < 0) {
    err("%s(%d), dev_config_port fail %d\n", __func__, port, ret);
    rte_atomic32_set(&impl->instance_in_reset, 0);
    return ret;
  }

  ret = dev_start_port(inf);
  if (ret < 0) {
    err("%s(%d), dev_start_port fail %d\n", __func__, port, ret);
    rte_atomic32_set(&impl->instance_in_reset, 0);
    return ret;
  }

  mt_cni_start(impl);

  /* clear rl status */
  for (uint16_t q = 0; q < inf->max_tx_queues; q++) {
    inf->tx_queues[q].rl_shapers_mapping = -1; /* init to invalid */
  }
  inf->tx_rl_root_active = false;
  struct mt_rl_shaper* shapers = &inf->tx_rl_shapers[0];
  memset(shapers, 0, sizeof(*shapers) * MT_MAX_RL_ITEMS);

  /* restore rte flow */
  struct mt_rx_queue* rx_queue;
  for (uint16_t rx_q = 0; rx_q < inf->max_rx_queues; rx_q++) {
    rx_queue = &inf->rx_queues[rx_q];
    if (rx_queue->flow_rsp) {
      mt_rte_free(rx_queue->flow_rsp);
      rx_queue->flow_rsp = dev_if_create_rx_flow(inf, rx_q, &rx_queue->flow);
      if (!rx_queue->flow_rsp) {
        err("%s(%d), restore flow fail for q %d\n", __func__, port, rx_q);
        rte_atomic32_set(&impl->instance_in_reset, 0);
        return -EIO;
      }
    }
  }
  /* restore mcast */
  mt_mcast_restore(impl, port);

  return 0;
}

static int dev_filelock_lock(struct mtl_main_impl* impl) {
  int fd = -1;
  if (access(MT_FLOCK_PATH, F_OK) < 0) {
    fd = open(MT_FLOCK_PATH, O_RDONLY | O_CREAT, 0666);
  } else {
    fd = open(MT_FLOCK_PATH, O_RDONLY);
  }

  if (fd < 0) {
    err("%s, failed to open %s, %s\n", __func__, MT_FLOCK_PATH, strerror(errno));
    return -EIO;
  }
  impl->lcore_lock_fd = fd;
  /* wait until locked */
  if (flock(fd, LOCK_EX) != 0) {
    err("%s, can not lock file\n", __func__);
    close(fd);
    impl->lcore_lock_fd = -1;
    return -EIO;
  }

  return 0;
}

static int dev_filelock_unlock(struct mtl_main_impl* impl) {
  int fd = impl->lcore_lock_fd;

  if (fd < 0) {
    err("%s, wrong lock file fd %d\n", __func__, fd);
    return -EIO;
  }

  if (flock(fd, LOCK_UN) != 0) {
    err("%s, can not unlock file\n", __func__);
    return -EIO;
  }
  close(fd);
  impl->lcore_lock_fd = -1;
  return 0;
}

int mt_dev_get_lcore(struct mtl_main_impl* impl, unsigned int* lcore) {
  unsigned int cur_lcore = 0;
  int ret;
  struct mt_lcore_shm* lcore_shm = impl->lcore_shm;

  ret = dev_filelock_lock(impl);
  if (ret < 0) {
    err("%s, dev_filelock_lock fail\n", __func__);
    return ret;
  }

  do {
    cur_lcore = rte_get_next_lcore(cur_lcore, 1, 0);

    if ((cur_lcore < RTE_MAX_LCORE) && mt_socket_match(rte_lcore_to_socket_id(cur_lcore),
                                                       mt_socket_id(impl, MTL_PORT_P))) {
      if (!lcore_shm->lcores_active[cur_lcore]) {
        *lcore = cur_lcore;
        lcore_shm->lcores_active[cur_lcore] = true;
        lcore_shm->used++;
        rte_atomic32_inc(&impl->lcore_cnt);
        impl->local_lcores_active[cur_lcore] = true;
        ret = dev_filelock_unlock(impl);
        info("%s, available lcore %d\n", __func__, cur_lcore);
        if (ret < 0) {
          err("%s, dev_filelock_unlock fail\n", __func__);
          return ret;
        }
        return 0;
      }
    }
  } while (cur_lcore < RTE_MAX_LCORE);

  dev_filelock_unlock(impl);
  err("%s, fail to find lcore\n", __func__);
  return -EIO;
}

int mt_dev_put_lcore(struct mtl_main_impl* impl, unsigned int lcore) {
  int ret;
  struct mt_lcore_shm* lcore_shm = impl->lcore_shm;

  if (lcore >= RTE_MAX_LCORE) {
    err("%s, invalid lcore %d\n", __func__, lcore);
    return -EIO;
  }
  if (!lcore_shm) {
    err("%s, no lcore shm attached\n", __func__);
    return -EIO;
  }
  ret = dev_filelock_lock(impl);
  if (ret < 0) {
    err("%s, dev_filelock_lock fail\n", __func__);
    return ret;
  }
  if (!lcore_shm->lcores_active[lcore]) {
    err("%s, lcore %d not active\n", __func__, lcore);
    ret = -EIO;
    goto err_unlock;
  }

  lcore_shm->lcores_active[lcore] = false;
  lcore_shm->used--;
  rte_atomic32_dec(&impl->lcore_cnt);
  impl->local_lcores_active[lcore] = false;
  ret = dev_filelock_unlock(impl);
  info("%s, lcore %d\n", __func__, lcore);
  if (ret < 0) {
    err("%s, dev_filelock_unlock fail\n", __func__);
    return ret;
  }
  return 0;

err_unlock:
  dev_filelock_unlock(impl);
  return ret;
}

bool mt_dev_lcore_valid(struct mtl_main_impl* impl, unsigned int lcore) {
  struct mt_lcore_shm* lcore_shm = impl->lcore_shm;

  if (lcore >= RTE_MAX_LCORE) {
    err("%s, invalid lcore %d\n", __func__, lcore);
    return -EIO;
  }
  if (!lcore_shm) {
    err("%s, no lcore shm attached\n", __func__);
    return -EIO;
  }

  return lcore_shm->lcores_active[lcore];
}

static int dev_uinit_lcores(struct mtl_main_impl* impl) {
  int ret;
  int shm_id = impl->lcore_shm_id;
  struct mt_lcore_shm* lcore_shm = impl->lcore_shm;
  if (!lcore_shm || shm_id == -1) {
    err("%s, no lcore shm attached\n", __func__);
    return -EIO;
  }

  for (unsigned int lcore = 0; lcore < RTE_MAX_LCORE; lcore++) {
    if (impl->local_lcores_active[lcore]) {
      warn("%s, lcore %d still active\n", __func__, lcore);
      mt_dev_put_lcore(impl, lcore);
    }
  }

  ret = dev_filelock_lock(impl);
  if (ret < 0) {
    err("%s, dev_filelock_lock fail\n", __func__);
    return ret;
  }

  ret = shmdt(impl->lcore_shm);
  if (ret == -1) {
    err("%s, shared memory detach failed, %s\n", __func__, strerror(errno));
    goto err_unlock;
  }

  struct shmid_ds shmds;
  ret = shmctl(shm_id, IPC_STAT, &shmds);
  if (ret < 0) {
    err("%s, can not stat shared memory, %s\n", __func__, strerror(errno));
    goto err_unlock;
  }
  if (shmds.shm_nattch == 0) { /* remove ipc if we are the last user */
    ret = shmctl(shm_id, IPC_RMID, NULL);
    if (ret < 0) {
      warn("%s, can not remove shared memory, %s\n", __func__, strerror(errno));
      goto err_unlock;
    }
  }

  impl->lcore_shm_id = -1;
  impl->lcore_shm = NULL;
  ret = dev_filelock_unlock(impl);
  if (ret < 0) {
    err("%s, dev_filelock_unlock fail\n", __func__);
    return ret;
  }
  return 0;

err_unlock:
  dev_filelock_unlock(impl);
  return ret;
}

static int dev_init_lcores(struct mtl_main_impl* impl) {
  int ret;
  struct mt_lcore_shm* lcore_shm;

  if (impl->lcore_shm) {
    err("%s, lcore_shm attached\n", __func__);
    return -EIO;
  }

  ret = dev_filelock_lock(impl);
  if (ret < 0) {
    err("%s, dev_filelock_lock fail\n", __func__);
    return ret;
  }

  key_t key = ftok("/dev/null", 21);
  if (key < 0) {
    err("%s, ftok error: %s\n", __func__, strerror(errno));
    ret = -EIO;
    goto err_unlock;
  }
  int shm_id = shmget(key, sizeof(*lcore_shm), 0666 | IPC_CREAT);
  if (shm_id < 0) {
    err("%s, can not get shared memory for lcore, %s\n", __func__, strerror(errno));
    ret = -EIO;
    goto err_unlock;
  }
  impl->lcore_shm_id = shm_id;

  lcore_shm = shmat(shm_id, NULL, 0);
  if (lcore_shm == (void*)-1) {
    err("%s, can not attach shared memory for lcore, %s\n", __func__, strerror(errno));
    ret = -EIO;
    goto err_unlock;
  }

  struct shmid_ds stat;
  ret = shmctl(shm_id, IPC_STAT, &stat);
  if (ret < 0) {
    err("%s, shmctl fail\n", __func__);
    shmdt(lcore_shm);
    goto err_unlock;
  }
  if (stat.shm_nattch == 1) /* clear shm as we are the first user */
    memset(lcore_shm, 0, sizeof(*lcore_shm));

  impl->lcore_shm = lcore_shm;
  info("%s, shared memory attached at %p nattch %d\n", __func__, impl->lcore_shm,
       (int)stat.shm_nattch);
  ret = dev_filelock_unlock(impl);
  if (ret < 0) {
    err("%s, dev_filelock_unlock fail\n", __func__);
    return ret;
  }
  return 0;

err_unlock:
  dev_filelock_unlock(impl);
  return ret;
}

static int dev_if_uinit_rx_queues(struct mt_interface* inf) {
  enum mtl_port port = inf->port;
  struct mt_rx_queue* rx_queue;

  if (!inf->rx_queues) return 0;

  for (uint16_t q = 0; q < inf->max_rx_queues; q++) {
    rx_queue = &inf->rx_queues[q];
    if (rx_queue->active) {
      warn("%s(%d), rx queue %d still active\n", __func__, port, q);
    }
    if (rx_queue->flow_rsp) {
      warn("%s(%d), flow %d still active\n", __func__, port, q);
      dev_if_free_rx_flow(inf, rx_queue->flow_rsp);
      rx_queue->flow_rsp = NULL;
    }
    if (rx_queue->mbuf_pool) {
      mt_mempool_free(rx_queue->mbuf_pool);
      rx_queue->mbuf_pool = NULL;
    }
    if (rx_queue->mbuf_payload_pool) {
      mt_mempool_free(rx_queue->mbuf_payload_pool);
      rx_queue->mbuf_payload_pool = NULL;
    }
  }

  mt_rte_free(inf->rx_queues);
  inf->rx_queues = NULL;

  return 0;
}

static int dev_if_init_rx_queues(struct mtl_main_impl* impl, struct mt_interface* inf) {
  struct mt_rx_queue* rx_queues =
      mt_rte_zmalloc_socket(sizeof(*rx_queues) * inf->max_rx_queues, inf->socket_id);
  if (!rx_queues) {
    err("%s(%d), rx_queues not alloc\n", __func__, inf->port);
    return -ENOMEM;
  }

  if (!mt_has_rx_mono_pool(impl)) {
    for (uint16_t q = 0; q < inf->max_rx_queues; q++) {
      rx_queues[q].queue_id = q;
      rx_queues[q].port = inf->port;
      rx_queues[q].port_id = inf->port_id;

      /* Create mempool to hold the rx queue mbufs. */
      unsigned int mbuf_elements = inf->nb_rx_desc + 1024;
      char pool_name[ST_MAX_NAME_LEN];
      snprintf(pool_name, ST_MAX_NAME_LEN, "%sP%dQ%d_MBUF", MT_RX_MEMPOOL_PREFIX,
               inf->port, q);
      struct rte_mempool* mbuf_pool = NULL;

      if (mt_pmd_is_kernel(impl, inf->port)) {
        mbuf_pool = mt_mempool_create_by_ops(
            impl, inf->port, pool_name, mbuf_elements, MT_MBUF_CACHE_SIZE,
            sizeof(struct mt_muf_priv_data), 2048 - MT_MBUF_CACHE_SIZE, NULL);
      } else {
        if (q < inf->system_rx_queues_end)
          mbuf_pool = mt_mempool_create_common(impl, inf->port, pool_name, mbuf_elements);
        else {
          uint16_t data_room_sz = ST_PKT_MAX_ETHER_BYTES;
          if (inf->drv_type == MT_DRV_IGC) /* to avoid igc nic split mbuf */
            data_room_sz = MT_MBUF_DEFAULT_DATA_SIZE;
          if (impl->rx_pool_data_size) /* user suggested data room size */
            data_room_sz = impl->rx_pool_data_size;
          mbuf_pool = mt_mempool_create(impl, inf->port, pool_name, mbuf_elements,
                                        MT_MBUF_CACHE_SIZE,
                                        sizeof(struct mt_muf_priv_data), data_room_sz);
        }
      }
      if (!mbuf_pool) {
        dev_if_uinit_rx_queues(inf);
        return -ENOMEM;
      }
      rx_queues[q].mbuf_pool = mbuf_pool;
      rx_queues[q].mbuf_elements = mbuf_elements;

      /* hdr split payload mbuf */
      if ((q >= inf->system_rx_queues_end) && (q < inf->hdr_split_rx_queues_end)) {
        if (!mt_if_has_hdr_split(impl, inf->port)) {
          err("%s(%d), no hdr split feature\n", __func__, inf->port);
          dev_if_uinit_rx_queues(inf);
          return -EIO;
        }
        snprintf(pool_name, ST_MAX_NAME_LEN, "%sP%dQ%d_PAYLOAD", MT_RX_MEMPOOL_PREFIX,
                 inf->port, q);
        mbuf_pool = mt_mempool_create(impl, inf->port, pool_name, mbuf_elements,
                                      MT_MBUF_CACHE_SIZE, sizeof(struct mt_muf_priv_data),
                                      ST_PKT_MAX_ETHER_BYTES);
        if (!mbuf_pool) {
          dev_if_uinit_rx_queues(inf);
          return -ENOMEM;
        }
        rx_queues[q].mbuf_payload_pool = mbuf_pool;
      }
    }
  }
  inf->rx_queues = rx_queues;

  return 0;
}

static int dev_if_uinit_tx_queues(struct mt_interface* inf) {
  enum mtl_port port = inf->port;
  struct mt_tx_queue* tx_queue;

  if (!inf->tx_queues) return 0;

  for (uint16_t q = 0; q < inf->max_tx_queues; q++) {
    tx_queue = &inf->tx_queues[q];
    if (tx_queue->active) {
      warn("%s(%d), tx_queue %d still active\n", __func__, port, q);
    }
  }

  mt_rte_free(inf->tx_queues);
  inf->tx_queues = NULL;

  return 0;
}

static int dev_if_init_tx_queues(struct mtl_main_impl* impl, struct mt_interface* inf) {
  struct mt_tx_queue* tx_queues =
      mt_rte_zmalloc_socket(sizeof(*tx_queues) * inf->max_tx_queues, inf->socket_id);
  if (!tx_queues) {
    err("%s(%d), tx_queues not alloc\n", __func__, inf->port);
    return -ENOMEM;
  }

  for (uint16_t q = 0; q < inf->max_tx_queues; q++) {
    tx_queues[q].port = inf->port;
    tx_queues[q].port_id = inf->port_id;
    tx_queues[q].queue_id = q;
    tx_queues[q].rl_shapers_mapping = -1;
  }
  inf->tx_queues = tx_queues;

  return 0;
}

/* detect pacing */
static int dev_if_init_pacing(struct mt_interface* inf) {
  enum mtl_port port = inf->port;
  int ret;

  if (mt_shared_tx_queue(inf->parent, inf->port)) {
    info("%s(%d), use tsc as shared tx queue\n", __func__, port);
    inf->tx_pacing_way = ST21_TX_PACING_WAY_TSC;
    return 0;
  }

  if ((ST21_TX_PACING_WAY_AUTO == inf->tx_pacing_way) ||
      (ST21_TX_PACING_WAY_RL == inf->tx_pacing_way)) {
    /* VF require all q config with RL */
    if (inf->port_type == MT_PORT_VF) {
      ret = dev_init_ratelimit_vf(inf);
    } else {
      ret = dev_tx_queue_set_rl_rate(inf, 0, ST_DEFAULT_RL_BPS);
      if (ret >= 0) dev_tx_queue_set_rl_rate(inf, 0, 0);
    }
    if (ret < 0) { /* fallback to tsc if no rl */
      if (ST21_TX_PACING_WAY_AUTO == inf->tx_pacing_way) {
        warn("%s(%d), fallback to tsc as rl init fail\n", __func__, port);
        inf->tx_pacing_way = ST21_TX_PACING_WAY_TSC;
      } else {
        err("%s(%d), rl init fail\n", __func__, port);
        return ret;
      }
    } else {
      inf->tx_pacing_way = ST21_TX_PACING_WAY_RL;
    }
  }

  return 0;
}

static struct mt_rx_flow_rsp* dev_if_create_rx_flow(struct mt_interface* inf, uint16_t q,
                                                    struct mt_rxq_flow* flow) {
  int ret;
  enum mtl_port port = inf->port;
  struct mtl_main_impl* impl = inf->parent;
  uint8_t* ip = flow->dip_addr;

  if (q >= inf->max_rx_queues) {
    err("%s(%d), invalid q %u\n", __func__, port, q);
    return NULL;
  }

  struct mt_rx_flow_rsp* rsp = mt_rte_zmalloc_socket(sizeof(*rsp), inf->socket_id);
  rsp->flow_id = -1;
  rsp->queue_id = q;

  if (mt_pmd_is_kernel(impl, port)) {
    ret = mt_socket_add_flow(impl, port, q, flow);
    if (ret < 0) {
      err("%s(%d), socket add flow fail for queue %d\n", __func__, port, q);
      mt_rte_free(rsp);
      return NULL;
    }
    rsp->flow_id = ret;
  } else {
    struct rte_flow* r_flow;

    r_flow = dev_rx_queue_create_flow(inf, q, flow);
    if (!r_flow) {
      err("%s(%d), create flow fail for queue %d, ip %u.%u.%u.%u port %u\n", __func__,
          port, q, ip[0], ip[1], ip[2], ip[3], flow->dst_port);
      mt_rte_free(rsp);
      return NULL;
    }

    rsp->flow = r_flow;
    /* WA to avoid iavf_flow_create fail in 1000+ mudp close at same time */
    if (inf->port_type == MT_PORT_VF) mt_sleep_ms(5);
  }

  return rsp;
}

static int dev_if_free_rx_flow(struct mt_interface* inf, struct mt_rx_flow_rsp* rsp) {
  enum mtl_port port = inf->port;
  struct rte_flow_error error;
  int ret;
  int max_retry = 5;
  int retry = 0;

retry:
  if (rsp->flow_id > 0) {
    mt_socket_remove_flow(inf->parent, port, rsp->flow_id);
    rsp->flow_id = -1;
  }
  if (rsp->flow) {
    mt_pthread_mutex_lock(&inf->vf_cmd_mutex);
    ret = rte_flow_destroy(inf->port_id, rsp->flow, &error);
    mt_pthread_mutex_unlock(&inf->vf_cmd_mutex);
    if (ret < 0) {
      err("%s(%d), flow destroy fail, queue %d, retry %d\n", __func__, port,
          rsp->queue_id, retry);
      retry++;
      if (retry < max_retry) {
        mt_sleep_ms(10); /* WA: to wait pf finish the vf request */
        goto retry;
      }
    }
    rsp->flow = NULL;
  }
  mt_rte_free(rsp);
  /* WA to avoid iavf_flow_destroy fail in 1000+ mudp close at same time */
  if (inf->port_type == MT_PORT_VF) mt_sleep_ms(1);
  return 0;
}

static uint64_t ptp_from_real_time(struct mtl_main_impl* impl, enum mtl_port port) {
  return mt_get_real_time();
}

static uint64_t ptp_from_user(struct mtl_main_impl* impl, enum mtl_port port) {
  struct mtl_init_params* p = mt_get_user_params(impl);

  return p->ptp_get_time_fn(p->priv);
}

static uint64_t ptp_from_tsc(struct mtl_main_impl* impl, enum mtl_port port) {
  struct mt_interface* inf = mt_if(impl, port);
  uint64_t tsc = mt_get_tsc(impl);
  return inf->real_time_base + tsc - inf->tsc_time_base;
}

uint16_t mt_dev_tx_sys_queue_burst(struct mtl_main_impl* impl, enum mtl_port port,
                                   struct rte_mbuf** tx_pkts, uint16_t nb_pkts) {
  struct mt_interface* inf = mt_if(impl, port);

  if (!inf->txq_sys_entry) {
    err("%s(%d), txq sys queue not active\n", __func__, port);
    return 0;
  }

  uint16_t tx;
  rte_spinlock_lock(&inf->txq_sys_entry_lock);
  tx = mt_txq_burst(inf->txq_sys_entry, tx_pkts, nb_pkts);
  rte_spinlock_unlock(&inf->txq_sys_entry_lock);
  return tx;
}

int mt_dev_set_tx_bps(struct mtl_main_impl* impl, enum mtl_port port, uint16_t q,
                      uint64_t bytes_per_sec) {
  struct mt_interface* inf = mt_if(impl, port);

  if (q >= inf->max_tx_queues) {
    err("%s(%d), invalid queue %d\n", __func__, port, q);
    return -EIO;
  }

  if (inf->tx_pacing_way == ST21_TX_PACING_WAY_RL) {
    dev_tx_queue_set_rl_rate(inf, q, bytes_per_sec);
  }

  return 0;
}

struct mt_tx_queue* mt_dev_get_tx_queue(struct mtl_main_impl* impl, enum mtl_port port,
                                        struct mt_txq_flow* flow) {
  struct mt_interface* inf = mt_if(impl, port);
  uint64_t bytes_per_sec = flow->bytes_per_sec;
  struct mt_tx_queue* tx_queue;
  int ret;

  if (mt_shared_tx_queue(impl, port)) {
    err("%s(%d), conflict with shared tx queue mode, use tsq api instead\n", __func__,
        port);
    return NULL;
  }

  mt_pthread_mutex_lock(&inf->tx_queues_mutex);
  for (uint16_t q = 0; q < inf->max_tx_queues; q++) {
    tx_queue = &inf->tx_queues[q];
    if (!tx_queue->active) {
      if (inf->tx_pacing_way == ST21_TX_PACING_WAY_RL) {
        ret = dev_tx_queue_set_rl_rate(inf, q, bytes_per_sec);
        if (ret < 0) {
          err("%s(%d), fallback to tsc as rl fail\n", __func__, port);
          inf->tx_pacing_way = ST21_TX_PACING_WAY_TSC;
        }
      }
      tx_queue->active = true;
      mt_pthread_mutex_unlock(&inf->tx_queues_mutex);
      if (inf->tx_pacing_way == ST21_TX_PACING_WAY_RL) {
        float bps_g = (float)tx_queue->bps * 8 / (1000 * 1000 * 1000);
        info("%s(%d), q %d with speed %fg bps\n", __func__, port, q, bps_g);
      } else {
        info("%s(%d), q %d without rl\n", __func__, port, q);
      }
      return tx_queue;
    }
  }
  mt_pthread_mutex_unlock(&inf->tx_queues_mutex);

  err("%s(%d), fail to find free tx queue\n", __func__, port);
  return NULL;
}

struct mt_rx_queue* mt_dev_get_rx_queue(struct mtl_main_impl* impl, enum mtl_port port,
                                        struct mt_rxq_flow* flow) {
  struct mt_interface* inf = mt_if(impl, port);
  int ret;
  struct mt_rx_queue* rx_queue;

  if (mt_has_srss(impl, port)) {
    err("%s(%d), conflict with srss mode, use srss api instead\n", __func__, port);
    return NULL;
  }

  if (mt_shared_rx_queue(impl, port)) {
    err("%s(%d), conflict with shared rx queue mode, use rsq api instead\n", __func__,
        port);
    return NULL;
  }

  mt_pthread_mutex_lock(&inf->rx_queues_mutex);
  for (uint16_t q = 0; q < inf->max_rx_queues; q++) {
    rx_queue = &inf->rx_queues[q];
    if (rx_queue->active) continue;
    if (flow && flow->hdr_split) { /* continue if not hdr split queue */
      if (!mt_if_hdr_split_pool(inf, q)) continue;
#ifdef ST_HAS_DPDK_HDR_SPLIT
      if (flow->hdr_split_mbuf_cb) {
        ret = rte_eth_hdrs_set_mbuf_callback(
            inf->port_id, q, flow->hdr_split_mbuf_cb_priv, flow->hdr_split_mbuf_cb);
        if (ret < 0) {
          err("%s(%d), hdrs callback fail %d for queue %d\n", __func__, port, ret, q);
          mt_pthread_mutex_unlock(&inf->rx_queues_mutex);
          return NULL;
        }
      }
#endif
    } else { /* continue if hdr split queue */
      if (mt_if_hdr_split_pool(inf, q)) continue;
    }

    /* free the dummy flow if any */
    if (rx_queue->flow_rsp) {
      dev_if_free_rx_flow(inf, rx_queue->flow_rsp);
      rx_queue->flow_rsp = NULL;
    }

    memset(&rx_queue->flow, 0, sizeof(rx_queue->flow));
    if (flow && !flow->sys_queue) {
      rx_queue->flow_rsp = dev_if_create_rx_flow(inf, q, flow);
      if (!rx_queue->flow_rsp) {
        err("%s(%d), create flow fail for queue %d\n", __func__, port, q);
        mt_pthread_mutex_unlock(&inf->rx_queues_mutex);
        return NULL;
      }
      rx_queue->flow = *flow;
    }

    if (inf->feature & MT_IF_FEATURE_RUNTIME_RX_QUEUE) {
      ret = rte_eth_dev_rx_queue_start(inf->port_id, q);
      if (ret < 0) {
        err("%s(%d), start runtime rx queue %d fail %d\n", __func__, port, q, ret);
        if (rx_queue->flow_rsp) {
          dev_if_free_rx_flow(inf, rx_queue->flow_rsp);
          rx_queue->flow_rsp = NULL;
        }
        mt_pthread_mutex_unlock(&inf->rx_queues_mutex);
        return NULL;
      }
    }
    rx_queue->active = true;
    mt_pthread_mutex_unlock(&inf->rx_queues_mutex);

    dev_flush_rx_queue(inf, rx_queue);
    if (flow) {
      uint8_t* ip = flow->dip_addr;
      info("%s(%d), q %u ip %u.%u.%u.%u port %u\n", __func__, port, q, ip[0], ip[1],
           ip[2], ip[3], flow->dst_port);
    } else {
      info("%s(%d), q %u\n", __func__, port, q);
    }
    return rx_queue;
  }
  mt_pthread_mutex_unlock(&inf->rx_queues_mutex);

  err("%s(%d), fail to find free rx queue for %s\n", __func__, port,
      flow && flow->hdr_split ? "hdr_split" : "normal");
  return NULL;
}

uint16_t mt_dev_tx_burst_busy(struct mtl_main_impl* impl, struct mt_tx_queue* queue,
                              struct rte_mbuf** tx_pkts, uint16_t nb_pkts,
                              int timeout_ms) {
  uint16_t sent = 0;
  uint64_t start_ts = mt_get_tsc(impl);

  /* Send this vector with busy looping */
  while (sent < nb_pkts) {
    if (timeout_ms > 0) {
      int ms = (mt_get_tsc(impl) - start_ts) / NS_PER_MS;
      if (ms > timeout_ms) {
        warn("%s(%u), fail as timeout to %d ms\n", __func__, mt_dev_tx_queue_id(queue),
             timeout_ms);
        return sent;
      }
    }
    sent += mt_dev_tx_burst(queue, &tx_pkts[sent], nb_pkts - sent);
  }

  return sent;
}

int mt_dev_flush_tx_queue(struct mtl_main_impl* impl, struct mt_tx_queue* queue,
                          struct rte_mbuf* pad) {
  enum mtl_port port = queue->port;
  uint16_t queue_id = queue->queue_id;

  int burst_pkts = mt_if_nb_tx_burst(impl, port);
  struct rte_mbuf* pads[1];
  pads[0] = pad;

  info("%s(%d), queue %u burst_pkts %d\n", __func__, port, queue_id, burst_pkts);
  for (int i = 0; i < burst_pkts; i++) {
    rte_mbuf_refcnt_update(pad, 1);
    mt_dev_tx_burst_busy(impl, queue, &pads[0], 1, 10);
  }
  dbg("%s, end\n", __func__);
  return 0;
}

int mt_dev_tx_done_cleanup(struct mtl_main_impl* impl, struct mt_tx_queue* queue) {
  uint16_t port_id = queue->port_id;
  uint16_t queue_id = queue->queue_id;

  return rte_eth_tx_done_cleanup(port_id, queue_id, 0);
}

int mt_dev_put_tx_queue(struct mtl_main_impl* impl, struct mt_tx_queue* queue) {
  enum mtl_port port = queue->port;
  struct mt_interface* inf = mt_if(impl, port);
  struct mt_tx_queue* tx_queue;
  uint16_t queue_id = queue->queue_id;

  if (queue_id >= inf->max_tx_queues) {
    err("%s(%d), invalid queue %d\n", __func__, port, queue_id);
    return -EIO;
  }

  tx_queue = &inf->tx_queues[queue_id];
  if (!tx_queue->active) {
    err("%s(%d), queue %d is not allocated\n", __func__, port, queue_id);
    return -EIO;
  }
  if (queue != tx_queue) {
    err("%s(%d), queue %d ctx mismatch\n", __func__, port, queue_id);
    return -EIO;
  }

  tx_queue->active = false;
  info("%s(%d), q %d\n", __func__, port, queue_id);
  return 0;
}

int mt_dev_put_rx_queue(struct mtl_main_impl* impl, struct mt_rx_queue* queue) {
  enum mtl_port port = queue->port;
  struct mt_interface* inf = mt_if(impl, port);
  uint16_t queue_id = queue->queue_id;
  int ret;
  struct mt_rx_queue* rx_queue;

  if (queue_id >= inf->max_rx_queues) {
    err("%s(%d), invalid queue %d\n", __func__, port, queue_id);
    return -EIO;
  }

  rx_queue = &inf->rx_queues[queue_id];
  if (!rx_queue->active) {
    err("%s(%d), queue %d is not allocated\n", __func__, port, queue_id);
    return -EIO;
  }

  if (rx_queue->flow_rsp) {
    dev_if_free_rx_flow(inf, rx_queue->flow_rsp);
    rx_queue->flow_rsp = NULL;
  }

  if (rx_queue->flow.hdr_split) {
#ifdef ST_HAS_DPDK_HDR_SPLIT
    /* clear hdrs mbuf callback */
    rte_eth_hdrs_set_mbuf_callback(inf->port_id, queue_id, NULL, NULL);
#endif
  }

  if (inf->feature & MT_IF_FEATURE_RUNTIME_RX_QUEUE) {
    ret = rte_eth_dev_rx_queue_stop(inf->port_id, queue_id);
    if (ret < 0)
      err("%s(%d), stop runtime rx queue %d fail %d\n", __func__, port, queue_id, ret);
  }

  rx_queue->active = false;
  info("%s(%d), q %d\n", __func__, port, queue_id);
  return 0;
}

int mt_dev_create(struct mtl_main_impl* impl) {
  int num_ports = mt_num_ports(impl);
  struct mtl_init_params* p = mt_get_user_params(impl);
  int ret;
  struct mt_interface* inf;
  enum mt_port_type port_type;

  ret = dev_init_lcores(impl);
  if (ret < 0) return ret;

  for (int i = 0; i < num_ports; i++) {
    int detect_retry = 0;

    inf = mt_if(impl, i);
    port_type = inf->port_type;

#if RTE_VERSION >= RTE_VERSION_NUM(21, 11, 0, 0)
    /* DPDK 21.11 support start time sync before rte_eth_dev_start */
    if ((mt_has_ptp_service(impl) || mt_has_ebu(impl)) && (port_type == MT_PORT_PF)) {
      ret = dev_start_timesync(inf);
      if (ret >= 0) inf->feature |= MT_IF_FEATURE_TIMESYNC;
    }
#endif

  retry:
    ret = dev_start_port(inf);
    if (ret < 0) {
      err("%s(%d), dev_start_port fail %d\n", __func__, i, ret);
      goto err_exit;
    }
    if (detect_retry > 0) {
      err("%s(%d), sleep 5s before detect link\n", __func__, i);
      /* leave time as reset */
      mt_sleep_ms(5 * 1000);
    }
    ret = dev_detect_link(inf); /* some port can only detect link after start */
    if (ret < 0) {
      err("%s(%d), dev_detect_link fail %d retry %d\n", __func__, i, ret, detect_retry);
      if (detect_retry < 3) {
        detect_retry++;
        rte_eth_dev_reset(inf->port_id);
        ret = dev_config_port(inf);
        if (ret < 0) {
          err("%s(%d), dev_config_port fail %d\n", __func__, i, ret);
          goto err_exit;
        }
        goto retry;
      } else {
        goto err_exit;
      }
    }
    /* try to start time sync after rte_eth_dev_start */
    if ((mt_has_ptp_service(impl) || mt_has_ebu(impl)) && (port_type == MT_PORT_PF) &&
        !(inf->feature & MT_IF_FEATURE_TIMESYNC)) {
      ret = dev_start_timesync(inf);
      if (ret >= 0) inf->feature |= MT_IF_FEATURE_TIMESYNC;
    }

    ret = dev_if_init_pacing(inf);
    if (ret < 0) {
      err("%s(%d), init pacing fail\n", __func__, i);
      goto err_exit;
    }

    if (inf->drv_type == MT_DRV_ENA) {
      inf->dev_stats = mt_rte_zmalloc_socket(sizeof(struct mt_dev_stats), inf->socket_id);
      if (!inf->dev_stats) {
        err("%s(%d), malloc dev_stats fail\n", __func__, i);
        ret = -ENOMEM;
        goto err_exit;
      }
    }

    mt_stat_register(impl, dev_inf_stat, inf, "dev_inf");

    info("%s(%d), feature 0x%x, tx pacing %s\n", __func__, i, inf->feature,
         st_tx_pacing_way_name(inf->tx_pacing_way));
  }

  /* init sch with one lcore scheduler */
  int data_quota_mbs_per_sch;
  if (mt_has_user_quota(impl)) {
    data_quota_mbs_per_sch = mt_get_user_params(impl)->data_quota_mbs_per_sch;
  } else {
    /* default: max ST_QUOTA_TX1080P_PER_SCH sessions 1080p@60fps for tx */
    data_quota_mbs_per_sch =
        ST_QUOTA_TX1080P_PER_SCH * st20_1080p59_yuv422_10bit_bandwidth_mps();
  }
  ret = mt_sch_mrg_init(impl, data_quota_mbs_per_sch);
  if (ret < 0) {
    err("%s, sch mgr init fail %d\n", __func__, ret);
    goto err_exit;
  }

  /* create system sch */
  impl->main_sch = mt_sch_get(impl, 0, MT_SCH_TYPE_DEFAULT, MT_SCH_MASK_ALL);
  if (ret < 0) {
    err("%s, get sch fail\n", __func__);
    goto err_exit;
  }

  /* rte_eth_stats_get fail in alarm context for VF, move it to thread */
  mt_pthread_mutex_init(&impl->stat_wake_mutex, NULL);
  mt_pthread_cond_init(&impl->stat_wake_cond, NULL);
  rte_atomic32_set(&impl->stat_stop, 0);
  pthread_create(&impl->stat_tid, NULL, dev_stat_thread, impl);
  if (!p->dump_period_s) p->dump_period_s = MT_DEV_STAT_INTERVAL_S;
  rte_eal_alarm_set(MT_DEV_STAT_INTERVAL_US(p->dump_period_s), dev_stat_alarm_handler,
                    impl);

  info("%s, succ, stat period %ds\n", __func__, p->dump_period_s);
  return 0;

err_exit:
  if (impl->main_sch) mt_sch_put(impl->main_sch, 0);
  for (int i = num_ports - 1; i >= 0; i--) {
    inf = mt_if(impl, i);

    dev_stop_port(inf);
  }
  return ret;
}

int mt_dev_free(struct mtl_main_impl* impl) {
  int num_ports = mt_num_ports(impl);
  int ret;
  struct mt_interface* inf;

  ret = rte_eal_alarm_cancel(dev_stat_alarm_handler, impl);
  if (ret < 0) err("%s, dev_stat_alarm_handler cancel fail %d\n", __func__, ret);
  if (impl->stat_tid) {
    rte_atomic32_set(&impl->stat_stop, 1);
    dev_stat_wakeup_thread(impl);
    pthread_join(impl->stat_tid, NULL);
    impl->stat_tid = 0;
  }
  mt_pthread_mutex_destroy(&impl->stat_wake_mutex);
  mt_pthread_cond_destroy(&impl->stat_wake_cond);

  mt_sch_mrg_uinit(impl);
  dev_uinit_lcores(impl);

  for (int i = 0; i < num_ports; i++) {
    inf = mt_if(impl, i);

    mt_stat_unregister(impl, dev_inf_stat, inf);
    if (inf->dev_stats) {
      mt_rte_free(inf->dev_stats);
      inf->dev_stats = NULL;
    }
    dev_stop_port(inf);
  }

  info("%s, succ\n", __func__);
  return 0;
}

int mt_dev_start(struct mtl_main_impl* impl) {
  int ret = 0;

  /* start active sch */
  ret = mt_sch_start_all(impl);
  if (ret < 0) {
    err("%s, start all sch fail %d\n", __func__, ret);
    return ret;
  }

  info("%s, succ\n", __func__);
  return 0;
}

int mt_dev_stop(struct mtl_main_impl* impl) {
  mt_sch_stop_all(impl);
  return 0;
}

int mt_dev_get_socket(const char* port) {
  uint16_t port_id = 0;
  int ret = rte_eth_dev_get_port_by_name(port, &port_id);
  if (ret < 0) {
    err("%s, failed to locate %s. Please run nicctl.sh\n", __func__, port);
    return ret;
  }
  int soc_id;
  soc_id = rte_eth_dev_socket_id(port_id);
  if (SOCKET_ID_ANY == soc_id) {
    soc_id = 0;
    info("%s, direct soc_id from SOCKET_ID_ANY to 0 for %s\n", __func__, port);
  }
  return soc_id;
}

int mt_dev_init(struct mtl_init_params* p, struct mt_kport_info* kport_info) {
  int ret;

  ret = dev_eal_init(p, kport_info);
  if (ret < 0) {
    err("%s, dev_eal_init fail %d\n", __func__, ret);
    return ret;
  }

  return 0;
}

int mt_dev_uinit(struct mtl_init_params* p) {
  rte_eal_cleanup();

  info("%s, succ\n", __func__);
  return 0;
}

int dev_arp_mac(struct mtl_main_impl* impl, uint8_t dip[MTL_IP_ADDR_LEN],
                struct rte_ether_addr* ea, enum mtl_port port, int timeout_ms) {
  int ret;

  dbg("%s(%d), start to get mac for ip %d.%d.%d.%d\n", __func__, port, dip[0], dip[1],
      dip[2], dip[3]);
  if (mt_pmd_is_kernel(impl, port)) {
    ret = mt_socket_get_mac(impl, mt_get_user_params(impl)->port[port], dip, ea,
                            timeout_ms);
    if (ret < 0) {
      dbg("%s(%d), failed to get mac from socket %d\n", __func__, port, ret);
      return ret;
    }
  } else {
    ret = mt_arp_cni_get_mac(impl, ea, port, mt_ip_to_u32(dip), timeout_ms);
    if (ret < 0) {
      dbg("%s(%d), failed to get mac from cni %d\n", __func__, port, ret);
      return ret;
    }
  }

  return 0;
}

int mt_dev_dst_ip_mac(struct mtl_main_impl* impl, uint8_t dip[MTL_IP_ADDR_LEN],
                      struct rte_ether_addr* ea, enum mtl_port port, int timeout_ms) {
  int ret;

  if (mt_is_multicast_ip(dip)) {
    mt_mcast_ip_to_mac(dip, ea);
    ret = 0;
  } else if (mt_is_lan_ip(dip, mt_sip_addr(impl, port), mt_sip_netmask(impl, port))) {
    ret = dev_arp_mac(impl, dip, ea, port, timeout_ms);
  } else {
    uint8_t* gateway = mt_sip_gateway(impl, port);
    if (mt_ip_to_u32(gateway)) {
      ret = dev_arp_mac(impl, gateway, ea, port, timeout_ms);
    } else {
      err("%s(%d), ip %d.%d.%d.%d is wan but no gateway support\n", __func__, port,
          dip[0], dip[1], dip[2], dip[3]);
      return -EIO;
    }
  }

  dbg("%s(%d), ip: %d.%d.%d.%d, mac: %02hhx:%02hhx:%02hhx:%02hhx:%02hhx:%02hhx\n",
      __func__, port, dip[0], dip[1], dip[2], dip[3], ea->addr_bytes[0],
      ea->addr_bytes[1], ea->addr_bytes[2], ea->addr_bytes[3], ea->addr_bytes[4],
      ea->addr_bytes[5]);
  return ret;
}

int mt_dev_if_uinit(struct mtl_main_impl* impl) {
  int num_ports = mt_num_ports(impl), ret;
  struct mt_interface* inf;

  for (int i = 0; i < num_ports; i++) {
    inf = mt_if(impl, i);

    if (inf->pad) {
      rte_pktmbuf_free(inf->pad);
      inf->pad = NULL;
    }

    dev_if_uinit_tx_queues(inf);
    dev_if_uinit_rx_queues(inf);

    if (inf->mcast_mac_lists) {
      warn("%s(%d), mcast_mac_lists still active\n", __func__, i);
      free(inf->mcast_mac_lists);
      inf->mcast_mac_lists = NULL;
    }

    if (inf->tx_mbuf_pool) {
      ret = mt_mempool_free(inf->tx_mbuf_pool);
      if (ret >= 0) inf->tx_mbuf_pool = NULL;
    }
    if (inf->rx_mbuf_pool) {
      ret = mt_mempool_free(inf->rx_mbuf_pool);
      if (ret >= 0) inf->rx_mbuf_pool = NULL;
    }

    mt_pthread_mutex_destroy(&inf->tx_queues_mutex);
    mt_pthread_mutex_destroy(&inf->rx_queues_mutex);
    mt_pthread_mutex_destroy(&inf->vf_cmd_mutex);

    dev_close_port(inf);
  }

  return 0;
}

int mt_dev_if_init(struct mtl_main_impl* impl) {
  int num_ports = mt_num_ports(impl);
  struct mtl_init_params* p = mt_get_user_params(impl);
  uint16_t port_id;
  char* port;
  struct rte_eth_dev_info* dev_info;
  struct mt_interface* inf;
  int ret;

  for (int i = 0; i < num_ports; i++) {
    inf = mt_if(impl, i);
    dev_info = &inf->dev_info;

    if (mt_pmd_is_kernel(impl, i))
      port = impl->kport_info.port[i];
    else
      port = p->port[i];
    ret = rte_eth_dev_get_port_by_name(port, &port_id);
    if (ret < 0) {
      err("%s, failed to locate %s. Please run nicctl.sh\n", __func__, port);
      mt_dev_if_uinit(impl);
      return ret;
    }
    ret = rte_eth_dev_info_get(port_id, dev_info);
    if (ret < 0) {
      err("%s, rte_eth_dev_info_get fail for %s\n", __func__, port);
      mt_dev_if_uinit(impl);
      return ret;
    }
    ret = parse_driver_info(dev_info->driver_name, &inf->port_type, &inf->drv_type,
                            &inf->flow_type);
    if (ret < 0) {
      err("%s, parse_driver_info fail(%d) for %s\n", __func__, ret, port);
      mt_dev_if_uinit(impl);
      return ret;
    }
    inf->port = i;
    inf->port_id = port_id;
    inf->tx_pacing_way = p->pacing;
    dbg("%s(%d), reta_size %u\n", __func__, i, dev_info->reta_size);
    mt_pthread_mutex_init(&inf->tx_queues_mutex, NULL);
    mt_pthread_mutex_init(&inf->rx_queues_mutex, NULL);
    mt_pthread_mutex_init(&inf->vf_cmd_mutex, NULL);
    rte_spinlock_init(&inf->txq_sys_entry_lock);

    if (mt_ptp_tsc_source(impl)) {
      info("%s(%d), use tsc ptp source\n", __func__, i);
      inf->ptp_get_time_fn = ptp_from_tsc;
    } else if (mt_has_user_ptp(impl)) {
      /* user provide the ptp source */
      info("%s(%d), use user ptp source\n", __func__, i);
      inf->ptp_get_time_fn = ptp_from_user;
    } else {
      info("%s(%d), use mt ptp source\n", __func__, i);
      inf->ptp_get_time_fn = ptp_from_real_time;
    }

    inf->rss_mode = p->rss_mode;
    /* enable rss if no flow support */
    if (inf->flow_type == MT_FLOW_NONE && inf->rss_mode == MTL_RSS_MODE_NONE) {
      inf->rss_mode = MTL_RSS_MODE_L3_L4; /* default l3_l4 */
    }

    /* set max tx/rx queues */
    if (p->pmd[i] == MTL_PMD_DPDK_AF_XDP) {
      /* af_xdp queues(ring pairs) count indicate by user */
      inf->max_tx_queues = p->xdp_info[i].queue_count;
      inf->max_rx_queues = inf->max_tx_queues;
      inf->system_rx_queues_end = 0;
    } else {
      info("%s(%d), user request queues tx %u rx %u, deprecated sessions tx %u rx %u\n",
           __func__, i, p->tx_queues_cnt[i], p->rx_queues_cnt[i], p->tx_sessions_cnt_max,
           p->rx_sessions_cnt_max);
      inf->max_tx_queues =
          p->tx_sessions_cnt_max ? p->tx_sessions_cnt_max : p->tx_queues_cnt[i];
      inf->max_tx_queues++; /* arp, mcast, ptp use shared sys queue */
#ifdef MTL_HAS_KNI
      inf->max_tx_queues++; /* kni tx queue */
#endif
#ifdef MTL_HAS_TAP
      inf->max_tx_queues++; /* tap tx queue */
#endif

      inf->max_rx_queues =
          p->rx_sessions_cnt_max ? p->rx_sessions_cnt_max : p->rx_queues_cnt[i];
      if (!mt_no_system_rxq(impl)) {
        inf->max_rx_queues++;
        inf->system_rx_queues_end = 1; /* cni rx */
        if (mt_has_ptp_service(impl)) {
          inf->max_rx_queues++;
          inf->system_rx_queues_end++;
        }
#ifdef MTL_HAS_TAP
        inf->max_rx_queues++;
        inf->system_rx_queues_end++;
#endif
      }
      inf->hdr_split_rx_queues_end =
          inf->system_rx_queues_end + p->nb_rx_hdr_split_queues;
    }
    /* max tx/rx queues don't exceed dev limit */
    inf->max_tx_queues = RTE_MIN(inf->max_tx_queues, dev_info->max_tx_queues);
    inf->max_rx_queues = RTE_MIN(inf->max_rx_queues, dev_info->max_rx_queues);

    /* when using VF, num_queue_pairs will be set as the max of tx/rx */
    if (inf->port_type == MT_PORT_VF) {
      inf->max_tx_queues = RTE_MAX(inf->max_rx_queues, inf->max_tx_queues);
      inf->max_rx_queues = inf->max_tx_queues;
    }

    if (dev_info->dev_capa & RTE_ETH_DEV_CAPA_RUNTIME_RX_QUEUE_SETUP)
      inf->feature |= MT_IF_FEATURE_RUNTIME_RX_QUEUE;

#if RTE_VERSION >= RTE_VERSION_NUM(22, 3, 0, 0)
    if (dev_info->tx_offload_capa & RTE_ETH_TX_OFFLOAD_MULTI_SEGS)
      inf->feature |= MT_IF_FEATURE_TX_MULTI_SEGS;
#else
    if (dev_info->tx_offload_capa & DEV_TX_OFFLOAD_MULTI_SEGS)
      inf->feature |= MT_IF_FEATURE_TX_MULTI_SEGS;
#endif

#if RTE_VERSION >= RTE_VERSION_NUM(22, 3, 0, 0)
    if (dev_info->tx_offload_capa & RTE_ETH_TX_OFFLOAD_IPV4_CKSUM)
      inf->feature |= MT_IF_FEATURE_TX_OFFLOAD_IPV4_CKSUM;
#else
    if (dev_info->tx_offload_capa & DEV_TX_OFFLOAD_IPV4_CKSUM)
      inf->feature |= MT_IF_FEATURE_TX_OFFLOAD_IPV4_CKSUM;
#endif

#if RTE_VERSION >= RTE_VERSION_NUM(23, 3, 0, 0)
    /* Detect LaunchTime capability */
    if (dev_info->tx_offload_capa & RTE_ETH_TX_OFFLOAD_SEND_ON_TIMESTAMP &&
        ST21_TX_PACING_WAY_TSN == inf->tx_pacing_way) {
      inf->feature |= MT_IF_FEATURE_TX_OFFLOAD_SEND_ON_TIMESTAMP;

      int* dev_tx_timestamp_dynfield_offset_ptr =
          dev_info->default_txconf.reserved_ptrs[1];
      uint64_t* dev_tx_timestamp_dynflag_ptr = dev_info->default_txconf.reserved_ptrs[0];
      ret = rte_mbuf_dyn_tx_timestamp_register(dev_tx_timestamp_dynfield_offset_ptr,
                                               dev_tx_timestamp_dynflag_ptr);
      if (ret < 0) {
        err("%s, rte_mbuf_dyn_tx_timestamp_register fail\n", __func__);
        return ret;
      }

      ret = rte_mbuf_dynflag_lookup(RTE_MBUF_DYNFLAG_TX_TIMESTAMP_NAME, NULL);
      if (ret < 0) return ret;
      inf->tx_launch_time_flag = 1ULL << ret;

      ret = rte_mbuf_dynfield_lookup(RTE_MBUF_DYNFIELD_TIMESTAMP_NAME, NULL);
      if (ret < 0) return ret;
      inf->tx_dynfield_offset = ret;
    }
#endif

    if (mt_has_ebu(impl) &&
#if RTE_VERSION >= RTE_VERSION_NUM(22, 3, 0, 0)
        (dev_info->rx_offload_capa & RTE_ETH_RX_OFFLOAD_TIMESTAMP)
#else
        (dev_info->rx_offload_capa & DEV_RX_OFFLOAD_TIMESTAMP)
#endif
    ) {
      if (!impl->dynfield_offset) {
        ret = rte_mbuf_dyn_rx_timestamp_register(&impl->dynfield_offset, NULL);
        if (ret < 0) {
          err("%s, rte_mbuf_dyn_rx_timestamp_register fail\n", __func__);
          return ret;
        }
        info("%s, rte_mbuf_dyn_rx_timestamp_register: mbuf dynfield offset: %d\n",
             __func__, impl->dynfield_offset);
      }
      inf->feature |= MT_IF_FEATURE_RX_OFFLOAD_TIMESTAMP;
    }

#ifdef RTE_ETH_RX_OFFLOAD_BUFFER_SPLIT
    if (dev_info->rx_queue_offload_capa & RTE_ETH_RX_OFFLOAD_BUFFER_SPLIT) {
      inf->feature |= MT_IF_FEATURE_RXQ_OFFLOAD_BUFFER_SPLIT;
      dbg("%s(%d), has rxq hdr split\n", __func__, i);
    }
#endif

    ret = dev_config_port(inf);
    if (ret < 0) {
      err("%s(%d), dev_config_port fail %d\n", __func__, i, ret);
      mt_dev_if_uinit(impl);
      return -EIO;
    }

    unsigned int mbuf_elements = 1024;
    char pool_name[ST_MAX_NAME_LEN];
    struct rte_mempool* mbuf_pool;
    /* Create mempool in memory to hold the system rx mbufs if mono */
    if (mt_has_rx_mono_pool(impl)) {
      mbuf_elements = 1024;
      /* append as rx queues */
      mbuf_elements += inf->max_rx_queues * inf->nb_rx_desc;
      snprintf(pool_name, ST_MAX_NAME_LEN, "%sP%d_SYS", MT_RX_MEMPOOL_PREFIX, i);
      mbuf_pool = mt_mempool_create_common(impl, i, pool_name, mbuf_elements);
      if (!mbuf_pool) {
        mt_dev_if_uinit(impl);
        return -ENOMEM;
      }
      inf->rx_mbuf_pool = mbuf_pool;
    }

    /* Create default mempool in memory to hold the system tx mbufs */
    mbuf_elements = 1024;
    if (mt_has_tx_mono_pool(impl)) {
      /* append as tx queues, double as tx ring */
      mbuf_elements += inf->max_tx_queues * inf->nb_tx_desc * 2;
    }
    snprintf(pool_name, ST_MAX_NAME_LEN, "%sP%d_SYS", MT_TX_MEMPOOL_PREFIX, i);
    mbuf_pool = mt_mempool_create_common(impl, i, pool_name, mbuf_elements);
    if (!mbuf_pool) {
      mt_dev_if_uinit(impl);
      return -ENOMEM;
    }
    inf->tx_mbuf_pool = mbuf_pool;

    ret = dev_if_init_tx_queues(impl, inf);
    if (ret < 0) {
      mt_dev_if_uinit(impl);
      return -ENOMEM;
    }
    ret = dev_if_init_rx_queues(impl, inf);
    if (ret < 0) {
      mt_dev_if_uinit(impl);
      return -ENOMEM;
    }

    inf->pad = mt_build_pad(impl, mt_get_tx_mempool(impl, i), port_id,
                            RTE_ETHER_TYPE_IPV4, 1024);
    if (!inf->pad) {
      err("%s(%d), pad alloc fail\n", __func__, i);
      mt_dev_if_uinit(impl);
      return -ENOMEM;
    }

    info("%s(%d), port_id %d port_type %d drv_type %d\n", __func__, i, port_id,
         inf->port_type, inf->drv_type);
    info("%s(%d), dev_capa 0x%" PRIx64 ", offload 0x%" PRIx64 ":0x%" PRIx64
         " queue offload 0x%" PRIx64 ":0x%" PRIx64 ", rss : 0x%" PRIx64 "\n",
         __func__, i, dev_info->dev_capa, dev_info->tx_offload_capa,
         dev_info->rx_offload_capa, dev_info->tx_queue_offload_capa,
         dev_info->rx_queue_offload_capa, dev_info->flow_type_rss_offloads);
    info("%s(%d), system_rx_queues_end %d hdr_split_rx_queues_end %d\n", __func__, i,
         inf->system_rx_queues_end, inf->hdr_split_rx_queues_end);
    uint8_t* ip = p->sip_addr[i];
    info("%s(%d), sip: %u.%u.%u.%u\n", __func__, i, ip[0], ip[1], ip[2], ip[3]);
    uint8_t* nm = p->netmask[i];
    info("%s(%d), netmask: %u.%u.%u.%u\n", __func__, i, nm[0], nm[1], nm[2], nm[3]);
    uint8_t* gw = p->gateway[i];
    info("%s(%d), gateway: %u.%u.%u.%u\n", __func__, i, gw[0], gw[1], gw[2], gw[3]);
    struct rte_ether_addr mac;
    rte_eth_macaddr_get(port_id, &mac);
    info("%s(%d), mac: %02x:%02x:%02x:%02x:%02x:%02x\n", __func__, i, mac.addr_bytes[0],
         mac.addr_bytes[1], mac.addr_bytes[2], mac.addr_bytes[3], mac.addr_bytes[4],
         mac.addr_bytes[5]);
  }

  return 0;
}

int mt_dev_if_pre_uinit(struct mtl_main_impl* impl) {
  int num_ports = mt_num_ports(impl);
  struct mt_interface* inf;

  if (impl->main_sch) {
    mt_sch_put(impl->main_sch, 0);
    impl->main_sch = NULL;
  }

  for (int i = 0; i < num_ports; i++) {
    inf = mt_if(impl, i);

    if (inf->txq_sys_entry) {
      mt_txq_put(inf->txq_sys_entry);
      inf->txq_sys_entry = NULL;
    }
  }

  return 0;
}

int mt_dev_if_post_init(struct mtl_main_impl* impl) {
  int num_ports = mt_num_ports(impl);
  struct mt_interface* inf;

  for (int i = 0; i < num_ports; i++) {
    /* no sys queue for kernel based pmd */
    if (mt_pmd_is_kernel(impl, i)) continue;

    inf = mt_if(impl, i);

    struct mt_txq_flow flow;
    memset(&flow, 0, sizeof(flow));
    flow.sys_queue = true;
    inf->txq_sys_entry = mt_txq_get(impl, i, &flow);
    if (!inf->txq_sys_entry) {
      err("%s(%d), txq sys entry get fail\n", __func__, i);
      mt_dev_if_pre_uinit(impl);
      return -ENOMEM;
    }
  }

  return 0;
}

uint32_t mt_dev_softrss(uint32_t* input_tuple, uint32_t input_len) {
  return rte_softrss(input_tuple, input_len, mt_rss_hash_key);
}

/* map with dev_config_rss_reta */
uint16_t mt_dev_rss_hash_queue(struct mtl_main_impl* impl, enum mtl_port port,
                               uint32_t hash) {
  struct mt_interface* inf = mt_if(impl, port);
  return (hash % inf->dev_info.reta_size) % inf->max_rx_queues;
}

struct mt_rx_flow_rsp* mt_dev_create_rx_flow(struct mtl_main_impl* impl,
                                             enum mtl_port port, uint16_t q,
                                             struct mt_rxq_flow* flow) {
  struct mt_interface* inf = mt_if(impl, port);
  struct mt_rx_flow_rsp* rsp;

  if (q >= inf->max_rx_queues) {
    err("%s(%d), invalid q %u\n", __func__, port, q);
    return NULL;
  }

  mt_pthread_mutex_lock(&inf->rx_queues_mutex);
  rsp = dev_if_create_rx_flow(inf, q, flow);
  mt_pthread_mutex_unlock(&inf->rx_queues_mutex);

  return rsp;
}

int mt_dev_free_rx_flow(struct mtl_main_impl* impl, enum mtl_port port,
                        struct mt_rx_flow_rsp* rsp) {
  struct mt_interface* inf = mt_if(impl, port);
  return dev_if_free_rx_flow(inf, rsp);
}

int mt_dev_tsc_done_action(struct mtl_main_impl* impl) {
  int num_ports = mt_num_ports(impl);
  struct mt_interface* inf;

  for (int i = 0; i < num_ports; i++) {
    inf = mt_if(impl, i);

    /* tsc stable now */
    inf->real_time_base = mt_get_real_time();
    inf->tsc_time_base = mt_get_tsc(impl);
  }

  return 0;
}

uint8_t* mt_sip_addr(struct mtl_main_impl* impl, enum mtl_port port) {
  if (mt_if(impl, port)->net_proto == MTL_PROTO_DHCP) return mt_dhcp_get_ip(impl, port);
  return mt_get_user_params(impl)->sip_addr[port];
}

uint8_t* mt_sip_netmask(struct mtl_main_impl* impl, enum mtl_port port) {
  if (mt_if(impl, port)->net_proto == MTL_PROTO_DHCP)
    return mt_dhcp_get_netmask(impl, port);
  return mt_get_user_params(impl)->netmask[port];
}

uint8_t* mt_sip_gateway(struct mtl_main_impl* impl, enum mtl_port port) {
  if (mt_if(impl, port)->net_proto == MTL_PROTO_DHCP)
    return mt_dhcp_get_gateway(impl, port);
  return mt_get_user_params(impl)->gateway[port];
}
