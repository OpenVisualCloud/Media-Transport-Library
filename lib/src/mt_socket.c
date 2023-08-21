/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "mt_socket.h"

#include "mt_log.h"
#include "mt_util.h"

#ifndef WINDOWSENV
int mt_socket_get_if_ip(char* if_name, uint8_t ip[MTL_IP_ADDR_LEN],
                        uint8_t netmask[MTL_IP_ADDR_LEN]) {
  int sock, ret;
  struct ifreq ifr;

  sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock < 0) {
    err("%s, socket call fail\n", __func__);
    return sock;
  }

  memset(&ifr, 0x0, sizeof(ifr));
  snprintf(ifr.ifr_name, sizeof(ifr.ifr_name) - 1, "%s", if_name);
  ret = ioctl(sock, SIOCGIFADDR, &ifr);
  if (ret < 0) {
    err("%s, SIOCGIFADDR fail %d for if %s\n", __func__, ret, if_name);
    close(sock);
    return ret;
  }
  struct sockaddr_in* ipaddr = (struct sockaddr_in*)&ifr.ifr_addr;
  if (ip) memcpy(ip, &ipaddr->sin_addr.s_addr, MTL_IP_ADDR_LEN);

  ret = ioctl(sock, SIOCGIFNETMASK, &ifr);
  if (ret < 0) {
    err("%s, SIOCGIFADDR fail %d for if %s\n", __func__, ret, if_name);
    close(sock);
    return ret;
  }
  ipaddr = (struct sockaddr_in*)&ifr.ifr_addr;
  if (netmask) memcpy(netmask, &ipaddr->sin_addr.s_addr, MTL_IP_ADDR_LEN);

  close(sock);
  return 0;
}

int mt_socket_get_if_gateway(char* if_name, uint8_t gateway[MTL_IP_ADDR_LEN]) {
  char cmd[256];
  char out[256];

  snprintf(cmd, sizeof(cmd), "route -n | grep 'UG' | grep '%s' | awk '{print $2}'",
           if_name);
  int ret = mt_run_cmd(cmd, out, sizeof(out));
  if (ret < 0) return ret;

  uint8_t a, b, c, d;
  ret = sscanf(out, "%" SCNu8 ".%" SCNu8 ".%" SCNu8 ".%" SCNu8 "", &a, &b, &c, &d);
  if (ret < 0) {
    info("%s, cmd: %s fail\n", __func__, cmd);
    return ret;
  }

  dbg("%s, cmd %s out %s\n", __func__, cmd, out);
  gateway[0] = a;
  gateway[1] = b;
  gateway[2] = c;
  gateway[3] = d;
  return 0;
}

int mt_socket_get_if_mac(char* if_name, struct rte_ether_addr* ea) {
  int sock, ret;
  struct ifreq ifr;

  sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock < 0) {
    err("%s, socket call fail\n", __func__);
    return sock;
  }

  memset(&ifr, 0x0, sizeof(ifr));
  snprintf(ifr.ifr_name, sizeof(ifr.ifr_name) - 1, "%s", if_name);
  ret = ioctl(sock, SIOCGIFHWADDR, &ifr);
  if (ret < 0) {
    err("%s, SIOCGIFADDR fail %d for if %s\n", __func__, ret, if_name);
    close(sock);
    return ret;
  }
  memcpy(ea->addr_bytes, ifr.ifr_hwaddr.sa_data, RTE_ETHER_ADDR_LEN);

  close(sock);
  return 0;
}

int mt_socket_join_mcast(struct mtl_main_impl* impl, enum mtl_port port, uint32_t group) {
  int ret;
  char cmd[128];
  uint8_t ip[MTL_IP_ADDR_LEN];

  if (!mt_pmd_is_kernel(impl, port)) {
    err("%s(%d), not kernel based pmd\n", __func__, port);
    return -EIO;
  }

  mt_u32_to_ip(group, ip);
  snprintf(cmd, sizeof(cmd), "ip addr add %u.%u.%u.%u/24 dev %s autojoin", ip[0], ip[1],
           ip[2], ip[3], mt_get_user_params(impl)->port[port]);
  ret = mt_run_cmd(cmd, NULL, 0);
  if (ret < 0) return ret;

  info("%s, succ, %s\n", __func__, cmd);
  return 0;
}

int mt_socket_drop_mcast(struct mtl_main_impl* impl, enum mtl_port port, uint32_t group) {
  int ret;
  char cmd[128];
  uint8_t ip[MTL_IP_ADDR_LEN];

  if (!mt_pmd_is_kernel(impl, port)) {
    err("%s(%d), not kernel based pmd\n", __func__, port);
    return -EIO;
  }

  mt_u32_to_ip(group, ip);
  snprintf(cmd, sizeof(cmd), "ip addr del %u.%u.%u.%u/24 dev %s autojoin", ip[0], ip[1],
           ip[2], ip[3], mt_get_user_params(impl)->port[port]);
  ret = mt_run_cmd(cmd, NULL, 0);
  if (ret < 0) return ret;

  info("%s, succ, %s\n", __func__, cmd);
  return 0;
}

