/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#include "mtl_interface.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <stdexcept>

int mtlm_pick_free_rule_location(const std::vector<uint32_t>& used, uint32_t table_size) {
  /* A device that reports no rule table cannot hold a rule. The old code read
   * this as location -1 and asked the driver to install there. */
  if (table_size == 0) return -ENOSPC;

  for (uint32_t loc = table_size - 1; loc > 0; loc--) {
    if (std::find(used.begin(), used.end(), loc) == used.end())
      return static_cast<int>(loc);
  }

  return -ENOSPC;
}

mtl_interface::mtl_interface(unsigned int ifindex,
                             std::shared_ptr<mtlm_netdev_ops> netdev,
                             std::unique_ptr<mtlm_xdp_ops> xdp, bool require_xdp)
    : ifindex(ifindex),
      netdev(std::move(netdev)),
      xdp(std::move(xdp)),
      xdp_attached(false),
      max_combined(0),
      combined_count(0) {
  if (this->netdev == nullptr || this->xdp == nullptr)
    throw std::runtime_error("No device operations for the interface.");

  if (this->netdev->if_name(ifindex).empty())
    throw std::runtime_error("No interface with index " + std::to_string(ifindex));

  clear_flow_rules();

  int ret = this->xdp->attach(ifindex);
  if (ret == 0) {
    xdp_attached = true;
  } else if (require_xdp) {
    throw std::runtime_error("Failed to attach the XDP program: " +
                             std::string(std::strerror(-ret)));
  } else {
    /* Not fatal. Queue and flow arbitration needs no XDP, and refusing the
     * whole interface here used to make the manager useless on a build or a
     * host without libxdp. */
    log(log_level::DEBUG, "No XDP program attached, queues and flows still work.");
  }

  mtlm_channel_info info = {};
  ret = this->netdev->get_channels(ifindex, info);
  if (ret < 0) {
    this->xdp->detach();
    xdp_attached = false;
    throw std::runtime_error("Failed to read the channel counts: " +
                             std::string(std::strerror(-ret)));
  }

  max_combined = info.max_combined;
  combined_count = info.combined_count;

  queues.resize(combined_count, false);
  if (queues.empty()) {
    log(log_level::WARNING,
        "The interface reports no combined channel, so it can hand out no queue.");
  } else {
    queues[0] = true; /* queue 0 stays with the kernel */
  }

  log(log_level::INFO, "Added interface, max_combined " + std::to_string(max_combined) +
                           " combined_count " + std::to_string(combined_count) +
                           ", xdp " + (xdp_attached ? "on" : "off"));
}

mtl_interface::~mtl_interface() {
  if (xdp != nullptr) xdp->detach();
  clear_flow_rules();

  log(log_level::INFO, "Removed interface.");
}

void mtl_interface::log(const log_level& level, const std::string& message) const {
  logger::log(level, "[Interface " + std::to_string(ifindex) + "] " + message);
}

int mtl_interface::get_xsks_map_fd() const {
  if (!xdp_attached) return -1;
  return xdp->xsks_map_fd();
}

int mtl_interface::update_udp_dp_filter(uint16_t dst_port, bool add) {
  if (!xdp_attached) {
    log(log_level::WARNING, "No XDP program, so no UDP port filter.");
    return -ENOTSUP;
  }

  return xdp->set_udp_dp_filter(dst_port, add);
}

int mtl_interface::get_queue() {
  auto it = std::find(queues.begin(), queues.end(), false);
  if (it == queues.end()) {
    log(log_level::ERROR, "No free queue");
    return -ENOSPC;
  }

  size_t q = static_cast<size_t>(std::distance(queues.begin(), it));
  queues[q] = true;
  log(log_level::INFO, "Get queue " + std::to_string(q));
  return static_cast<int>(q);
}

int mtl_interface::put_queue(uint16_t queue_id) {
  /* Queue 0 belongs to the kernel and get_queue() never hands it out, so a put
   * of it can only be a mistake. Accepting it would put queue 0 in the free
   * pool and the next instance would take the queue the kernel uses. */
  if (queue_id == 0 || queue_id >= queues.size() || !queues[queue_id]) {
    log(log_level::ERROR, "Invalid or free queue " + std::to_string(queue_id));
    return -EINVAL;
  }

  queues[queue_id] = false;
  log(log_level::INFO, "Put queue " + std::to_string(queue_id));
  return 0;
}

