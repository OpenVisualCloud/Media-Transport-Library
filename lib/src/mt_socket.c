/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "mt_socket.h"

#include "mt_log.h"
#include "mt_util.h"

#ifndef WINDOWSENV
int mt_socket_get_if_ip(const char* if_name, uint8_t ip[MTL_IP_ADDR_LEN],
                        uint8_t netmask[MTL_IP_ADDR_LEN]) {
  int sock, ret;
  struct ifreq ifr;

  sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock < 0) {
    err("%s, socket call fail\n", __func__);
    return sock;
  }

  memset(&ifr, 0x0, sizeof(ifr));
  snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", if_name);
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

int mt_socket_set_if_ip(const char* if_name, uint8_t ip[MTL_IP_ADDR_LEN],
                        uint8_t netmask[MTL_IP_ADDR_LEN]) {
  struct ifreq ifr;
  int sock;
  int ret;

  sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock < 0) {
    err("%s, socket call fail\n", __func__);
    return sock;
  }

  memset(&ifr, 0x0, sizeof(ifr));
  snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", if_name);

  ifr.ifr_addr.sa_family = AF_INET;
  memcpy(&((struct sockaddr_in*)&ifr.ifr_addr)->sin_addr.s_addr, ip, MTL_IP_ADDR_LEN);
  ret = ioctl(sock, SIOCSIFADDR, &ifr);
  if (ret < 0) {
    err("%s, SIOCSIFADDR fail %d for if %s\n", __func__, ret, if_name);
    close(sock);
    return ret;
  }

  ifr.ifr_addr.sa_family = AF_INET;
  memcpy(&((struct sockaddr_in*)&ifr.ifr_addr)->sin_addr.s_addr, netmask,
         MTL_IP_ADDR_LEN);
  ret = ioctl(sock, SIOCSIFNETMASK, &ifr);
  if (ret < 0) {
    err("%s, SIOCSIFNETMASK fail %d for if %s\n", __func__, ret, if_name);
    close(sock);
    return ret;
  }

  close(sock);

  return 0;
}

int mt_socket_get_if_gateway(const char* if_name, uint8_t gateway[MTL_IP_ADDR_LEN]) {
  FILE* fp = fopen("/proc/net/route", "r");
  char line[100], iface[IF_NAMESIZE], dest[9], gway[9];

  if (fp == NULL) {
    err("%s, open /proc/net/route fail\n", __func__);
    return -EIO;
  }

  /* skip header line */
  if (!fgets(line, sizeof(line), fp)) {
    err("%s, empty file\n", __func__);
    fclose(fp);
    return -EIO;
  }

  while (fgets(line, sizeof(line), fp)) {
    sscanf(line, "%s %s %s", iface, dest, gway);
    if (strcmp(iface, if_name) == 0 && strcmp(dest, "00000000") == 0) {
      for (int i = 0; i < MTL_IP_ADDR_LEN; ++i) {
        int byte;
        sscanf(gway + (MTL_IP_ADDR_LEN - 1 - i) * 2, "%2x", &byte);
        gateway[i] = byte;
      }
      fclose(fp);
      return 0;
    }
  }

  fclose(fp);
  return -EIO;
}

int mt_socket_get_if_mac(const char* if_name, struct rte_ether_addr* ea) {
  int sock, ret;
  struct ifreq ifr;

  sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock < 0) {
    err("%s, socket call fail\n", __func__);
    return sock;
  }

  memset(&ifr, 0x0, sizeof(ifr));
  snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", if_name);
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

int mt_socket_set_if_up(const char* if_name) {
  int sock, ret;
  struct ifreq ifr;

  sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock < 0) {
    err("%s, socket call fail\n", __func__);
    return sock;
  }

  memset(&ifr, 0x0, sizeof(ifr));
  snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", if_name);
  ret = ioctl(sock, SIOCGIFFLAGS, &ifr);
  if (ret < 0) {
    err("%s, SIOCGIFFLAGS fail %d for if %s\n", __func__, ret, if_name);
    close(sock);
    return ret;
  }

  ifr.ifr_flags |= IFF_UP;
  ret = ioctl(sock, SIOCSIFFLAGS, &ifr);
  if (ret < 0) {
    err("%s, SIOCSIFFLAGS fail %d for if %s\n", __func__, ret, if_name);
    close(sock);
    return ret;
  }

  close(sock);
  return 0;
}

