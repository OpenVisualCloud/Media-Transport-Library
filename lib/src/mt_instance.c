/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#include "mt_instance.h"

#ifndef WINDOWSENV

/*
 * The library side of the MtlManager IPC.
 *
 * Every call goes through the client API of the manager, mtlm_api.h, whose
 * source the library compiles in. There is one implementation of the record
 * layout, the byte order and the socket path search, so the library and the
 * manager cannot disagree about any of them.
 *
 * The path comes from mtlm_client_create(NULL), which looks at
 * $MTL_MANAGER_SOCK_PATH, then the per-user runtime directory, then the system
 * directory. That is why an instance reaches a manager that runs without root.
 */

#include "mt_log.h"
#include "mt_util.h"
#include "mtlm_api.h"

static inline mtlm_client* instance_client(struct mtl_main_impl* impl) {
  return (mtlm_client*)impl->instance_client;
}

int mt_instance_put_lcore(struct mtl_main_impl* impl, uint16_t lcore_id) {
  return mtlm_lcore_put(instance_client(impl), lcore_id);
}

int mt_instance_get_lcore(struct mtl_main_impl* impl, uint16_t lcore_id) {
  return mtlm_lcore_get(instance_client(impl), lcore_id);
}

int mt_instance_request_xsks_map_fd(struct mtl_main_impl* impl, unsigned int ifindex) {
  int ret = mtlm_xsk_map_fd(instance_client(impl), ifindex);

  if (ret < 0) err("%s(%u), no xsks map fd, %s\n", __func__, ifindex, mtlm_strerror(ret));

  return ret;
}

int mt_instance_update_udp_dp_filter(struct mtl_main_impl* impl, unsigned int ifindex,
                                     uint16_t dst_port, bool add) {
  if (add) return mtlm_udp_dp_filter_add(instance_client(impl), ifindex, dst_port);

  return mtlm_udp_dp_filter_del(instance_client(impl), ifindex, dst_port);
}

int mt_instance_get_queue(struct mtl_main_impl* impl, unsigned int ifindex) {
  return mtlm_queue_get(instance_client(impl), ifindex);
}

int mt_instance_put_queue(struct mtl_main_impl* impl, unsigned int ifindex,
                          uint16_t queue_id) {
  return mtlm_queue_put(instance_client(impl), ifindex, queue_id);
}

int mt_instance_add_flow(struct mtl_main_impl* impl, unsigned int ifindex,
                         uint16_t queue_id, uint32_t flow_type, uint32_t src_ip,
                         uint32_t dst_ip, uint16_t src_port, uint16_t dst_port) {
  struct mtlm_flow flow;

  memset(&flow, 0, sizeof(flow));
  flow.ifindex = ifindex;
  flow.queue_id = queue_id;
  flow.flow_type = flow_type;
  /* The two addresses stay as they are. The caller reads them out of a
   * mt_rxq_flow, where they are already network byte order, which is the order
   * ethtool wants. A swap here and a swap back in the manager was the old way,
   * and it broke as soon as one of the two changed. */
  flow.src_ip = src_ip;
  flow.dst_ip = dst_ip;
  flow.src_port = src_port;
  flow.dst_port = dst_port;

  return mtlm_flow_add(instance_client(impl), &flow);
}

int mt_instance_del_flow(struct mtl_main_impl* impl, unsigned int ifindex,
                         uint32_t flow_id) {
  return mtlm_flow_del(instance_client(impl), ifindex, flow_id);
}

int mt_instance_init(struct mtl_main_impl* impl, struct mtl_init_params* p) {
  struct mt_user_info* u_info = &impl->u_info;
  unsigned int ifindex[MTL_MANAGER_MAX_IF];
  struct mtlm_register_args args;
  mtlm_client* client;
  uint16_t num_xdp_if = 0;
  int ret;

  impl->instance_client = NULL;

  client = mtlm_client_create(NULL);
  if (client == NULL) {
    warn("%s, no manager answers, assume single instance mode\n", __func__);
    return -EIO;
  }

  /* The manager loads the XDP program of each AF_XDP interface, so it must know
   * them before the first queue request. */
  for (int i = 0; i < p->num_ports; i++) {
    const char* if_name;

    if (!mtl_pmd_is_af_xdp(p->pmd[i])) continue;
    if (num_xdp_if >= MTL_MANAGER_MAX_IF) {
      err("%s, more than %d af_xdp ports\n", __func__, MTL_MANAGER_MAX_IF);
      mtlm_client_destroy(client);
      return -EINVAL;
    }

    if (p->pmd[i] == MTL_PMD_NATIVE_AF_XDP)
      if_name = mt_native_afxdp_port2if(p->port[i]);
    else
      if_name = mt_dpdk_afxdp_port2if(p->port[i]);
    ifindex[num_xdp_if] = if_nametoindex(if_name);
    num_xdp_if++;
  }

  memset(&args, 0, sizeof(args));
  args.pid = u_info->pid;
  args.uid = (int)getuid();
  args.hostname = u_info->hostname;
  args.ifindex = ifindex;
  args.num_if = num_xdp_if;

  ret = mtlm_register(client, &args);
  if (ret < 0) {
    err("%s, register fail, %s\n", __func__, mtlm_strerror(ret));
    mtlm_client_destroy(client);
    return ret;
  }

  impl->instance_client = client;
  info("%s, connected to the manager on %s\n", __func__, mtlm_client_sock_path(client));

  return 0;
}

