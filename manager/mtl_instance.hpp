/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#ifndef __MTL_INSTANCE_HPP__
#define __MTL_INSTANCE_HPP__

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "logging.hpp"
#include "mtl_interface.hpp"
#include "mtl_lcore.hpp"
#include "mtl_mproto.h"

/**
 * One connected MTL instance.
 *
 * It tracks every resource the instance holds, and gives all of them back in
 * the destructor, so a client that dies leaves nothing behind.
 */
class mtl_instance {
 public:
  /**
   * @param conn_fd   Connected socket. The instance owns it and closes it.
   * @param registry  Shared interfaces. Must outlive the instance.
   * @param lcores    Shared lcore map. Must outlive the instance.
   */
  mtl_instance(int conn_fd, mtl_interface_registry& registry, mtl_lcore& lcores);
  ~mtl_instance();

  mtl_instance(const mtl_instance&) = delete;
  mtl_instance& operator=(const mtl_instance&) = delete;

  /**
   * Add bytes the socket delivered and handle every whole record in them.
   *
   * A stream socket may split one record or join several, so the leftover
   * stays here until the rest arrives.
   *
   * @return The number of records handled, or -EBADMSG when the stream no
   *         longer starts on a record boundary. The caller must drop the
   *         connection on -EBADMSG, because a desynced stream cannot recover.
   */
  int feed(const char* buf, size_t len);

  int get_conn_fd() const {
    return conn_fd;
  }
  int get_pid() const {
    return pid;
  }
  int get_uid() const {
    return uid;
  }
  const std::string& get_hostname() const {
    return hostname;
  }
  bool registered() const {
    return is_registered;
  }

  /** Number of lcores this instance holds. */
  size_t lcore_count() const {
    return lcore_ids.size();
  }

  /** Number of queues this instance holds on `ifindex`. */
  size_t queue_count(unsigned int ifindex) const;

  /** Number of flow rules this instance holds on `ifindex`. */
  size_t flow_count(unsigned int ifindex) const;

 private:
  void log(const log_level& level, const std::string& message) const;

  /** Handle exactly one whole record. */
  void dispatch(const mtl_message_t* msg);

  void handle_message_register(const mtl_register_message_t* register_msg);
  void handle_message_heartbeat(const mtl_heartbeat_message_t* heartbeat_msg);
  void handle_message_get_lcore(const mtl_lcore_message_t* lcore_msg);
  void handle_message_put_lcore(const mtl_lcore_message_t* lcore_msg);
  void handle_message_if_xsk_map_fd(const mtl_if_message_t* if_msg);
  void handle_message_udp_dp_filter(const mtl_udp_dp_filter_message_t* msg, bool add);
  void handle_message_if_get_queue(const mtl_if_message_t* if_msg);
  void handle_message_if_put_queue(const mtl_if_message_t* if_msg);
  void handle_message_if_add_flow(const mtl_if_message_t* if_msg);
  void handle_message_if_del_flow(const mtl_if_message_t* if_msg);

  /**
   * Send a response record.
   *
   * @param response Value for the response field: 0 or a resource id on
   *                 success, a negative errno on failure.
   */
  int send_response(int response, mtl_message_type_t type = MTL_MSG_TYPE_RESPONSE);

  /**
   * Refuse an operation from a client that never registered.
   *
   * @return true when the caller may go on.
   */
  bool require_registered(const char* what, mtl_message_type_t response_type);

  /** Interface for `ifindex`, or nullptr after it reports the failure. */
  std::shared_ptr<mtl_interface> get_interface(unsigned int ifindex,
                                               bool require_xdp = false);

  const int conn_fd;
  mtl_interface_registry& registry;
  mtl_lcore& lcores;
  bool is_registered;
  int pid;
  int uid;
  std::string hostname;
  std::vector<char> rx_buf;
  std::unordered_set<uint16_t> lcore_ids;
  std::unordered_map<unsigned int, std::shared_ptr<mtl_interface>> interfaces;
  std::unordered_map<unsigned int, std::unordered_set<uint16_t>> if_queue_ids;
  std::unordered_map<unsigned int, std::unordered_set<uint32_t>> if_flow_ids;
};

#endif