static int socket_arp_get(int sfd, in_addr_t ip, struct rte_ether_addr* ea,
                          const char* if_name) {
  struct arpreq arp;
  struct sockaddr_in* sin;
  struct in_addr ina;
  unsigned char* hw_addr;

  memset(&arp, 0, sizeof(arp));

  sin = (struct sockaddr_in*)&arp.arp_pa;
  memset(sin, 0, sizeof(struct sockaddr_in));
  sin->sin_family = AF_INET;
  ina.s_addr = ip;
  memcpy(&sin->sin_addr, (char*)&ina, sizeof(struct in_addr));

  snprintf(arp.arp_dev, sizeof(arp.arp_dev) - 1, "%s", if_name);
  int ret = ioctl(sfd, SIOCGARP, &arp);
  if (ret < 0) {
    dbg("%s, entry not available in cache...\n", __func__);
    return -EIO;
  }

  if (!(arp.arp_flags & ATF_COM)) {
    dbg("%s, arp_flags 0x%x\n", __func__, arp.arp_flags);
    return -EIO;
  }

  dbg("%s, entry has been successfully retrieved\n", __func__);
  hw_addr = (unsigned char*)arp.arp_ha.sa_data;
  memcpy(ea->addr_bytes, hw_addr, RTE_ETHER_ADDR_LEN);
  dbg("%s, mac addr found : %02x:%02x:%02x:%02x:%02x:%02x\n", __func__, hw_addr[0],
      hw_addr[1], hw_addr[2], hw_addr[3], hw_addr[4], hw_addr[5]);

  return 0;
}

static int socket_query_local_mac(uint8_t ip[MTL_IP_ADDR_LEN],
                                  struct rte_ether_addr* ea) {
  int sock;
  struct ifconf conf;
  struct ifreq* ifr = NULL;

  sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock < 0) {
    err("%s, socket call fail %d\n", __func__, sock);
    return sock;
  }

  memset(&conf, 0, sizeof(conf));
  conf.ifc_ifcu.ifcu_req = NULL;
  conf.ifc_len = 0;

  int ret = ioctl(sock, SIOCGIFCONF, &conf);
  if (ret < 0) {
    err("%s, SIOCGIFCONF fail %d\n", __func__, ret);
    close(sock);
    return ret;
  }

  ifr = malloc(conf.ifc_len);
  if (!ifr) {
    err("%s, malloc fail\n", __func__);
    close(sock);
    return -ENOMEM;
  }

  conf.ifc_ifcu.ifcu_req = ifr;
  ret = ioctl(sock, SIOCGIFCONF, &conf);
  if (ret < 0) {
    err("%s, SIOCGIFCONF fail %d\n", __func__, ret);
    close(sock);
    free(ifr);
    return ret;
  }

  int numif = conf.ifc_len / sizeof(*ifr);
  for (int i = 0; i < numif; i++) {
    struct ifreq* r = &ifr[i];
    struct sockaddr_in* sin = (struct sockaddr_in*)&r->ifr_addr;
    dbg("%s: %s\n", r->ifr_name, inet_ntoa(sin->sin_addr));
    if (0 == memcmp(ip, &sin->sin_addr.s_addr, MTL_IP_ADDR_LEN)) {
      dbg("%s: %s match the input\n", r->ifr_name, inet_ntoa(sin->sin_addr));
      ret = mt_socket_get_if_mac(r->ifr_name, ea);
      close(sock);
      free(ifr);
      return ret;
    }
  }

  close(sock);
  free(ifr);
  return -EIO;
}

int mt_socket_get_mac(struct mtl_main_impl* impl, char* if_name,
                      uint8_t dip[MTL_IP_ADDR_LEN], struct rte_ether_addr* ea,
                      int timeout_ms) {
  int sock, ret;
  struct sockaddr_in addr;
  char dummy_buf[4];
  int max_retry = 0;
  int sleep_interval_ms = 100;

  if (timeout_ms) max_retry = (timeout_ms / sleep_interval_ms) + 1;

  ret = socket_query_local_mac(dip, ea);
  if (ret >= 0) {
    dbg("%s: %u.%u.%u.%u is a local ip\n", __func__, dip[0], dip[1], dip[2], dip[3]);
    return 0;
  }

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  memcpy(&addr.sin_addr.s_addr, dip, MTL_IP_ADDR_LEN);
  addr.sin_port = htons(12345); /* which port? */

  sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock < 0) {
    err("%s, socket call fail\n", __func__);
    return sock;
  }

  int retry = 0;
  while (socket_arp_get(sock, addr.sin_addr.s_addr, ea, if_name) < 0) {
    memset(dummy_buf, 0, sizeof(dummy_buf));
    /* tx one dummy pkt to send arp request */
    if (sendto(sock, dummy_buf, 0, 0, (struct sockaddr*)&addr,
               sizeof(struct sockaddr_in)) < 0)
      continue;

    if (mt_aborted(impl)) {
      err("%s, fail as user aborted\n", __func__);
      close(sock);
      return -EIO;
    }
    if (retry >= max_retry) {
      if (max_retry) /* log only if not zero timeout */
        err("%s, fail as timeout to %d ms\n", __func__, timeout_ms);
      close(sock);
      return -EIO;
    }
    retry++;
    if (0 == (retry % 50)) {
      info("%s(%s), waiting arp from %d.%d.%d.%d\n", __func__, if_name, dip[0], dip[1],
           dip[2], dip[3]);
    }
    mt_sleep_ms(sleep_interval_ms);
  }

  close(sock);
  return 0;
}

