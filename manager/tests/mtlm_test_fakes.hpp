/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

/*
 * Fake device operations for the manager unit tests. They need no NIC and no
 * privilege, and they record every call so a test can check the accounting.
 */

#ifndef _MTLM_TEST_FAKES_HPP_
#define _MTLM_TEST_FAKES_HPP_

#include <algorithm>
#include <cerrno>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "mtl_interface.hpp"
#include "mtlm_netdev.hpp"

/** One fake interface. */
struct fake_if {
  std::string name = "fake0";
  uint32_t max_combined = 8;
  uint32_t combined_count = 4;
  uint32_t table_size = 8;
  /** Locations that hold a rule. get_rules() reports these. */
  std::vector<uint32_t> rules;
};

/** Device operations over a table of fake interfaces. */
class fake_netdev : public mtlm_netdev_ops {
 public:
  std::unordered_map<unsigned int, fake_if> ifaces;

  /* Failures a test asks for. 0 means the call works. */
  int get_channels_ret = 0;
  int get_rules_ret = 0;
  int insert_rule_ret = 0;
  int delete_rule_ret = 0;

  /* What the calls did. */
  int insert_calls = 0;
  int delete_calls = 0;
  std::vector<uint32_t> deleted;
  std::vector<mtlm_flow_rule> inserted;
  std::vector<uint32_t> inserted_at;

  void add_if(unsigned int ifindex, uint32_t combined = 4, uint32_t table = 8) {
    fake_if entry;
    entry.name = "fake" + std::to_string(ifindex);
    entry.combined_count = combined;
    entry.max_combined = combined;
    entry.table_size = table;
    ifaces[ifindex] = entry;
  }

  std::string if_name(unsigned int ifindex) override {
    auto it = ifaces.find(ifindex);
    return it == ifaces.end() ? std::string() : it->second.name;
  }

  int get_channels(unsigned int ifindex, mtlm_channel_info& info) override {
    if (get_channels_ret < 0) return get_channels_ret;

    auto it = ifaces.find(ifindex);
    if (it == ifaces.end()) return -ENODEV;

    info.max_combined = it->second.max_combined;
    info.combined_count = it->second.combined_count;
    return 0;
  }

  int get_rules(unsigned int ifindex, std::vector<uint32_t>& locations,
                uint32_t& table_size) override {
    if (get_rules_ret < 0) return get_rules_ret;

    auto it = ifaces.find(ifindex);
    if (it == ifaces.end()) return -ENODEV;

    locations = it->second.rules;
    table_size = it->second.table_size;
    return 0;
  }

  int insert_rule(unsigned int ifindex, const mtlm_flow_rule& rule,
                  uint32_t location) override {
    insert_calls++;
    if (insert_rule_ret < 0) return insert_rule_ret;

    auto it = ifaces.find(ifindex);
    if (it == ifaces.end()) return -ENODEV;

    inserted.push_back(rule);
    inserted_at.push_back(location);
    it->second.rules.push_back(location);
    return static_cast<int>(location);
  }

  int delete_rule(unsigned int ifindex, uint32_t location) override {
    delete_calls++;
    if (delete_rule_ret < 0) return delete_rule_ret;

    auto it = ifaces.find(ifindex);
    if (it == ifaces.end()) return -ENODEV;

    auto& rules = it->second.rules;
    auto found = std::find(rules.begin(), rules.end(), location);
    if (found == rules.end()) return -ENOENT;

    rules.erase(found);
    deleted.push_back(location);
    return 0;
  }
};

/**
 * State of a fake XDP program.
 *
 * The interface owns the mtlm_xdp_ops it is given, so the test keeps this
 * instead and reads the calls through it.
 */
struct fake_xdp_state {
  int attach_ret = 0;
  int set_filter_ret = 0;
  int map_fd = 42;
  int attach_calls = 0;
  int detach_calls = 0;
  bool attached = false;
  /** Every set_udp_dp_filter() call, in order. */
  std::vector<std::pair<uint16_t, bool>> filter_calls;
};

class fake_xdp : public mtlm_xdp_ops {
 public:
  explicit fake_xdp(std::shared_ptr<fake_xdp_state> state) : state(std::move(state)) {
  }

  int attach(unsigned int ifindex) override {
    (void)ifindex;
    state->attach_calls++;
    if (state->attach_ret < 0) return state->attach_ret;
    state->attached = true;
    return 0;
  }

  void detach() override {
    state->detach_calls++;
    state->attached = false;
  }

  int xsks_map_fd() const override {
    return state->attached ? state->map_fd : -1;
  }

  int set_udp_dp_filter(uint16_t dst_port, bool present) override {
    state->filter_calls.push_back(std::make_pair(dst_port, present));
    return state->set_filter_ret;
  }

 private:
  std::shared_ptr<fake_xdp_state> state;
};

/** A maker for mtl_interface_registry that hands out fakes over one state. */
inline mtl_interface_registry::xdp_factory fake_xdp_factory(
    std::shared_ptr<fake_xdp_state> state) {
  return [state]() -> std::unique_ptr<mtlm_xdp_ops> {
    return std::unique_ptr<mtlm_xdp_ops>(new fake_xdp(state));
  };
}

#endif
