/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#ifndef _MTLM_SERVER_HPP_
#define _MTLM_SERVER_HPP_

#include <memory>
#include <string>
#include <vector>

#include "mtl_instance.hpp"
#include "mtl_interface.hpp"
#include "mtl_lcore.hpp"
#include "mtlm_netdev.hpp"

/** How the server binds and who may reach it. */
struct mtlm_server_config {
  /** Path to bind. Empty asks mtlm_sock_path() to resolve one. */
  std::string sock_path;

  /**
   * Mode of the socket file.
   *
   * A client needs write permission to connect. 0 asks for a default: 0666 for
   * the system path, which any user can reach, and 0600 for a per-user path,
   * which is already inside a private directory.
   */
  unsigned int socket_mode = 0;

  /**
   * Group to own the socket. Empty leaves the group alone.
   *
   * Naming a group and a mode of 0660 is how to let a chosen set of users
   * reach a system manager without opening it to everyone.
   */
  std::string socket_group;

  /** Largest number of instances that may be connected at once. */
  size_t max_clients = 64;
};

/**
 * The manager server: one listening AF_UNIX socket and the connected instances.
 */
class mtlm_server {
 public:
  explicit mtlm_server(const mtlm_server_config& config);
  ~mtlm_server();

  mtlm_server(const mtlm_server&) = delete;
  mtlm_server& operator=(const mtlm_server&) = delete;

  /**
   * Resolve the path, bind, listen, and arm the signal and epoll descriptors.
   *
   * @return 0 on success, else a negative errno. It refuses to start when
   *         another manager already answers on the path.
   */
  int setup();

  /**
   * Serve until SIGINT or SIGTERM arrives, or stop() is called.
   *
   * @return 0 on a clean shutdown, else a negative errno.
   */
  int run();

  /**
   * Ask run() to return.
   *
   * It writes to an eventfd the loop watches, so it wakes a run() that sits in
   * epoll_wait, and it may be called from another thread.
   */
  void stop();

  /** Path the server bound, valid after setup(). */
  const std::string& sock_path() const {
    return path;
  }

  /** Number of connected instances. */
  size_t client_count() const {
    return clients.size();
  }

 private:
  int bind_socket();
  int apply_socket_permissions();
  int arm_signals();
  int accept_client();
  /** Read from a client and hand the bytes to it. Returns false to drop it. */
  bool service_client(int fd);
  void drop_client(int fd);
  void close_all();

  mtlm_server_config config;
  std::string path;
  bool path_is_system = false;
  int listen_fd = -1;
  int epoll_fd = -1;
  int signal_fd = -1;
  int stop_fd = -1;
  bool bound = false;
  volatile bool running = false;

  std::shared_ptr<mtlm_netdev_ops> netdev;
  std::unique_ptr<mtl_interface_registry> registry;
  std::vector<std::unique_ptr<mtl_instance>> clients;
};

#endif
