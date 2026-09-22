/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#ifndef _MTL_INTERFACE_HPP_
#define _MTL_INTERFACE_HPP_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "logging.hpp"
#include "mtlm_netdev.hpp"

/**
 * Pick a free rule location, from the lowest priority end of the table.
 *
 * Location 0 is the highest priority slot and stays free on purpose, so a rule
 * the operator installs by hand always wins.
 *
 * @param used       Locations that already hold a rule.
 * @param table_size Number of locations the device has.
 * @return The location, 1 or greater, or -ENOSPC.
 */
int mtlm_pick_free_rule_location(const std::vector<uint32_t>& used, uint32_t table_size);

/**
 * One network interface the manager arbitrates.
 *
 * It owns the receive queues of that interface, the ethtool flow rules it
 * installed, and the XDP program when there is one. Every instance that names
 * the same ifindex shares one object, so the counts stay right across clients.
 *
 * All device access goes through mtlm_netdev_ops and mtlm_xdp_ops, so the
 * accounting in this class runs in a unit test with no NIC.
 */
class mtl_interface {
 public:
  /**
   * @param ifindex      Interface to take over.
   * @param netdev       Device operations, must not be null.
   * @param xdp          XDP operations, must not be null.
   * @param require_xdp  Fail construction when the XDP program cannot attach.
   *                     Pass true only for an interface an AF_XDP session
   *                     needs. Queue and flow work needs no XDP.
   * @throw std::runtime_error when the interface cannot be taken over.
   */
  mtl_interface(unsigned int ifindex, std::shared_ptr<mtlm_netdev_ops> netdev,
                std::unique_ptr<mtlm_xdp_ops> xdp, bool require_xdp);
  ~mtl_interface();

  mtl_interface(const mtl_interface&) = delete;
  mtl_interface& operator=(const mtl_interface&) = delete;

  unsigned int get_ifindex() const {
    return ifindex;
  }

  /** Descriptor of the xsks map, or -1 when no XDP program is attached. */
  int get_xsks_map_fd() const;

  /** Whether an XDP program is attached. */
  bool has_xdp() const {
    return xdp_attached;
  }

  /**
   * Add or remove a UDP destination port in the XDP filter map.
   *
   * @return 0 on success, -ENOTSUP with no XDP program, else a negative errno.
   */
  int update_udp_dp_filter(uint16_t dst_port, bool add);

  /**
   * Reserve the lowest free receive queue.
   *
   * @return The queue id, 1 or greater, or -ENOSPC when none is free. Queue 0
   *         stays with the kernel.
   */
  int get_queue();

  /**
   * Release a queue.
   *
   * @return 0 on success, -EINVAL when the id is 0, out of range, or free.
   */
  int put_queue(uint16_t queue_id);

  /** Number of queues this interface can hand out, queue 0 included. */
  size_t queue_count() const {
    return queues.size();
  }

  /**
   * Install a flow rule in the lowest priority free location.
   *
   * @return The location, 1 or greater, or a negative errno. -ENOSPC when the
   *         rule table is full or the device reports no table.
   */
  int add_flow(uint16_t queue_id, uint32_t flow_type, uint32_t src_ip, uint32_t dst_ip,
               uint16_t src_port, uint16_t dst_port);

  /** Delete a flow rule. @return 0 on success, else a negative errno. */
  int del_flow(uint32_t flow_id);

 private:
  void log(const log_level& level, const std::string& message) const;

  /** Delete every flow rule the device holds, ours and anyone else's. */
  int clear_flow_rules();

  const unsigned int ifindex;
  std::shared_ptr<mtlm_netdev_ops> netdev;
  std::unique_ptr<mtlm_xdp_ops> xdp;
  bool xdp_attached;
  uint32_t max_combined;
  uint32_t combined_count;
  std::vector<bool> queues;
};

/**
 * The one mtl_interface per ifindex, shared by every instance that asks.
 *
 * The registry keeps a weak reference, so an interface goes away, and gives up
 * its queues and rules, when the last instance that used it disconnects.
 */
class mtl_interface_registry {
 public:
  /** Maker of the XDP operations of one interface. */
  using xdp_factory = std::function<std::unique_ptr<mtlm_xdp_ops>()>;

  /**
   * @param netdev       Device operations every interface uses.
   * @param make_xdp     Maker of the XDP operations. An empty one asks for
   *                     mtlm_xdp_create(), which is what the server uses.
   */
  explicit mtl_interface_registry(std::shared_ptr<mtlm_netdev_ops> netdev,
                                  xdp_factory make_xdp = nullptr);

  /**
   * Interface for `ifindex`, created when no live one exists.
   *
   * @param require_xdp Passed to the constructor on creation. It has no effect
   *                    on an interface that already exists.
   * @return The interface, or nullptr when it cannot be taken over.
   */
  std::shared_ptr<mtl_interface> get(unsigned int ifindex, bool require_xdp);

  /** Number of live interfaces. */
  size_t live_count() const;

 private:
  std::shared_ptr<mtlm_netdev_ops> netdev;
  xdp_factory make_xdp;
  std::unordered_map<unsigned int, std::weak_ptr<mtl_interface>> interfaces;
};

#endif
