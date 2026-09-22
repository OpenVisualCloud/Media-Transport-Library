/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#include "mtl_instance.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

mtl_instance::mtl_instance(int conn_fd, mtl_interface_registry& registry,
                           mtl_lcore& lcores)
    : conn_fd(conn_fd),
      registry(registry),
      lcores(lcores),
      is_registered(false),
      pid(-1),
      uid(-1),
      hostname("unknown") {
}

mtl_instance::~mtl_instance() {
  log(log_level::INFO, "Remove client.");

  for (const auto& lcore_id : lcore_ids) lcores.put_lcore(lcore_id);

  for (auto& pair : if_queue_ids) {
    auto it = interfaces.find(pair.first);
    if (it == interfaces.end() || it->second == nullptr) continue;
    for (uint16_t id : pair.second) it->second->put_queue(id);
    pair.second.clear();
  }

  for (auto& pair : if_flow_ids) {
    auto it = interfaces.find(pair.first);
    if (it == interfaces.end() || it->second == nullptr) continue;
    for (uint32_t id : pair.second) it->second->del_flow(id);
    pair.second.clear();
  }

  if (conn_fd >= 0) close(conn_fd);
}

void mtl_instance::log(const log_level& level, const std::string& message) const {
  logger::log(level,
              "[Instance " + hostname + ":" + std::to_string(pid) + "] " + message);
}

size_t mtl_instance::queue_count(unsigned int ifindex) const {
  auto it = if_queue_ids.find(ifindex);
  return it == if_queue_ids.end() ? 0 : it->second.size();
}

size_t mtl_instance::flow_count(unsigned int ifindex) const {
  auto it = if_flow_ids.find(ifindex);
  return it == if_flow_ids.end() ? 0 : it->second.size();
}

int mtl_instance::feed(const char* buf, size_t len) {
  int handled = 0;

  if (buf != nullptr && len > 0) rx_buf.insert(rx_buf.end(), buf, buf + len);

  while (rx_buf.size() >= MTL_MANAGER_MSG_SIZE) {
    mtl_message_t msg;

    /* Copy out of the buffer: the vector data has no alignment guarantee that
     * reading the struct in place would need. */
    std::memcpy(&msg, rx_buf.data(), sizeof(msg));
    rx_buf.erase(rx_buf.begin(), rx_buf.begin() + MTL_MANAGER_MSG_SIZE);

    if (ntohl(msg.header.magic) != MTL_MANAGER_MAGIC) {
      log(log_level::ERROR, "Invalid magic, dropping the connection.");
      rx_buf.clear();
      return -EBADMSG;
    }

    dispatch(&msg);
    handled++;
  }

  return handled;
}

void mtl_instance::dispatch(const mtl_message_t* msg) {
  switch (ntohl(static_cast<uint32_t>(msg->header.type))) {
    case MTL_MSG_TYPE_REGISTER:
      handle_message_register(&msg->body.register_msg);
      break;
    case MTL_MSG_TYPE_HEARTBEAT:
      handle_message_heartbeat(&msg->body.heartbeat_msg);
      break;
    case MTL_MSG_TYPE_GET_LCORE:
      handle_message_get_lcore(&msg->body.lcore_msg);
      break;
    case MTL_MSG_TYPE_PUT_LCORE:
      handle_message_put_lcore(&msg->body.lcore_msg);
      break;
    case MTL_MSG_TYPE_IF_XSK_MAP_FD:
      handle_message_if_xsk_map_fd(&msg->body.if_msg);
      break;
    case MTL_MSG_TYPE_ADD_UDP_DP_FILTER:
      handle_message_udp_dp_filter(&msg->body.udp_dp_filter_msg, true);
      break;
    case MTL_MSG_TYPE_DEL_UDP_DP_FILTER:
      handle_message_udp_dp_filter(&msg->body.udp_dp_filter_msg, false);
      break;
    case MTL_MSG_TYPE_IF_GET_QUEUE:
      handle_message_if_get_queue(&msg->body.if_msg);
      break;
    case MTL_MSG_TYPE_IF_PUT_QUEUE:
      handle_message_if_put_queue(&msg->body.if_msg);
      break;
    case MTL_MSG_TYPE_IF_ADD_FLOW:
      handle_message_if_add_flow(&msg->body.if_msg);
      break;
    case MTL_MSG_TYPE_IF_DEL_FLOW:
      handle_message_if_del_flow(&msg->body.if_msg);
      break;
    default:
      log(log_level::ERROR, "Unknown message type");
      /* Answer anyway. A client that waits for a response would otherwise
       * block until its own timeout. */
      send_response(-ENOTSUP);
      break;
  }
}