int mt_socket_add_flow(struct mtl_main_impl* impl, enum mtl_port port, uint16_t queue_id,
                       struct mt_rxq_flow* flow) {
  char cmd[256];
  char out[128]; /* Added rule with ID 15871 */
  int ret;
  int flow_id = -1;
  uint8_t start_queue = mt_afxdp_start_queue(impl, port);

  if (flow->sys_queue) {
    err("%s(%d), sys_queue not supported\n", __func__, port);
    return -EIO;
  }
  if (flow->no_port_flow) {
    err("%s(%d), no_port_flow not supported\n", __func__, port);
    return -EIO;
  }

  if (flow->no_ip_flow) {
    snprintf(cmd, sizeof(cmd), "ethtool -N %s flow-type udp4 dst-port %u action %u",
             mt_get_user_params(impl)->port[port], flow->dst_port,
             queue_id + start_queue);
  } else if (mt_is_multicast_ip(flow->dip_addr)) {
    snprintf(cmd, sizeof(cmd),
             "ethtool -N %s flow-type udp4 dst-ip %u.%u.%u.%u dst-port %u action %u",
             mt_get_user_params(impl)->port[port], flow->dip_addr[0], flow->dip_addr[1],
             flow->dip_addr[2], flow->dip_addr[3], flow->dst_port,
             queue_id + start_queue);
  } else {
    snprintf(cmd, sizeof(cmd),
             "ethtool -N %s flow-type udp4 src-ip %u.%u.%u.%u dst-ip %u.%u.%u.%u "
             "dst-port %u action %u",
             mt_get_user_params(impl)->port[port], flow->dip_addr[0], flow->dip_addr[1],
             flow->dip_addr[2], flow->dip_addr[3], flow->sip_addr[0], flow->sip_addr[1],
             flow->sip_addr[2], flow->sip_addr[3], flow->dst_port,
             queue_id + start_queue);
  }
  ret = mt_run_cmd(cmd, out, sizeof(out));
  if (ret < 0) return ret;

  ret = sscanf(out, "Added rule with ID %d", &flow_id);
  if (ret < 0) {
    err("%s(%d), unknown out: %s\n", __func__, port, out);
    return ret;
  }

  info("%s(%d), succ, flow_id %d, cmd %s\n", __func__, port, flow_id, cmd);
  return flow_id;
}

int mt_socket_remove_flow(struct mtl_main_impl* impl, enum mtl_port port, int flow_id) {
  char cmd[128];
  int ret;

  if (flow_id > 0) {
    snprintf(cmd, sizeof(cmd), "ethtool -N %s delete %d",
             mt_get_user_params(impl)->port[port], flow_id);
    ret = mt_run_cmd(cmd, NULL, 0);
    if (ret < 0) return ret;
    info("%s(%d), succ, flow_id %d, cmd %s\n", __func__, port, flow_id, cmd);
  }

  return 0;
}
#else
int mt_socket_get_if_ip(char* if_name, uint8_t ip[MTL_IP_ADDR_LEN],
                        uint8_t netmask[MTL_IP_ADDR_LEN]) {
  return -ENOTSUP;
}

int mt_socket_get_if_gateway(char* if_name, uint8_t gateway[MTL_IP_ADDR_LEN]) {
  return -ENOTSUP;
}

int mt_socket_get_if_mac(char* if_name, struct rte_ether_addr* ea) { return -ENOTSUP; }

int mt_socket_join_mcast(struct mtl_main_impl* impl, enum mtl_port port, uint32_t group) {
  return -ENOTSUP;
}

int mt_socket_drop_mcast(struct mtl_main_impl* impl, enum mtl_port port, uint32_t group) {
  return -ENOTSUP;
}

int mt_socket_get_mac(struct mtl_main_impl* impl, char* if_name,
                      uint8_t dip[MTL_IP_ADDR_LEN], struct rte_ether_addr* ea,
                      int timeout_ms) {
  return -ENOTSUP;
}

int mt_socket_add_flow(struct mtl_main_impl* impl, enum mtl_port port, uint16_t queue_id,
                       struct mt_rxq_flow* flow) {
  return -ENOTSUP;
}

int mt_socket_remove_flow(struct mtl_main_impl* impl, enum mtl_port port, int flow_id) {
  return -ENOTSUP;
}
#endif

int mtl_get_if_ip(char* if_name, uint8_t ip[MTL_IP_ADDR_LEN],
                  uint8_t netmask[MTL_IP_ADDR_LEN]) {
  return mt_socket_get_if_ip(if_name, ip, netmask);
}
