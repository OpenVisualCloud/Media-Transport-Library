/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

/*
 * mtlm_xdp_ops over libxdp and libbpf.
 *
 * Without those two libraries the build still provides the interface, and
 * every call reports -ENOTSUP. The manager then serves lcore, queue and flow
 * requests as usual, which do not need XDP at all.
 */

#ifdef MTL_HAS_XDP_BACKEND
#include <bpf/libbpf.h>
#include <xdp/libxdp.h>
#include <xdp/xsk.h>
#endif

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>

#include "logging.hpp"
#include "mtlm_netdev.hpp"

/** Environment variable that names the XDP object file to load. */
#define MTLM_XDP_OBJECT_ENV "MTL_MANAGER_XDP_OBJECT"

/** Object file libxdp looks for in its search path when the variable is unset. */
#define MTLM_XDP_OBJECT_DEFAULT "mtl.xdp.o"

namespace {

#ifdef MTL_HAS_XDP_BACKEND

class xdp_libxdp : public mtlm_xdp_ops {
 public:
  ~xdp_libxdp() override {
    detach();
  }

  int attach(unsigned int ifindex) override {
    if (prog_ != nullptr) return -EALREADY;

    const char* object = getenv(MTLM_XDP_OBJECT_ENV);
    if (object == nullptr || object[0] == '\0') object = MTLM_XDP_OBJECT_DEFAULT;

    /* xdp_program__find_file() takes a bare file name and looks for it in the
     * libxdp search path. It cannot open a path, so a name that holds a '/' has
     * to go to xdp_program__open_file(). Without this an absolute path in
     * MTLM_XDP_OBJECT_ENV, which is what a build tree gives, was refused with
     * "Couldn't find a BPF file with name". */
    struct xdp_program* prog = strchr(object, '/') == nullptr
                                   ? xdp_program__find_file(object, nullptr, nullptr)
                                   : xdp_program__open_file(object, nullptr, nullptr);
    if (prog == nullptr || libxdp_get_error(prog) != 0) {
      log(log_level::ERROR, "Failed to load XDP object " + std::string(object) +
                                ". Set " MTLM_XDP_OBJECT_ENV
                                " to its path, or install it.");
      return -ENOENT;
    }

    /* Native mode needs driver support. Skb mode works anywhere and is slower,
     * so keep it as the fallback and remember which one took, because the
     * detach must name the same mode. */
    enum xdp_attach_mode mode = XDP_MODE_NATIVE;
    if (xdp_program__attach(prog, ifindex, XDP_MODE_NATIVE, 0) < 0) {
      log(log_level::WARNING,
          "Failed to attach the XDP program in native mode, trying skb mode.");
      if (xdp_program__attach(prog, ifindex, XDP_MODE_SKB, 0) < 0) {
        log(log_level::ERROR, "Failed to attach the XDP program.");
        xdp_program__close(prog);
        return -EIO;
      }
      mode = XDP_MODE_SKB;
    }

    prog_ = prog;
    mode_ = mode;
    ifindex_ = ifindex;

    if (xsk_setup_xdp_prog(ifindex, &xsks_map_fd_) < 0 || xsks_map_fd_ < 0) {
      log(log_level::ERROR, "Failed to set up the AF_XDP socket map.");
      detach();
      return -EIO;
    }

    struct bpf_map* map =
        bpf_object__find_map_by_name(xdp_program__bpf_obj(prog_), "udp4_dp_filter");
    if (map == nullptr) {
      log(log_level::ERROR, "The XDP object has no udp4_dp_filter map.");
      detach();
      return -ENOENT;
    }

    udp4_dp_filter_fd_ = bpf_map__fd(map);
    if (udp4_dp_filter_fd_ < 0) {
      log(log_level::ERROR, "Failed to get the udp4_dp_filter map descriptor.");
      detach();
      return -EIO;
    }

    log(log_level::INFO, "Attached the XDP program in " +
                             std::string(mode_ == XDP_MODE_NATIVE ? "native" : "skb") +
                             " mode, udp4_dp_filter fd " +
                             std::to_string(udp4_dp_filter_fd_));
    return 0;
  }

  void detach() override {
    if (prog_ == nullptr) return;

    xdp_program__detach(prog_, ifindex_, mode_, 0);
    xdp_program__close(prog_);
    remove_dispatcher(ifindex_);

    prog_ = nullptr;
    xsks_map_fd_ = -1;
    udp4_dp_filter_fd_ = -1;
    refcnt_.clear();

    log(log_level::INFO, "Detached the XDP program.");
  }

