/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#include <arpa/inet.h>
#include <unistd.h>

#include <chrono>
#include <memory>
#include <vector>

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
  std::vector<int> lcore_ids;
  std::shared_ptr<mtl_lcore> lcore_manager_sp;

 private:
  int handle_message_get_lcore(mtl_lcore_message_t* lcore_msg);
  int handle_message_put_lcore(mtl_lcore_message_t* lcore_msg);
  int handle_message_register(mtl_register_message_t* register_msg);
  int send_response(bool success) {
    mtl_message_t msg;
    msg.header.magic = htonl(IMTL_MAGIC);
    msg.header.type = (mtl_message_type_t)htonl(MTL_MSG_TYPE_RESPONSE);
    msg.header.body_len = htonl(sizeof(mtl_response_message_t));
    msg.body.response_msg.response = success ? 0 : 1;
    return send(conn_fd, &msg, sizeof(mtl_message_t), 0);
  }

 public:
  mtl_instance(int conn_fd, std::shared_ptr<mtl_lcore> lcore_manager)
      : conn_fd(conn_fd), lcore_manager_sp(lcore_manager) {}
  ~mtl_instance() {
    for (const auto& lcore_id : lcore_ids) lcore_manager_sp->put_lcore(lcore_id);
    ifindex.clear();
    lcore_ids.clear();

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
  if (ntohl(msg->header.magic) != IMTL_MAGIC) {
    std::cout << "Invalid magic number" << std::endl;
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
    case MTL_MSG_TYPE_REQUEST_MAP_FD:
      std::cout << "Receive request map fd from client" << std::endl;
      break;
    case MTL_MSG_TYPE_UDP_PORT_OPERATION:
      std::cout << "Receive udp port operation from client" << std::endl;
      break;
    default:
      std::cout << "Unknown message type" << std::endl;
      break;
  }

  return 0;
}

int mtl_instance::handle_message_get_lcore(mtl_lcore_message_t* lcore_msg) {
  if (!is_registered) {
    std::cout << "Instance is not registered" << std::endl;
    return -1;
  }
  uint16_t lcore_id = ntohs(lcore_msg->lcore);
  int ret = lcore_manager_sp->get_lcore(lcore_id);
  if (ret < 0) {
    send_response(false);
    return -1;
  }
  lcore_ids.push_back(lcore_id);
  std::cout << "Add lcore " << lcore_id << " to instance " << hostname << ":" << pid
            << std::endl;
  send_response(true);
  return 0;
}

int mtl_instance::handle_message_put_lcore(mtl_lcore_message_t* lcore_msg) {
  if (!is_registered) {
    std::cout << "Instance is not registered" << std::endl;
    return -1;
  }
  uint16_t lcore_id = ntohs(lcore_msg->lcore);
  lcore_manager_sp->put_lcore(lcore_id);
  for (auto it = lcore_ids.begin(); it != lcore_ids.end(); it++) {
    if (*it == lcore_id) {
      lcore_ids.erase(it);
      break;
    }
  }
  std::cout << "Remove lcore " << lcore_id << " from instance " << hostname << ":" << pid
            << std::endl;
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

  std::cout << "Register message from " << hostname << std::endl;
  std::cout << "pid: " << pid << std::endl;
  std::cout << "uid: " << uid << std::endl;
  for (int i = 0; i < num_if; i++) {
    std::cout << "ifindex[" << i << "]: " << ifindex[i] << std::endl;
  }

  is_registered = true;
  send_response(true);

  return 0;
}