int mtl_instance::send_response(int response, mtl_message_type_t type) {
  mtl_message_t msg;

  /* Zero the whole record. Sending only the header and the response field
   * would leak the rest of this stack frame to the client. */
  std::memset(&msg, 0, sizeof(msg));
  msg.header.magic = htonl(MTL_MANAGER_MAGIC);
  msg.header.type = static_cast<mtl_message_type_t>(htonl(static_cast<uint32_t>(type)));
  msg.header.body_len = htonl(sizeof(mtl_response_message_t));
  msg.body.response_msg.response =
      static_cast<int>(htonl(static_cast<uint32_t>(response)));

  size_t done = 0;
  while (done < sizeof(msg)) {
    ssize_t ret = send(conn_fd, reinterpret_cast<const char*>(&msg) + done,
                       sizeof(msg) - done, MSG_NOSIGNAL);
    if (ret > 0) {
      done += static_cast<size_t>(ret);
      continue;
    }
    if (ret < 0 && errno == EINTR) continue;
    return ret < 0 ? -errno : -EPIPE;
  }

  return 0;
}

bool mtl_instance::require_registered(const char* what,
                                      mtl_message_type_t response_type) {
  if (is_registered) return true;

  log(log_level::WARNING,
      "Refused " + std::string(what) + " from an instance that never registered.");
  if (send_response(-EPERM, response_type) < 0)
    log(log_level::ERROR, "Failed to send the response for " + std::string(what));

  return false;
}

std::shared_ptr<mtl_interface> mtl_instance::get_interface(unsigned int ifindex,
                                                           bool require_xdp) {
  auto it = interfaces.find(ifindex);
  if (it != interfaces.end()) return it->second;

  auto interface = registry.get(ifindex, require_xdp);
  if (interface == nullptr) {
    log(log_level::ERROR, "Could not get interface " + std::to_string(ifindex));
    return nullptr;
  }

  interfaces[ifindex] = interface;
  return interface;
}

void mtl_instance::handle_message_register(const mtl_register_message_t* register_msg) {
  uint16_t num_if = ntohs(register_msg->num_if);

  /* The record has room for MTL_MANAGER_MAX_IF indexes. A larger count used to
   * read past the end of the record. */
  if (num_if > MTL_MANAGER_MAX_IF) {
    log(log_level::ERROR, "Register names " + std::to_string(num_if) +
                              " interfaces, the limit is " +
                              std::to_string(MTL_MANAGER_MAX_IF));
    if (send_response(-EINVAL) < 0)
      log(log_level::ERROR, "Failed to send the response for register");
    return;
  }

  pid = static_cast<int>(ntohl(static_cast<uint32_t>(register_msg->pid)));
  uid = static_cast<int>(ntohl(static_cast<uint32_t>(register_msg->uid)));

  /* The field is a fixed 64 bytes of padded text, so stop at the first zero.
   * Taking all 64 put the padding, zero bytes included, into every log line. */
  size_t host_len = strnlen(register_msg->hostname, sizeof(register_msg->hostname));
  hostname.assign(register_msg->hostname, host_len);
  if (hostname.empty()) hostname = "unknown";

  /* These are the AF_XDP ports of the instance, so the XDP program must load
   * for each of them or the instance cannot receive anything. */
  for (uint16_t i = 0; i < num_if; i++) {
    unsigned int ifindex = ntohl(register_msg->ifindex[i]);
    if (get_interface(ifindex, true) == nullptr) {
      if (send_response(-ENODEV) < 0)
        log(log_level::ERROR, "Failed to send the response for register");
      return;
    }
  }

  is_registered = true;
  log(log_level::INFO, "Registered, uid " + std::to_string(uid) + ", " +
                           std::to_string(num_if) + " interface(s).");

  if (send_response(0) < 0)
    log(log_level::ERROR, "Failed to send the response for register");
}

