/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#ifndef __MTL_INSTANCE_HPP__
#define __MTL_INSTANCE_HPP__

#include <arpa/inet.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <unordered_map>
#include <vector>

#include "logging.hpp"
#include "mtl_interface.hpp"
#include "mtl_lcore.hpp"
#include "mtl_mproto.h"

class mtl_instance {
 private:
  const int conn_fd;
  bool is_registered;
  int pid;
  int uid;
  std::string hostname;
  std::vector<uint16_t> lcore_ids;
  std::unordered_map<unsigned int, std::shared_ptr<mtl_interface>> interfaces;

 private:
  void log(const log_level& level, const std::string& message) const {
    logger::log(level,
                "[Instance " + hostname + ":" + std::to_string(pid) + "] " + message);
  }

  void handle_message_get_lcore(mtl_lcore_message_t* lcore_msg);
  void handle_message_put_lcore(mtl_lcore_message_t* lcore_msg);
  void handle_message_register(mtl_register_message_t* register_msg);
  void handle_message_if_xsk_map_fd(mtl_if_message_t* request_map_fd_msg);
  void handle_message_udp_dp_filter(mtl_udp_dp_filter_message_t* udp_dp_filter_msg,
                                    bool add);
  void handle_message_if_get_queue(mtl_if_message_t* queue_msg);
  void handle_message_if_put_queue(mtl_if_message_t* queue_msg);

  int send_response(int response, mtl_message_type_t type = MTL_MSG_TYPE_RESPONSE) {
    mtl_message_t msg;
    msg.header.magic = htonl(MTL_MANAGER_MAGIC);
    msg.header.type = (mtl_message_type_t)htonl(type);
    msg.header.body_len = htonl(sizeof(mtl_response_message_t));
    msg.body.response_msg.response = htonl(response);
    return send(conn_fd, &msg, sizeof(mtl_message_t), 0);
  }
  std::shared_ptr<mtl_interface> get_interface(const unsigned int ifindex);

 public:
  mtl_instance(int conn_fd)
      : conn_fd(conn_fd), is_registered(false), pid(-1), uid(-1), hostname("unknown") {}
  ~mtl_instance() {
    log(log_level::INFO, "Remove client.");
    for (const auto& lcore_id : lcore_ids) mtl_lcore::get_instance().put_lcore(lcore_id);

    close(conn_fd);
  }

  int get_conn_fd() const { return conn_fd; }
  int get_pid() const { return pid; }
  int get_uid() const { return uid; }
  std::string get_hostname() const { return hostname; }
  void handle_message(const char* buf, int len);
};

void mtl_instance::handle_message(const char* buf, int len) {
  if ((size_t)len < sizeof(mtl_message_t)) return;
  mtl_message_t* msg = (mtl_message_t*)buf;
  if (ntohl(msg->header.magic) != MTL_MANAGER_MAGIC) {
    log(log_level::ERROR, "Invalid magic");
    return;
  }

  switch (ntohl(msg->header.type)) {
    case MTL_MSG_TYPE_REGISTER:
      handle_message_register(&msg->body.register_msg);
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
    default:
      log(log_level::ERROR, "Unknown message type");
      break;
  }
}

void mtl_instance::handle_message_get_lcore(mtl_lcore_message_t* lcore_msg) {
  if (!is_registered) {
    log(log_level::WARNING, "Instance is not registered");
    return;
  }
  uint16_t lcore_id = ntohs(lcore_msg->lcore);
  int ret = mtl_lcore::get_instance().get_lcore(lcore_id);
  if (ret < 0) {
    send_response(ret);
    return;
  }
  lcore_ids.push_back(lcore_id);
  log(log_level::INFO, "Added lcore " + std::to_string(lcore_id));
  send_response(0);
}

void mtl_instance::handle_message_put_lcore(mtl_lcore_message_t* lcore_msg) {
  if (!is_registered) {
    log(log_level::INFO, "Instance is not registered");
    return;
  }
  uint16_t lcore_id = ntohs(lcore_msg->lcore);

  auto it = std::find_if(lcore_ids.begin(), lcore_ids.end(),
                         [lcore_id](uint16_t id) { return id == lcore_id; });
  if (it != lcore_ids.end()) lcore_ids.erase(it);

  int ret = mtl_lcore::get_instance().put_lcore(lcore_id);
  if (ret < 0) {
    send_response(ret);
    return;
  }
  log(log_level::INFO, "Removed lcore " + std::to_string(lcore_id));
  send_response(0);
}

