/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#ifndef _MTL_INTERFACE_HPP_
#define _MTL_INTERFACE_HPP_

#ifdef MTL_HAS_XDP_BACKEND
#include <xdp/libxdp.h>
#include <xdp/xsk.h>
#endif

#include <memory>
#include <string>
#include <unordered_map>

#include "logging.hpp"

class mtl_interface {
 private:
  const int ifindex;
  struct xdp_program* xdp_prog;
  int xsks_map_fd;

 public:
  mtl_interface(int ifindex);
  ~mtl_interface();

  int get_xsks_map_fd() { return xsks_map_fd; }
};

mtl_interface::mtl_interface(int ifindex)
    : ifindex(ifindex), xdp_prog(nullptr), xsks_map_fd(-1) {
#ifdef MTL_HAS_XDP_BACKEND
  /* get xdp prog path from env or use manager default path */
  std::string xdp_prog_path;
  char* xdp_prog_path_env = getenv("MTL_XDP_PROG_PATH");
  if (xdp_prog_path_env != nullptr) {
    xdp_prog_path = xdp_prog_path_env;
  } else {
    xdp_prog_path = "/tmp/imtl/xdp_prog.o";
  }

  if (!std::filesystem::is_regular_file(xdp_prog_path)) {
    logger::log(log_level::WARNING, "Fallback to use libxdp default prog for " +
                                        xdp_prog_path + " is not valid.");
  } else {
    xdp_prog = xdp_program__open_file(xdp_prog_path.c_str(), NULL, NULL);
    if (libxdp_get_error(xdp_prog)) {
      logger::log(log_level::WARNING, "Fallback to use libxdp default prog for " +
                                          xdp_prog_path + " cannot load.");
      xdp_prog = nullptr;
    } else {
      if (xdp_program__attach(xdp_prog, ifindex, XDP_MODE_NATIVE, 0) < 0) {
        xdp_program__close(xdp_prog);
        throw std::runtime_error("Failed to attach XDP program " + xdp_prog_path);
      }
    }
  }

  if (xsk_setup_xdp_prog(ifindex, &xsks_map_fd) < 0 || xsks_map_fd < 0) {
    if (xdp_prog != nullptr) {
      xdp_program__detach(xdp_prog, ifindex, XDP_MODE_NATIVE, 0);
      xdp_program__close(xdp_prog);
    }
    /* unload all xdp programs for the interface */
    struct xdp_multiprog* multiprog = xdp_multiprog__get_from_ifindex(ifindex);
    if (multiprog != nullptr) {
      xdp_multiprog__detach(multiprog);
      xdp_multiprog__close(multiprog);
    }

    throw std::runtime_error("Failed to setup AF_XDP socket.");
  }

  if (xdp_prog == nullptr) xdp_prog_path = "<libxdp_default>";
  logger::log(log_level::INFO, "Add interface " + std::to_string(ifindex) +
                                   " with xdp prog " + xdp_prog_path);
#else
  logger::log(log_level::INFO, "Add interface " + std::to_string(ifindex));
#endif
}

mtl_interface::~mtl_interface() {
#ifdef MTL_HAS_XDP_BACKEND
  if (xdp_prog != nullptr) {
    xdp_program__detach(xdp_prog, ifindex, XDP_MODE_NATIVE, 0);
    xdp_program__close(xdp_prog);
    xdp_prog = nullptr;
  } else {
    logger::log(log_level::WARNING,
                "Using libxdp default prog. Try to unload all for interface " +
                    std::to_string(ifindex));
  }
  /* unload all xdp programs for the interface */
  struct xdp_multiprog* multiprog = xdp_multiprog__get_from_ifindex(ifindex);
  if (multiprog != nullptr) {
    xdp_multiprog__detach(multiprog);
    xdp_multiprog__close(multiprog);
  }
#endif

  logger::log(log_level::INFO, "Remove interface " + std::to_string(ifindex));
}

std::unordered_map<unsigned int, std::weak_ptr<mtl_interface>> g_interfaces;

#endif