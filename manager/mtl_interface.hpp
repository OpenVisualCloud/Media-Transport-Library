/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#include <bpf/libbpf.h>
#include <xdp/libxdp.h>
#include <xdp/xsk.h>

#include <string>
#include <vector>

class mtl_interface {
 private:
  int ifindex;
  struct xdp_program* xdp_prog;

 public:
  mtl_interface(int ifindex);
  ~mtl_interface();

  int attach_xdp_prog(std::string xdp_prog_path);
  int detach_xdp_prog();
  int get_ifindex();
};

mtl_interface::mtl_interface(int ifindex) {
  this->ifindex = ifindex;
  this->xdp_prog = nullptr;
}

mtl_interface::~mtl_interface() {
  if (this->xdp_prog != nullptr) {
    this->detach_xdp_prog();
  }
}

int mtl_interface::get_ifindex() { return this->ifindex; }

int mtl_interface::attach_xdp_prog(std::string xdp_prog_path) { return 0; }

int mtl_interface::detach_xdp_prog() { return 0; }
