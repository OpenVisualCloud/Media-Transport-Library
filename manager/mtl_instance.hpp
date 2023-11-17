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
  int conn_fd;
  bool is_registered;
  int pid;
  int uid;
  std::string hostname;
  std::vector<uint16_t> lcore_ids;
  std::shared_ptr<mtl_lcore> lcore_manager_sp;
  std::unordered_map<unsigned int, std::shared_ptr<mtl_interface>> interfaces;

 private:
  void log(log_level level, const std::string& message) {
    logger::log(level,
                "[Instance " + hostname + ":" + std::to_string(pid) + "] " + message);
  }

  int handle_message_get_lcore(mtl_lcore_message_t* lcore_msg);
  int handle_message_put_lcore(mtl_lcore_message_t* lcore_msg);
  int handle_message_register(mtl_register_message_t* register_msg);
  int send_response(bool success) {
    mtl_message_t msg;
    msg.header.magic = htonl(MTL_MANAGER_MAGIC);
    msg.header.type = (mtl_message_type_t)htonl(MTL_MSG_TYPE_RESPONSE);
    msg.header.body_len = htonl(sizeof(mtl_response_message_t));
    msg.body.response_msg.response = success ? 0 : 1;
    return send(conn_fd, &msg, sizeof(mtl_message_t), 0);
  }
  std::shared_ptr<mtl_interface> get_interface(unsigned int ifindex);

 public:
  mtl_instance(int conn_fd, std::shared_ptr<mtl_lcore> lcore_manager)
      : conn_fd(conn_fd),
        is_registered(false),
        pid(-1),
        uid(-1),
        hostname("unknown"),
        lcore_manager_sp(lcore_manager) {}
  ~mtl_instance() {
    log(log_level::INFO, "Remove client.");
    for (const auto& lcore_id : lcore_ids) lcore_manager_sp->put_lcore(lcore_id);

    close(conn_fd);
  }

  int get_conn_fd() { return conn_fd; }
  int get_pid() { return pid; }
  int get_uid() { return uid; }
  std::string get_hostname() { return hostname; }
  int handle_message(const char* buf, int len);
};

int mtl_instance::handle_message(const char* buf, int len) {
  if ((size_t)len < sizeof(mtl_message_t)) return -1;
  mtl_message_t* msg = (mtl_message_t*)buf;
  if (ntohl(msg->header.magic) != MTL_MANAGER_MAGIC) {
    log(log_level::ERROR, "Invalid magic");
    return -1;
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
    default:
      log(log_level::ERROR, "Unknown message type");
      break;
  }

  return 0;
}

int mtl_instance::handle_message_get_lcore(mtl_lcore_message_t* lcore_msg) {
  if (!is_registered) {
    log(log_level::WARNING, "Instance is not registered");
    return -1;
  }
  uint16_t lcore_id = ntohs(lcore_msg->lcore);
  int ret = lcore_manager_sp->get_lcore(lcore_id);
  if (ret < 0) {
    send_response(false);
    return -1;
  }
  lcore_ids.push_back(lcore_id);
  log(log_level::INFO, "Add lcore " + std::to_string(lcore_id));
  send_response(true);
  return 0;
}

int mtl_instance::handle_message_put_lcore(mtl_lcore_message_t* lcore_msg) {
  if (!is_registered) {
    log(log_level::INFO, "Instance is not registered");
    return -1;
  }
  uint16_t lcore_id = ntohs(lcore_msg->lcore);
  lcore_manager_sp->put_lcore(lcore_id);
  auto it = std::find_if(lcore_ids.begin(), lcore_ids.end(),
                         [lcore_id](uint16_t id) { return id == lcore_id; });
  if (it != lcore_ids.end()) lcore_ids.erase(it);

  log(log_level::INFO, "Remove lcore " + std::to_string(lcore_id));
  return 0;
}

int mtl_instance::handle_message_register(mtl_register_message_t* register_msg) {
  pid = ntohl(register_msg->pid);
  uid = ntohl(register_msg->uid);
  hostname = std::string(register_msg->hostname, 64);
  uint16_t num_if = ntohs(register_msg->num_if);
  for (int i = 0; i < num_if; i++) {
    unsigned int ifindex = ntohl(register_msg->ifindex[i]);
    auto interface = get_interface(ifindex);
    if (interface == nullptr) {
      log(log_level::ERROR, "Could not get interface " + std::to_string(ifindex));
      send_response(false);
      return -1;
    }
  }

  log(log_level::INFO, "Registered.");

  is_registered = true;
  send_response(true);

  return 0;
}

std::shared_ptr<mtl_interface> mtl_instance::get_interface(unsigned int ifindex) {
  auto it = interfaces.find(ifindex);

  if (it != interfaces.end()) {
    log(log_level::INFO, "Returning existing interface.");
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

  // Interface does not exist, create and initialize it
  log(log_level::INFO, "Initializing a new interface " + std::to_string(ifindex));
  std::shared_ptr<mtl_interface> new_interface = std::make_shared<mtl_interface>(ifindex);
  g_interfaces[ifindex] = new_interface;
  interfaces[ifindex] = new_interface;
  return new_interface;
}

#endif