int mt_socket_get_numa(const char* if_name) {
  char path[256];
  snprintf(path, sizeof(path), "/sys/class/net/%s/device/numa_node", if_name);

  FILE* file = fopen(path, "r");
  if (!file) {
    err("%s, open %s fail\n", __func__, path);
    return 0;
  }

  int numa_node = 0;
  if (fscanf(file, "%d", &numa_node) != 1) {
    err("%s, fscanf %s fail\n", __func__, path);
    fclose(file);
    return 0;
  }

  fclose(file);
  dbg("%s, numa_node %d for %s\n", __func__, numa_node, if_name);
  if (SOCKET_ID_ANY == numa_node) {
    numa_node = 0;
    info("%s, direct soc_id from SOCKET_ID_ANY to 0 for %s\n", __func__, if_name);
  }
  return numa_node;
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

int mt_socket_get_mac(struct mtl_main_impl* impl, const char* if_name,
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
  struct ifreq ifr;
  int ret, fd;
  int free_loc = -1, flow_id = -1;
  uint8_t start_queue = mt_afxdp_start_queue(impl, port);
  const char* if_name = mt_kernel_if_name(impl, port);
  bool has_ip_flow = true;

  if (flow->flags & MT_RXQ_FLOW_F_SYS_QUEUE) {
    err("%s(%d), sys_queue not supported\n", __func__, port);
    return -EIO;
  }
  if (flow->flags & MT_RXQ_FLOW_F_NO_PORT) {
    err("%s(%d), no_port_flow not supported\n", __func__, port);
    return -EIO;
  }

  /* no ip flow requested */
  if (flow->flags & MT_RXQ_FLOW_F_NO_IP) has_ip_flow = false;

  if (mt_get_user_params(impl)->flags & MTL_FLAG_RX_UDP_PORT_ONLY) {
    if (has_ip_flow) {
      info("%s(%d), no ip flow as MTL_FLAG_RX_UDP_PORT_ONLY is set\n", __func__, port);
      has_ip_flow = false;
    }
  }

  /* open control socket */
  fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    err("%s(%d), cannot create control socket: %d\n", __func__, port, fd);
    return fd;
  }

  memset(&ifr, 0, sizeof(ifr));
  strncpy(ifr.ifr_name, if_name, sizeof(ifr.ifr_name) - 1);
  struct ethtool_rxnfc cmd;
  memset(&cmd, 0, sizeof(cmd));

  /* get the free location */
  cmd.cmd = ETHTOOL_GRXCLSRLCNT;
  ifr.ifr_data = (void*)&cmd;
  ret = ioctl(fd, SIOCETHTOOL, &ifr);
  if (ret < 0) {
    err("%s(%d), cannot get rule count: %s\n", __func__, port, strerror(errno));
    close(fd);
    return ret;
  }

  struct ethtool_rxnfc* cmd_w_rules;
  cmd_w_rules = calloc(1, sizeof(*cmd_w_rules) + cmd.rule_cnt * sizeof(uint32_t));
  cmd_w_rules->cmd = ETHTOOL_GRXCLSRLALL;
  cmd_w_rules->rule_cnt = cmd.rule_cnt;
  ifr.ifr_data = (void*)cmd_w_rules;
  ret = ioctl(fd, SIOCETHTOOL, &ifr);
  if (ret < 0) {
    err("%s(%d), cannot get rules: %s\n", __func__, port, strerror(errno));
    close(fd);
    free(cmd_w_rules);
    return ret;
  }

  uint32_t rule_size = cmd_w_rules->data;
  free_loc = rule_size - 1; /* start from lowest priority */
  while (free_loc > 0) {
    bool used = false;
    for (int i = 0; i < cmd.rule_cnt; i++) {
      if (cmd_w_rules->rule_locs[i] == free_loc) {
        used = true;
        break;
      }
    }
    if (used)
      free_loc--;
    else {
      info("%s(%d), found free location: %d\n", __func__, port, free_loc);
      break;
    }
  }
  if (free_loc == 0) {
    err("%s(%d), cannot find free location\n", __func__, port);
    close(fd);
    free(cmd_w_rules);
    return -EIO;
  }

  free(cmd_w_rules);

  /* set the flow rule */
  memset(&cmd, 0, sizeof(cmd));
  cmd.cmd = ETHTOOL_SRXCLSRLINS;
  struct ethtool_rx_flow_spec* fs = &cmd.fs;
  fs->flow_type = UDP_V4_FLOW;
  fs->m_u.udp_ip4_spec.pdst = 0xFFFF;
  fs->h_u.udp_ip4_spec.pdst = htons(flow->dst_port);
  if (has_ip_flow) {
    fs->m_u.udp_ip4_spec.ip4dst = 0xFFFFFFFF;
    if (mt_is_multicast_ip(flow->dip_addr)) {
      rte_memcpy(&fs->h_u.udp_ip4_spec.ip4dst, flow->dip_addr, MTL_IP_ADDR_LEN);
    } else {
      fs->m_u.udp_ip4_spec.ip4src = 0xFFFFFFFF;
      rte_memcpy(&fs->h_u.udp_ip4_spec.ip4src, flow->dip_addr, MTL_IP_ADDR_LEN);
      rte_memcpy(&fs->h_u.udp_ip4_spec.ip4dst, flow->sip_addr, MTL_IP_ADDR_LEN);
    }
  }
  fs->ring_cookie = queue_id + start_queue;
  fs->location = free_loc; /* for some NICs the location must be set */

  ifr.ifr_data = (void*)&cmd;
  ret = ioctl(fd, SIOCETHTOOL, &ifr);
  if (ret < 0) {
    err("%s(%d), cannot insert classifier: %s, start_queue %u, if %s\n", __func__, port,
        strerror(errno), start_queue, if_name);
    if (ret == -EPERM)
      err("%s(%d), please add capability for the app: sudo setcap 'cap_net_admin+ep' "
          "<app>\n",
          __func__, port);
    close(fd);
    return ret;
  }
  flow_id = fs->location;

  close(fd);

  info("%s(%d), succ, flow_id %d\n", __func__, port, flow_id);
  return flow_id;
}

