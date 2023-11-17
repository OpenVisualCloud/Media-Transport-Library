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
#include <vector>

#include "logging.hpp"
#include "mtl_lcore.hpp"
#include "mtl_mproto.h"

class mtl_instance {
 private:
  int conn_fd;
  bool is_registered;
  int pid;
  int uid;
  std::string hostname;
  std::vector<int> ifindex;
  std::vector<uint16_t> lcore_ids;
  std::shared_ptr<mtl_lcore> lcore_manager_sp;

 private:
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

 public:
  mtl_instance(int conn_fd, std::shared_ptr<mtl_lcore> lcore_manager)
      : conn_fd(conn_fd), is_registered(false), lcore_manager_sp(lcore_manager) {}
  ~mtl_instance() {
    logger::log(log_level::INFO, "Remove client " + hostname + ":" + std::to_string(pid));
    for (const auto& lcore_id : lcore_ids) lcore_manager_sp->put_lcore(lcore_id);

    close(conn_fd);
  }

  int get_conn_fd() { return conn_fd; }
  int get_pid() { return pid; }
  int get_uid() { return uid; }
  std::string get_hostname() { return hostname; }
  std::vector<int> get_ifindex() { return ifindex; }
  int handle_message(const char* buf, int len);
};

int mtl_instance::handle_message(const char* buf, int len) {
  if ((size_t)len < sizeof(mtl_message_t)) return -1;
  mtl_message_t* msg = (mtl_message_t*)buf;
  if (ntohl(msg->header.magic) != MTL_MANAGER_MAGIC) {
    logger::log(log_level::INFO, "Invalid magic");
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
      logger::log(log_level::INFO, "Unknown message type");
      break;
  }

  return 0;
}

int mtl_instance::handle_message_get_lcore(mtl_lcore_message_t* lcore_msg) {
  if (!is_registered) {
    logger::log(log_level::INFO, "Instance is not registered");
    return -1;
  }
  uint16_t lcore_id = ntohs(lcore_msg->lcore);
  int ret = lcore_manager_sp->get_lcore(lcore_id);
  if (ret < 0) {
    send_response(false);
    return -1;
  }
  lcore_ids.push_back(lcore_id);
  logger::log(log_level::INFO, "Add lcore " + std::to_string(lcore_id) + " to instance " +
                                   hostname + ":" + std::to_string(pid));
  send_response(true);
  return 0;
}

int mtl_instance::handle_message_put_lcore(mtl_lcore_message_t* lcore_msg) {
  if (!is_registered) {
    logger::log(log_level::INFO, "Instance is not registered");
    return -1;
  }
  uint16_t lcore_id = ntohs(lcore_msg->lcore);
  lcore_manager_sp->put_lcore(lcore_id);
  auto it = std::find_if(lcore_ids.begin(), lcore_ids.end(),
                         [lcore_id](uint16_t id) { return id == lcore_id; });
  if (it != lcore_ids.end()) lcore_ids.erase(it);

  logger::log(log_level::INFO, "Remove lcore " + std::to_string(lcore_id) +
                                   " from instance " + hostname + ":" +
                                   std::to_string(pid));
  return 0;
}

int mtl_instance::handle_message_register(mtl_register_message_t* register_msg) {
  pid = ntohl(register_msg->pid);
  uid = ntohl(register_msg->uid);
  hostname = std::string(register_msg->hostname, 64);
  uint16_t num_if = ntohs(register_msg->num_if);
  for (int i = 0; i < num_if; i++) {
    ifindex.push_back(ntohs(register_msg->ifindex[i]));
  }

  logger::log(log_level::INFO,
              "Register instance " + hostname + ":" + std::to_string(pid));

  is_registered = true;
  send_response(true);

  return 0;
}

#endif