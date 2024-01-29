/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#ifndef _MTL_INTERFACE_HPP_
#define _MTL_INTERFACE_HPP_

#ifdef MTL_HAS_XDP_BACKEND
#include <bpf/libbpf.h>
#include <xdp/libxdp.h>
#include <xdp/xsk.h>

#include "xdp.skel.h"
#endif

#include <net/if.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "logging.hpp"

class mtl_interface {
 private:
  const int ifindex;
  struct xdp_program* xdp_prog;
  int xsks_map_fd;
  int udp4_dp_filter_fd;
  enum xdp_attach_mode xdp_mode;

 private:
  void log(const log_level& level, const std::string& message) const {
    logger::log(level, "[Interface " + std::to_string(ifindex) + "] " + message);
  }
  int clear_flow_rules();
#ifdef MTL_HAS_XDP_BACKEND
  int load_xdp();
  void unload_xdp();
#endif

 public:
  mtl_interface(int ifindex);
  ~mtl_interface();

  int get_xsks_map_fd() { return xsks_map_fd; }
  int update_udp_dp_filter(uint16_t dst_port, bool add);
};

mtl_interface::mtl_interface(int ifindex)
    : ifindex(ifindex),
      xdp_prog(nullptr),
      xsks_map_fd(-1),
      udp4_dp_filter_fd(-1),
      xdp_mode(0) {
#ifdef MTL_HAS_XDP_BACKEND
  if (load_xdp() < 0) throw std::runtime_error("Failed to load XDP program.");
#endif

  log(log_level::INFO, "Added interface.");
}

mtl_interface::~mtl_interface() {
#ifdef MTL_HAS_XDP_BACKEND
  unload_xdp();
#endif
  clear_flow_rules();

  log(log_level::INFO, "Removed interface.");
}

int mtl_interface::update_udp_dp_filter(uint16_t dst_port, bool add) {
#ifdef MTL_HAS_XDP_BACKEND
  if (xdp_prog == nullptr) {
    log(log_level::WARNING, "Default xdp prog does not support port filter.");
    return -1;
  }

  if (udp4_dp_filter_fd < 0) {
    log(log_level::WARNING, "No valid udp4_dp_filter map fd");
    return -1;
  }

  int value = add ? 1 : 0;
  if (bpf_map_update_elem(udp4_dp_filter_fd, &dst_port, &value, BPF_ANY) < 0) {
    log(log_level::WARNING, "Failed to update udp4_dp_filter map");
    return -1;
  }

  if (add)
    log(log_level::INFO, "Added port " + std::to_string(dst_port) + " to udp4_dp_filter");
  else
    log(log_level::INFO,
        "Removed port " + std::to_string(dst_port) + " from udp4_dp_filter");

  return 0;
#else
  log(log_level::WARNING,
      "update_udp_dp_filter() called but XDP backend is not enabled.");
  return -1;
#endif
}

int mtl_interface::clear_flow_rules() {
  char ifname[IF_NAMESIZE];
  if (!if_indextoname(ifindex, ifname)) {
    log(log_level::ERROR, "Failed to get interface name");
    return -1;
  }

  FILE* fp;
  char buffer[1024];
  std::string command = "ethtool -n " + std::string(ifname);

  fp = popen(command.c_str(), "r");
  if (fp == nullptr) {
    log(log_level::ERROR, "Failed to run " + command);
    return -1;
  }

  std::vector<int> rule_ids;
  int rule_id;
  while (fgets(buffer, sizeof(buffer), fp) != nullptr) {
    if (sscanf(buffer, "Filter: %d", &rule_id) == 1) {
      rule_ids.push_back(rule_id);
    }
  }

  pclose(fp);

  for (int id : rule_ids) {
    log(log_level::INFO, "Delete flow rule " + std::to_string(id));
    std::string delete_command =
        "ethtool -N " + std::string(ifname) + " delete " + std::to_string(id);
    if (system(delete_command.c_str()))
      log(log_level::WARNING, "Failed to run " + delete_command);
  }

  return 0;
}