void mtl_instance::handle_message_heartbeat(
    const mtl_heartbeat_message_t* heartbeat_msg) {
  mtl_message_t msg;

  std::memset(&msg, 0, sizeof(msg));
  msg.header.magic = htonl(MTL_MANAGER_MAGIC);
  msg.header.type = static_cast<mtl_message_type_t>(
      htonl(static_cast<uint32_t>(MTL_MSG_TYPE_HEARTBEAT_ACK)));
  msg.header.body_len = htonl(sizeof(mtl_heartbeat_message_t));
  /* Echo the sequence number back exactly as it arrived. */
  msg.body.heartbeat_msg.seq = heartbeat_msg->seq;

  size_t done = 0;
  while (done < sizeof(msg)) {
    ssize_t ret = send(conn_fd, reinterpret_cast<const char*>(&msg) + done,
                       sizeof(msg) - done, MSG_NOSIGNAL);
    if (ret > 0) {
      done += static_cast<size_t>(ret);
      continue;
    }
    if (ret < 0 && errno == EINTR) continue;
    log(log_level::ERROR, "Failed to send the heartbeat acknowledgement");
    return;
  }
}

void mtl_instance::handle_message_get_lcore(const mtl_lcore_message_t* lcore_msg) {
  if (!require_registered("get_lcore", MTL_MSG_TYPE_RESPONSE)) return;

  uint16_t lcore_id = ntohs(lcore_msg->lcore);
  int ret = lcores.get_lcore(lcore_id);
  if (ret == 0) {
    lcore_ids.insert(lcore_id);
    log(log_level::INFO, "Added lcore " + std::to_string(lcore_id));
  }

  if (send_response(ret) < 0)
    log(log_level::ERROR, "Failed to send the response for get_lcore");
}

void mtl_instance::handle_message_put_lcore(const mtl_lcore_message_t* lcore_msg) {
  if (!require_registered("put_lcore", MTL_MSG_TYPE_RESPONSE)) return;

  uint16_t lcore_id = ntohs(lcore_msg->lcore);

  /* Only give back an lcore this instance holds. Without the check one client
   * could release the lcore of another. */
  if (lcore_ids.find(lcore_id) == lcore_ids.end()) {
    log(log_level::WARNING,
        "Asked to put lcore " + std::to_string(lcore_id) + ", which it does not hold.");
    if (send_response(-EINVAL) < 0)
      log(log_level::ERROR, "Failed to send the response for put_lcore");
    return;
  }

  int ret = lcores.put_lcore(lcore_id);
  if (ret == 0) {
    lcore_ids.erase(lcore_id);
    log(log_level::INFO, "Removed lcore " + std::to_string(lcore_id));
  }

  if (send_response(ret) < 0)
    log(log_level::ERROR, "Failed to send the response for put_lcore");
}

void mtl_instance::handle_message_if_xsk_map_fd(const mtl_if_message_t* if_msg) {
  unsigned int ifindex = ntohl(if_msg->ifindex);
  int fd = -1;

  auto interface = get_interface(ifindex, true);
  if (interface != nullptr) fd = interface->get_xsks_map_fd();

  struct msghdr msg = {};
  struct iovec iov[1];
  char control[CMSG_SPACE(sizeof(int))] = {0};
  char data[1] = {' '};

  iov[0].iov_base = data;
  iov[0].iov_len = sizeof(data);
  msg.msg_iov = iov;
  msg.msg_iovlen = 1;

  if (fd >= 0) {
    msg.msg_control = control;
    msg.msg_controllen = CMSG_SPACE(sizeof(int));

    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    std::memcpy(CMSG_DATA(cmsg), &fd, sizeof(fd));
  } else {
    /* SCM_RIGHTS cannot carry -1: sendmsg would fail with EBADF and the client
     * would wait for a message that never comes. Send the one data byte on its
     * own instead, and let the client read the missing control data as the
     * error. */
    data[0] = 'E';
    log(log_level::WARNING, "No xsks map for interface " + std::to_string(ifindex));
  }

  if (sendmsg(conn_fd, &msg, MSG_NOSIGNAL) < 0)
    log(log_level::ERROR, "Failed to send the xsks map descriptor");
}

