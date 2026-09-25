/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

/*
 * mtlm_netdev_ops over the ethtool ioctl.
 */

#include <arpa/inet.h>
#include <linux/ethtool.h>
#include <linux/sockios.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

#include "logging.hpp"
#include "mtlm_netdev.hpp"

namespace {

/* One socket and one ifreq, for the life of a few ethtool calls. */
class ethtool_request {
 public:
  explicit ethtool_request(const std::string& name) : fd_(-1) {
    std::memset(&ifr_, 0, sizeof(ifr_));
    if (name.empty() || name.size() >= IF_NAMESIZE) return;
    std::snprintf(ifr_.ifr_name, IF_NAMESIZE, "%s", name.c_str());
    fd_ = socket(AF_INET, SOCK_DGRAM, 0);
  }

  ~ethtool_request() {
    if (fd_ >= 0) close(fd_);
  }

  ethtool_request(const ethtool_request&) = delete;
  ethtool_request& operator=(const ethtool_request&) = delete;

  bool ok() const {
    return fd_ >= 0;
  }

  int call(void* data) {
    ifr_.ifr_data = reinterpret_cast<caddr_t>(data);
    if (ioctl(fd_, SIOCETHTOOL, &ifr_) < 0) return -errno;
    return 0;
  }

 private:
  int fd_;
  struct ifreq ifr_;
};

class netdev_linux : public mtlm_netdev_ops {
 public:
  std::string if_name(unsigned int ifindex) override {
    char name[IF_NAMESIZE] = {0};

    if (if_indextoname(ifindex, name) == nullptr) return std::string();
    return std::string(name);
  }

  int get_channels(unsigned int ifindex, mtlm_channel_info& info) override {
    struct ethtool_channels channels = {};
    ethtool_request req(if_name(ifindex));

    info.max_combined = 0;
    info.combined_count = 0;
    if (!req.ok()) return -ENODEV;

    channels.cmd = ETHTOOL_GCHANNELS;
    int ret = req.call(&channels);
    if (ret < 0) return ret;

    info.max_combined = channels.max_combined;
    info.combined_count = channels.combined_count;
    return 0;
  }

  int get_rules(unsigned int ifindex, std::vector<uint32_t>& locations,
                uint32_t& table_size) override {
    struct ethtool_rxnfc count = {};
    ethtool_request req(if_name(ifindex));

    locations.clear();
    table_size = 0;
    if (!req.ok()) return -ENODEV;

    count.cmd = ETHTOOL_GRXCLSRLCNT;
    int ret = req.call(&count);
    if (ret < 0) return ret;

    /* ETHTOOL_GRXCLSRLALL wants one uint32_t of room per installed rule after
     * the command struct, and answers with the table size in `data`. */
    std::vector<uint8_t> buf(sizeof(struct ethtool_rxnfc) +
                                 static_cast<size_t>(count.rule_cnt) * sizeof(uint32_t),
                             0);
    auto* all = reinterpret_cast<struct ethtool_rxnfc*>(buf.data());
    all->cmd = ETHTOOL_GRXCLSRLALL;
    all->rule_cnt = count.rule_cnt;

    ret = req.call(all);
    if (ret < 0) return ret;

    table_size = all->data;

    /* Trust the smaller of the two counts: the kernel may report fewer rules
     * than the first call did, and reading past the buffer would be a fault. */
    uint32_t got = all->rule_cnt < count.rule_cnt ? all->rule_cnt : count.rule_cnt;
    locations.assign(all->rule_locs, all->rule_locs + got);
    return 0;
  }

  int insert_rule(unsigned int ifindex, const mtlm_flow_rule& rule,
                  uint32_t location) override {
    struct ethtool_rxnfc cmd = {};
    ethtool_request req(if_name(ifindex));

    if (!req.ok()) return -ENODEV;

    cmd.cmd = ETHTOOL_SRXCLSRLINS;
    struct ethtool_rx_flow_spec* fs = &cmd.fs;
    fs->flow_type = rule.flow_type;
    if (rule.dst_port) {
      fs->m_u.udp_ip4_spec.pdst = 0xFFFF;
      fs->h_u.udp_ip4_spec.pdst = htons(rule.dst_port);
    }
    if (rule.src_port) {
      fs->m_u.udp_ip4_spec.psrc = 0xFFFF;
      fs->h_u.udp_ip4_spec.psrc = htons(rule.src_port);
    }
    if (rule.dst_ip) {
      fs->m_u.udp_ip4_spec.ip4dst = 0xFFFFFFFF;
      fs->h_u.udp_ip4_spec.ip4dst = rule.dst_ip;
    }
    if (rule.src_ip) {
      fs->m_u.udp_ip4_spec.ip4src = 0xFFFFFFFF;
      fs->h_u.udp_ip4_spec.ip4src = rule.src_ip;
    }
    fs->ring_cookie = rule.queue_id;
    fs->location = location; /* some NICs refuse to pick a location themselves */

    int ret = req.call(&cmd);
    if (ret < 0) return ret;

    return static_cast<int>(fs->location);
  }

  int delete_rule(unsigned int ifindex, uint32_t location) override {
    struct ethtool_rxnfc cmd = {};
    ethtool_request req(if_name(ifindex));

    if (!req.ok()) return -ENODEV;

    cmd.cmd = ETHTOOL_SRXCLSRLDEL;
    cmd.fs.location = location;

    return req.call(&cmd);
  }
};

} /* namespace */

std::shared_ptr<mtlm_netdev_ops> mtlm_netdev_linux() {
  return std::make_shared<netdev_linux>();
}