void mtl_instance::handle_message_register(mtl_register_message_t* register_msg) {
  pid = ntohl(register_msg->pid);
  uid = ntohl(register_msg->uid);
  hostname = std::string(register_msg->hostname, 64);
  uint16_t num_if = ntohs(register_msg->num_if);
  for (int i = 0; i < num_if; i++) {
    unsigned int ifindex = ntohl(register_msg->ifindex[i]);
    auto interface = get_interface(ifindex);
    if (interface == nullptr) {
      log(log_level::ERROR, "Could not get interface " + std::to_string(ifindex));
      send_response(-1);
      return;
    }
  }

  log(log_level::INFO, "Registered.");

  is_registered = true;
  send_response(0);
}

std::shared_ptr<mtl_interface> mtl_instance::get_interface(const unsigned int ifindex) {
  auto it = interfaces.find(ifindex);
  if (it != interfaces.end()) {
    log(log_level::DEBUG, "Returning existing interface.");
    return it->second;
  }

  auto g_it = g_interfaces.find(ifindex);
  if (g_it != g_interfaces.end()) {
    if (auto g_interface = g_it->second.lock()) {
      log(log_level::INFO, "Acquiring global interface " + std::to_string(ifindex));
      interfaces[ifindex] = g_interface;
      return g_interface;
    }
  }

  /* Interface does not exist, create and initialize it */
  log(log_level::INFO, "Initializing a new interface " + std::to_string(ifindex));
  try {
    auto new_interface = std::make_shared<mtl_interface>(ifindex);
    g_interfaces[ifindex] = new_interface;
    interfaces[ifindex] = new_interface;
    return new_interface;
  } catch (const std::exception& e) {
    log(log_level::ERROR, "Failed to initialize interface: " + std::string(e.what()));
    return nullptr;
  }
}

void mtl_instance::handle_message_if_xsk_map_fd(mtl_if_message_t* if_msg) {
  int fd = -1;
  unsigned int ifindex = ntohl(if_msg->ifindex);
  auto interface = get_interface(ifindex);
  if (interface != nullptr) fd = interface->get_xsks_map_fd();

  struct msghdr msg = {0};
  struct iovec iov[1];
  char control[CMSG_SPACE(sizeof(int))] = {0};
  char data[1] = {' '};

  iov[0].iov_base = data;
  iov[0].iov_len = sizeof(data);

  msg.msg_iov = iov;
  msg.msg_iovlen = 1;
  msg.msg_controllen = CMSG_SPACE(sizeof(int));
  msg.msg_control = control;

  struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
  cmsg->cmsg_level = SOL_SOCKET;
  cmsg->cmsg_type = SCM_RIGHTS;
  cmsg->cmsg_len = CMSG_LEN(sizeof(int));
  *((int*)CMSG_DATA(cmsg)) = fd;

  sendmsg(conn_fd, &msg, 0);
}

void mtl_instance::handle_message_udp_dp_filter(
    mtl_udp_dp_filter_message_t* udp_dp_filter_msg, bool add) {
  unsigned int ifindex = ntohl(udp_dp_filter_msg->ifindex);
  uint16_t port = ntohs(udp_dp_filter_msg->port);
  auto interface = get_interface(ifindex);
  if (interface == nullptr) {
    log(log_level::ERROR, "Failed to get interface " + std::to_string(ifindex));
    send_response(-1);
    return;
  }
  int ret = interface->update_udp_dp_filter(port, add);
  send_response(ret);
}

void mtl_instance::handle_message_if_get_queue(mtl_if_message_t* if_msg) {
  unsigned int ifindex = ntohl(if_msg->ifindex);
  auto interface = get_interface(ifindex);
  if (interface == nullptr) {
    log(log_level::ERROR, "Failed to get interface " + std::to_string(ifindex));
    send_response(-1, MTL_MSG_TYPE_IF_QUEUE_ID);
    return;
  }
  int ret = interface->get_queue();
  send_response(ret, MTL_MSG_TYPE_IF_QUEUE_ID);
}

void mtl_instance::handle_message_if_put_queue(mtl_if_message_t* if_msg) {
  unsigned int ifindex = ntohl(if_msg->ifindex);
  auto interface = get_interface(ifindex);
  if (interface == nullptr) {
    log(log_level::ERROR, "Failed to get interface " + std::to_string(ifindex));
    send_response(-1);
    return;
  }
  int ret = interface->put_queue(ntohs(if_msg->queue_id));
  send_response(ret);
}

#endif