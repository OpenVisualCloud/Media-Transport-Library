/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

/**
 * @file mtlm_netdev.hpp
 *
 * Everything the manager needs from a network device, behind one interface.
 *
 * The Linux build talks ethtool ioctl and libxdp. A unit test gives a fake, so
 * the queue accounting, the flow accounting and the filter reference counting
 * run with no NIC and no root.
 */

#ifndef _MTLM_NETDEV_HPP_
#define _MTLM_NETDEV_HPP_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

/**
 * One ethtool receive flow rule.
 *
 * Addresses are network byte order and ports are host byte order, which is
 * what ethtool itself asks for. See struct mtlm_flow in mtlm_api.h.
 */
struct mtlm_flow_rule {
  uint32_t flow_type;
  /** 0 keeps the field out of the match. */
  uint32_t src_ip;
  uint32_t dst_ip;
  uint16_t src_port;
  uint16_t dst_port;
  /** Receive queue the match steers to. */
  uint16_t queue_id;
};

/** Receive channel counts of an interface. */
struct mtlm_channel_info {
  uint32_t max_combined;
  uint32_t combined_count;
};

/** Device operations. Every call returns 0 or a resource id, else -errno. */
class mtlm_netdev_ops {
 public:
  virtual ~mtlm_netdev_ops() = default;

  /** Name of `ifindex`, or an empty string when there is no such interface. */
  virtual std::string if_name(unsigned int ifindex) = 0;

  virtual int get_channels(unsigned int ifindex, mtlm_channel_info& info) = 0;

  /**
   * Read the installed receive flow rules.
   *
   * @param locations  Receives the location of each installed rule.
   * @param table_size Receives the number of locations the device has.
   */
  virtual int get_rules(unsigned int ifindex, std::vector<uint32_t>& locations,
                        uint32_t& table_size) = 0;

  /** Insert `rule` at `location`. @return the location, else -errno. */
  virtual int insert_rule(unsigned int ifindex, const mtlm_flow_rule& rule,
                          uint32_t location) = 0;

  virtual int delete_rule(unsigned int ifindex, uint32_t location) = 0;
};

/** The XDP program of one interface and the maps it owns. */
class mtlm_xdp_ops {
 public:
  virtual ~mtlm_xdp_ops() = default;

  /** Load and attach the program. @return 0, else -errno. */
  virtual int attach(unsigned int ifindex) = 0;

  /** Detach and close. Safe to call when attach() failed or never ran. */
  virtual void detach() = 0;

  /** Descriptor of the xsks map, or -1 when there is none. */
  virtual int xsks_map_fd() const = 0;

  /** Add or remove one UDP destination port in the filter map. */
  virtual int set_udp_dp_filter(uint16_t dst_port, bool present) = 0;
};

/** Whether this build has libxdp and libbpf. */
bool mtlm_xdp_supported();

/** Device operations that use ethtool ioctl. */
std::shared_ptr<mtlm_netdev_ops> mtlm_netdev_linux();

/**
 * XDP operations for one interface.
 *
 * Without libxdp this returns an object whose attach() reports -ENOTSUP, so a
 * caller never needs to test mtlm_xdp_supported() first.
 */
std::unique_ptr<mtlm_xdp_ops> mtlm_xdp_create();

#endif