int mtl_interface::clear_flow_rules() {
  std::vector<uint32_t> used;
  uint32_t table_size = 0;

  int ret = netdev->get_rules(ifindex, used, table_size);
  if (ret < 0) {
    log(log_level::ERROR,
        "Failed to read the flow rules: " + std::string(std::strerror(-ret)));
    return ret;
  }

  for (uint32_t loc : used) {
    ret = netdev->delete_rule(ifindex, loc);
    if (ret < 0)
      log(log_level::WARNING, "Failed to clear rule " + std::to_string(loc));
    else
      log(log_level::INFO, "Rule " + std::to_string(loc) + " cleared");
  }

  return 0;
}

int mtl_interface::add_flow(uint16_t queue_id, uint32_t flow_type, uint32_t src_ip,
                            uint32_t dst_ip, uint16_t src_port, uint16_t dst_port) {
  std::vector<uint32_t> used;
  uint32_t table_size = 0;

  int ret = netdev->get_rules(ifindex, used, table_size);
  if (ret < 0) {
    log(log_level::ERROR,
        "Failed to read the flow rules: " + std::string(std::strerror(-ret)));
    return ret;
  }

  int location = mtlm_pick_free_rule_location(used, table_size);
  if (location < 0) {
    log(log_level::ERROR, "Cannot find a free rule location, table size " +
                              std::to_string(table_size) + ", " +
                              std::to_string(used.size()) + " in use");
    return location;
  }

  mtlm_flow_rule rule = {};
  rule.flow_type = flow_type;
  rule.src_ip = src_ip;
  rule.dst_ip = dst_ip;
  rule.src_port = src_port;
  rule.dst_port = dst_port;
  rule.queue_id = queue_id;

  ret = netdev->insert_rule(ifindex, rule, static_cast<uint32_t>(location));
  if (ret < 0) {
    log(log_level::ERROR,
        "Cannot insert a flow rule: " + std::string(std::strerror(-ret)));
    return ret;
  }

  log(log_level::INFO, "Inserted flow rule " + std::to_string(ret) + " with queue " +
                           std::to_string(queue_id));
  return ret;
}

int mtl_interface::del_flow(uint32_t flow_id) {
  int ret = netdev->delete_rule(ifindex, flow_id);
  if (ret < 0) {
    log(log_level::ERROR, "Cannot delete flow rule " + std::to_string(flow_id) + ": " +
                              std::string(std::strerror(-ret)));
    return ret;
  }

  log(log_level::INFO, "Deleted flow rule " + std::to_string(flow_id));
  return 0;
}

mtl_interface_registry::mtl_interface_registry(std::shared_ptr<mtlm_netdev_ops> netdev,
                                               xdp_factory make_xdp)
    : netdev(std::move(netdev)), make_xdp(std::move(make_xdp)) {
  if (!this->make_xdp) this->make_xdp = mtlm_xdp_create;
}

std::shared_ptr<mtl_interface> mtl_interface_registry::get(unsigned int ifindex,
                                                           bool require_xdp) {
  auto it = interfaces.find(ifindex);
  if (it != interfaces.end()) {
    if (auto live = it->second.lock()) return live;
    interfaces.erase(it); /* the last user went away, so build a new one */
  }

  try {
    auto created =
        std::make_shared<mtl_interface>(ifindex, netdev, make_xdp(), require_xdp);
    interfaces[ifindex] = created;
    return created;
  } catch (const std::exception& e) {
    logger::log(log_level::ERROR, "[Interface " + std::to_string(ifindex) +
                                      "] Failed to initialize: " + e.what());
    return nullptr;
  }
}

size_t mtl_interface_registry::live_count() const {
  size_t count = 0;

  for (const auto& pair : interfaces)
    if (!pair.second.expired()) count++;

  return count;
}