int mt_socket_remove_flow(struct mtl_main_impl* impl, enum mtl_port port, int flow_id) {
  struct ethtool_rxnfc cmd;
  int ret, fd;
  const char* if_name = mt_kernel_if_name(impl, port);

  if (flow_id <= 0) {
    err("%s(%d), flow_id %d is invalid\n", __func__, port, flow_id);
    return -EINVAL;
  }

  memset(&cmd, 0, sizeof(cmd));
  cmd.cmd = ETHTOOL_SRXCLSRLDEL;
  cmd.fs.location = flow_id;

  fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    err("%s(%d), cannot get control socket: %d\n", __func__, port, fd);
    return fd;
  }

  struct ifreq ifr;
  memset(&ifr, 0, sizeof(ifr));
  strncpy(ifr.ifr_name, if_name, IFNAMSIZ - 1);
  ifr.ifr_data = (char*)&cmd;

  ret = ioctl(fd, SIOCETHTOOL, &ifr);
  if (ret < 0) {
    err("%s(%d), cannot remove classifier: %s\n", __func__, port, strerror(errno));
    if (ret == -EPERM)
      err("%s(%d), please add capability for the app: sudo setcap 'cap_net_admin+ep' "
          "<app>\n",
          __func__, port);
    close(fd);
    return ret;
  }
  close(fd);
  info("%s(%d), succ, flow_id %d\n", __func__, port, flow_id);

  return 0;
}
#else
int mt_socket_get_if_ip(const char* if_name, uint8_t ip[MTL_IP_ADDR_LEN],
                        uint8_t netmask[MTL_IP_ADDR_LEN]) {
  MTL_MAY_UNUSED(if_name);
  MTL_MAY_UNUSED(ip);
  MTL_MAY_UNUSED(netmask);
  return -ENOTSUP;
}

int mt_socket_get_if_gateway(const char* if_name, uint8_t gateway[MTL_IP_ADDR_LEN]) {
  MTL_MAY_UNUSED(if_name);
  MTL_MAY_UNUSED(gateway);
  return -ENOTSUP;
}

int mt_socket_get_if_mac(const char* if_name, struct rte_ether_addr* ea) {
  MTL_MAY_UNUSED(if_name);
  MTL_MAY_UNUSED(ea);
  return -ENOTSUP;
}

int mt_socket_get_numa(const char* if_name) {
  MTL_MAY_UNUSED(if_name);
  return 0;
}

int mt_socket_join_mcast(struct mtl_main_impl* impl, enum mtl_port port, uint32_t group) {
  MTL_MAY_UNUSED(impl);
  MTL_MAY_UNUSED(port);
  MTL_MAY_UNUSED(group);
  return -ENOTSUP;
}

int mt_socket_leave_mcast(struct mtl_main_impl* impl, enum mtl_port port,
                          uint32_t group) {
  MTL_MAY_UNUSED(impl);
  MTL_MAY_UNUSED(port);
  MTL_MAY_UNUSED(group);
  return -ENOTSUP;
}

int mt_socket_get_mac(struct mtl_main_impl* impl, const char* if_name,
                      uint8_t dip[MTL_IP_ADDR_LEN], struct rte_ether_addr* ea,
                      int timeout_ms) {
  MTL_MAY_UNUSED(impl);
  MTL_MAY_UNUSED(if_name);
  MTL_MAY_UNUSED(dip);
  MTL_MAY_UNUSED(ea);
  MTL_MAY_UNUSED(timeout_ms);
  return -ENOTSUP;
}

int mt_socket_add_flow(struct mtl_main_impl* impl, enum mtl_port port, uint16_t queue_id,
                       struct mt_rxq_flow* flow) {
  MTL_MAY_UNUSED(impl);
  MTL_MAY_UNUSED(port);
  MTL_MAY_UNUSED(queue_id);
  MTL_MAY_UNUSED(flow);
  return -ENOTSUP;
}

int mt_socket_remove_flow(struct mtl_main_impl* impl, enum mtl_port port, int flow_id) {
  MTL_MAY_UNUSED(impl);
  MTL_MAY_UNUSED(port);
  MTL_MAY_UNUSED(flow_id);
  return -ENOTSUP;
}
#endif

int mtl_get_if_ip(char* if_name, uint8_t ip[MTL_IP_ADDR_LEN],
                  uint8_t netmask[MTL_IP_ADDR_LEN]) {
  return mt_socket_get_if_ip(if_name, ip, netmask);
}