  int xsks_map_fd() const override {
    return xsks_map_fd_;
  }

  int set_udp_dp_filter(uint16_t dst_port, bool present) override {
    if (prog_ == nullptr) return -ENOTSUP;
    if (udp4_dp_filter_fd_ < 0) return -ENOTSUP;

    /* Several sessions can ask for the same port. Only the first add and the
     * last delete touch the map. */
    int& refcnt = refcnt_[dst_port];
    if (!present && refcnt == 0) {
      log(log_level::WARNING,
          "Asked to remove UDP port " + std::to_string(dst_port) + ", which is not set.");
      refcnt_.erase(dst_port);
      return -EINVAL;
    }

    refcnt += present ? 1 : -1;
    if (present && refcnt > 1) return 0;
    if (!present && refcnt > 0) return 0;

    uint8_t value = present ? 1 : 0;
    int ret = bpf_map_update_elem(udp4_dp_filter_fd_, &dst_port, &value, BPF_ANY);
    if (ret < 0) {
      refcnt -= present ? 1 : -1; /* the map did not change, so neither may the count */
      log(log_level::ERROR, "Failed to update udp4_dp_filter for port " +
                                std::to_string(dst_port) + ", error " +
                                std::to_string(ret));
      return ret;
    }

    if (!present) refcnt_.erase(dst_port); /* do not grow the map for ever */

    log(log_level::INFO, std::string(present ? "Added " : "Removed ") +
                             std::to_string(dst_port) + " in udp4_dp_filter");
    return 0;
  }

 private:
  static void log(log_level level, const std::string& message) {
    logger::log(level, "[XDP] " + message);
  }

  /*
   * Take the libxdp dispatcher off the interface.
   *
   * Two programs go on: mtl.xdp.o, and the AF_XDP redirect program that
   * xsk_setup_xdp_prog() installs to make the xsks map. libxdp holds both inside
   * a dispatcher of its own. xdp_program__detach() takes the first one out, and
   * there is no call that takes the second one out without the AF_XDP socket
   * that the manager does not own, so the dispatcher stayed attached for ever:
   * `ip link` kept reporting "xdp" and the next program could not take the
   * interface.
   *
   * The manager caused every program in that dispatcher, and an interface it
   * holds is an interface MTL uses alone, so it takes the whole dispatcher down.
   */
  static void remove_dispatcher(unsigned int ifindex) {
    struct xdp_multiprog* mp = xdp_multiprog__get_from_ifindex((int)ifindex);

    if (mp == nullptr || libxdp_get_error(mp) != 0) return;

    if (xdp_multiprog__detach(mp) < 0)
      log(log_level::WARNING,
          "Failed to take the XDP dispatcher off the interface. "
          "Use xdp-loader unload to clear it.");

    xdp_multiprog__close(mp);
  }

  struct xdp_program* prog_ = nullptr;
  enum xdp_attach_mode mode_ = XDP_MODE_UNSPEC;
  unsigned int ifindex_ = 0;
  int xsks_map_fd_ = -1;
  int udp4_dp_filter_fd_ = -1;
  std::unordered_map<uint16_t, int> refcnt_;
};

#else /* MTL_HAS_XDP_BACKEND */

class xdp_absent : public mtlm_xdp_ops {
 public:
  int attach(unsigned int ifindex) override {
    (void)ifindex;
    return -ENOTSUP;
  }
  void detach() override {
  }
  int xsks_map_fd() const override {
    return -1;
  }
  int set_udp_dp_filter(uint16_t dst_port, bool present) override {
    (void)dst_port;
    (void)present;
    return -ENOTSUP;
  }
};

#endif /* MTL_HAS_XDP_BACKEND */

} /* namespace */

bool mtlm_xdp_supported() {
#ifdef MTL_HAS_XDP_BACKEND
  return true;
#else
  return false;
#endif
}

std::unique_ptr<mtlm_xdp_ops> mtlm_xdp_create() {
#ifdef MTL_HAS_XDP_BACKEND
  return std::unique_ptr<mtlm_xdp_ops>(new xdp_libxdp());
#else
  return std::unique_ptr<mtlm_xdp_ops>(new xdp_absent());
#endif
}
