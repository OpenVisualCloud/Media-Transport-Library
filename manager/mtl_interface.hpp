/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#ifndef _MTL_INTERFACE_HPP_
#define _MTL_INTERFACE_HPP_

#if 0 /* TODO: add xdp loader/unloader */
#include <bpf/libbpf.h>
#include <xdp/libxdp.h>
#include <xdp/xsk.h>
#endif

#include <memory>
#include <string>
#include <unordered_map>

#include "logging.hpp"

static const std::string xdp_prog_path = "/var/run/imtl/xdp_prog.o";

class mtl_interface {
 private:
  const int ifindex;
  struct xdp_program* xdp_prog;

 public:
  mtl_interface(int ifindex);
  ~mtl_interface();
};

mtl_interface::mtl_interface(int ifindex) : ifindex(ifindex) {
  logger::log(log_level::INFO, "Add interface " + std::to_string(ifindex));
  xdp_prog = nullptr;
}

mtl_interface::~mtl_interface() {
  logger::log(log_level::INFO, "Remove interface " + std::to_string(ifindex));
}

std::unordered_map<unsigned int, std::weak_ptr<mtl_interface>> g_interfaces;

#endif