#ifdef MTL_HAS_XDP_BACKEND
int mtl_interface::load_xdp() {
  /* get customized xdp prog path from env */
  std::string xdp_prog_path;
  char* xdp_prog_path_env = getenv("MTL_XDP_PROG_PATH");
  if (xdp_prog_path_env != nullptr) {
    xdp_prog_path = xdp_prog_path_env;
    if (!std::filesystem::is_regular_file(xdp_prog_path)) {
      log(log_level::WARNING,
          "Fallback to use built-in prog for " + xdp_prog_path + " is not valid.");
    } else {
      xdp_prog = xdp_program__open_file(xdp_prog_path.c_str(), NULL, NULL);
      if (libxdp_get_error(xdp_prog)) {
        log(log_level::WARNING, "Fallback to use built-in prog prog for " +
                                    xdp_prog_path + " cannot be loaded.");
        xdp_prog = nullptr;
      }
    }
  } else {
    xdp_prog_path = "<built-in>";
  }

  struct manager_xdp* skel = NULL;
  if (xdp_prog == nullptr) {
    /* load built-in xdp prog from skeleton */
    skel = manager_xdp__open_and_load();
    if (!skel) {
      log(log_level::ERROR, "Failed to open built-in xdp prog skeleton");
      return -1;
    }
    xdp_prog = xdp_program__from_fd(bpf_program__fd(skel->progs.xsk_def_prog));
    if (libxdp_get_error(xdp_prog)) {
      log(log_level::ERROR, "Failed to load built-in xdp prog");
      return -1;
    }
  }

  if (xdp_program__attach(xdp_prog, ifindex, XDP_MODE_NATIVE, 0) < 0) {
    log(log_level::WARNING,
        "Failed to attach XDP program with native mode, try skb mode.");
    if (xdp_program__attach(xdp_prog, ifindex, XDP_MODE_SKB, 0) < 0) {
      log(log_level::ERROR, "Failed to attach XDP program " + xdp_prog_path);
      xdp_program__close(xdp_prog);
      return -1;
    }
    xdp_mode = XDP_MODE_SKB;
  }
  xdp_mode = XDP_MODE_NATIVE;

  struct bpf_map* map = skel ? skel->maps.udp4_dp_filter
                             : bpf_object__find_map_by_name(
                                   xdp_program__bpf_obj(xdp_prog), "udp4_dp_filter");
  udp4_dp_filter_fd = bpf_map__fd(map);
  if (udp4_dp_filter_fd < 0) {
    log(log_level::ERROR, "Failed to get udp4_dp_filter fd");
    xdp_program__detach(xdp_prog, ifindex, xdp_mode, 0);
    xdp_program__close(xdp_prog);
    return -1;
  }

  if (xsk_setup_xdp_prog(ifindex, &xsks_map_fd) < 0 || xsks_map_fd < 0) {
    if (xdp_prog != nullptr) {
      xdp_program__detach(xdp_prog, ifindex, xdp_mode, 0);
      xdp_program__close(xdp_prog);
    }
    /* unload all xdp programs for the interface */
    struct xdp_multiprog* multiprog = xdp_multiprog__get_from_ifindex(ifindex);
    if (multiprog != nullptr) {
      xdp_multiprog__detach(multiprog);
      xdp_multiprog__close(multiprog);
    }

    log(log_level::ERROR, "Failed to setup AF_XDP socket.");
    return -1;
  }

  log(log_level::INFO, "Loaded xdp prog " + xdp_prog_path);
  return 0;
}

void mtl_interface::unload_xdp() {
  if (xdp_prog != nullptr) {
    xdp_program__detach(xdp_prog, ifindex, xdp_mode, 0);
    xdp_program__close(xdp_prog);
    xdp_prog = nullptr;
  } else {
    log(log_level::WARNING, "Using libxdp default prog. Try to unload all.");
  }
  /* unload all xdp programs for the interface */
  struct xdp_multiprog* multiprog = xdp_multiprog__get_from_ifindex(ifindex);
  if (multiprog != nullptr) {
    xdp_multiprog__detach(multiprog);
    xdp_multiprog__close(multiprog);
  }

  log(log_level::INFO, "Unloaded xdp prog");
}
#endif

std::unordered_map<unsigned int, std::weak_ptr<mtl_interface>> g_interfaces;

#endif