int mt_instance_uinit(struct mtl_main_impl* impl) {
  mtlm_client* client = instance_client(impl);

  if (client == NULL) return -EIO;

  impl->instance_client = NULL;
  mtlm_client_destroy(client);
  return 0;
}

bool mtl_is_manager_alive(void) {
  return mtlm_manager_alive(NULL);
}

#else /* not supported on Windows */

int mt_instance_init(struct mtl_main_impl* impl, struct mtl_init_params* p) {
  impl->instance_client = NULL;
  MTL_MAY_UNUSED(p);
  return -ENOTSUP;
}

int mt_instance_uinit(struct mtl_main_impl* impl) {
  MTL_MAY_UNUSED(impl);
  return -ENOTSUP;
}

int mt_instance_get_lcore(struct mtl_main_impl* impl, uint16_t lcore_id) {
  MTL_MAY_UNUSED(impl);
  MTL_MAY_UNUSED(lcore_id);
  return -ENOTSUP;
}

int mt_instance_put_lcore(struct mtl_main_impl* impl, uint16_t lcore_id) {
  MTL_MAY_UNUSED(impl);
  MTL_MAY_UNUSED(lcore_id);
  return -ENOTSUP;
}

int mt_instance_request_xsks_map_fd(struct mtl_main_impl* impl, unsigned int ifindex) {
  MTL_MAY_UNUSED(impl);
  MTL_MAY_UNUSED(ifindex);
  return -ENOTSUP;
}

int mt_instance_update_udp_dp_filter(struct mtl_main_impl* impl, unsigned int ifindex,
                                     uint16_t dst_port, bool add) {
  MTL_MAY_UNUSED(impl);
  MTL_MAY_UNUSED(ifindex);
  MTL_MAY_UNUSED(dst_port);
  MTL_MAY_UNUSED(add);
  return -ENOTSUP;
}

int mt_instance_get_queue(struct mtl_main_impl* impl, unsigned int ifindex) {
  MTL_MAY_UNUSED(impl);
  MTL_MAY_UNUSED(ifindex);
  return -ENOTSUP;
}

int mt_instance_put_queue(struct mtl_main_impl* impl, unsigned int ifindex,
                          uint16_t queue_id) {
  MTL_MAY_UNUSED(impl);
  MTL_MAY_UNUSED(ifindex);
  MTL_MAY_UNUSED(queue_id);
  return -ENOTSUP;
}

int mt_instance_add_flow(struct mtl_main_impl* impl, unsigned int ifindex,
                         uint16_t queue_id, uint32_t flow_type, uint32_t src_ip,
                         uint32_t dst_ip, uint16_t src_port, uint16_t dst_port) {
  MTL_MAY_UNUSED(impl);
  MTL_MAY_UNUSED(ifindex);
  MTL_MAY_UNUSED(queue_id);
  MTL_MAY_UNUSED(flow_type);
  MTL_MAY_UNUSED(src_ip);
  MTL_MAY_UNUSED(dst_ip);
  MTL_MAY_UNUSED(src_port);
  MTL_MAY_UNUSED(dst_port);
  return -ENOTSUP;
}

int mt_instance_del_flow(struct mtl_main_impl* impl, unsigned int ifindex,
                         uint32_t flow_id) {
  MTL_MAY_UNUSED(impl);
  MTL_MAY_UNUSED(ifindex);
  MTL_MAY_UNUSED(flow_id);
  return -ENOTSUP;
}

bool mtl_is_manager_alive(void) {
  return false;
}

#endif