void mtl_instance::handle_message_udp_dp_filter(
    const mtl_udp_dp_filter_message_t* udp_dp_filter_msg, bool add) {
  if (!require_registered("udp_dp_filter", MTL_MSG_TYPE_RESPONSE)) return;

  unsigned int ifindex = ntohl(udp_dp_filter_msg->ifindex);
  uint16_t port = ntohs(udp_dp_filter_msg->port);

  auto interface = get_interface(ifindex, true);
  int ret = interface == nullptr ? -ENODEV : interface->update_udp_dp_filter(port, add);

  if (send_response(ret) < 0)
    log(log_level::ERROR, "Failed to send the response for udp_dp_filter");
}

void mtl_instance::handle_message_if_get_queue(const mtl_if_message_t* if_msg) {
  if (!require_registered("if_get_queue", MTL_MSG_TYPE_IF_QUEUE_ID)) return;

  unsigned int ifindex = ntohl(if_msg->ifindex);
  auto interface = get_interface(ifindex);
  int ret = interface == nullptr ? -ENODEV : interface->get_queue();

  if (ret > 0) if_queue_ids[ifindex].insert(static_cast<uint16_t>(ret));

  if (send_response(ret, MTL_MSG_TYPE_IF_QUEUE_ID) < 0)
    log(log_level::ERROR, "Failed to send the response for if_get_queue");
}

void mtl_instance::handle_message_if_put_queue(const mtl_if_message_t* if_msg) {
  if (!require_registered("if_put_queue", MTL_MSG_TYPE_RESPONSE)) return;

  unsigned int ifindex = ntohl(if_msg->ifindex);
  uint16_t queue_id = ntohs(if_msg->queue_id);

  /* Only release a queue this instance reserved. */
  auto tracked = if_queue_ids.find(ifindex);
  if (tracked == if_queue_ids.end() ||
      tracked->second.find(queue_id) == tracked->second.end()) {
    log(log_level::WARNING, "Asked to put queue " + std::to_string(queue_id) +
                                " on interface " + std::to_string(ifindex) +
                                ", which it does not hold.");
    if (send_response(-EINVAL) < 0)
      log(log_level::ERROR, "Failed to send the response for if_put_queue");
    return;
  }

  auto interface = get_interface(ifindex);
  int ret = interface == nullptr ? -ENODEV : interface->put_queue(queue_id);
  if (ret == 0) tracked->second.erase(queue_id);

  if (send_response(ret) < 0)
    log(log_level::ERROR, "Failed to send the response for if_put_queue");
}

void mtl_instance::handle_message_if_add_flow(const mtl_if_message_t* if_msg) {
  if (!require_registered("if_add_flow", MTL_MSG_TYPE_IF_FLOW_ID)) return;

  unsigned int ifindex = ntohl(if_msg->ifindex);
  auto interface = get_interface(ifindex);
  int ret = -ENODEV;

  if (interface != nullptr)
    ret = interface->add_flow(ntohs(if_msg->queue_id), ntohl(if_msg->flow_type),
                              if_msg->src_ip, if_msg->dst_ip, ntohs(if_msg->src_port),
                              ntohs(if_msg->dst_port));

  if (ret > 0) if_flow_ids[ifindex].insert(static_cast<uint32_t>(ret));

  if (send_response(ret, MTL_MSG_TYPE_IF_FLOW_ID) < 0)
    log(log_level::ERROR, "Failed to send the response for if_add_flow");
}

void mtl_instance::handle_message_if_del_flow(const mtl_if_message_t* if_msg) {
  if (!require_registered("if_del_flow", MTL_MSG_TYPE_RESPONSE)) return;

  unsigned int ifindex = ntohl(if_msg->ifindex);
  uint32_t flow_id = ntohl(if_msg->flow_id);

  /* Only delete a rule this instance installed. */
  auto tracked = if_flow_ids.find(ifindex);
  if (tracked == if_flow_ids.end() ||
      tracked->second.find(flow_id) == tracked->second.end()) {
    log(log_level::WARNING, "Asked to delete flow " + std::to_string(flow_id) +
                                " on interface " + std::to_string(ifindex) +
                                ", which it does not hold.");
    if (send_response(-EINVAL) < 0)
      log(log_level::ERROR, "Failed to send the response for if_del_flow");
    return;
  }

  auto interface = get_interface(ifindex);
  int ret = interface == nullptr ? -ENODEV : interface->del_flow(flow_id);
  if (ret == 0) tracked->second.erase(flow_id);

  if (send_response(ret) < 0)
    log(log_level::ERROR, "Failed to send the response for if_del_flow");
}
