/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

/*
 * Helpers that build and read one protocol record, for the tests that drive
 * mtl_instance over a socket pair.
 */

#ifndef _MTLM_TEST_WIRE_HPP_
#define _MTLM_TEST_WIRE_HPP_

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <utility>

#include "mtl_mproto.h"

/** One request record with the header filled in. */
inline mtl_message_t wire_request(mtl_message_type_t type, uint32_t body_len) {
  mtl_message_t msg;

  std::memset(&msg, 0, sizeof(msg));
  msg.header.magic = htonl(MTL_MANAGER_MAGIC);
  msg.header.type = htonl(static_cast<uint32_t>(type));
  msg.header.body_len = htonl(body_len);
  return msg;
}

/** A heartbeat request that carries `seq`. */
inline mtl_message_t wire_heartbeat(uint32_t seq) {
  mtl_message_t msg =
      wire_request(MTL_MSG_TYPE_HEARTBEAT, sizeof(mtl_heartbeat_message_t));

  msg.body.heartbeat_msg.seq = htonl(seq);
  return msg;
}

/** A GET_LCORE or PUT_LCORE request for `lcore_id`. */
inline mtl_message_t wire_lcore(mtl_message_type_t type, uint16_t lcore_id) {
  mtl_message_t msg = wire_request(type, sizeof(mtl_lcore_message_t));

  msg.body.lcore_msg.lcore = htons(lcore_id);
  return msg;
}

/** Type of a record the manager sent. */
inline uint32_t wire_type(const mtl_message_t& msg) {
  return ntohl(msg.header.type);
}

/** Response field of a record the manager sent. */
inline int wire_response(const mtl_message_t& msg) {
  return static_cast<int>(ntohl(static_cast<uint32_t>(msg.body.response_msg.response)));
}

/**
 * Read one whole record from `fd`.
 *
 * @return 0 on success, else a negative errno. The tests set a receive timeout
 *         on the socket, so a missing answer fails the test instead of hanging.
 */
inline int wire_read(int fd, mtl_message_t& msg) {
  char* p = reinterpret_cast<char*>(&msg);
  size_t done = 0;

  std::memset(&msg, 0, sizeof(msg));
  while (done < MTL_MANAGER_MSG_SIZE) {
    ssize_t ret = recv(fd, p + done, MTL_MANAGER_MSG_SIZE - done, 0);
    if (ret > 0) {
      done += static_cast<size_t>(ret);
      continue;
    }
    if (ret == 0) return -ECONNRESET;
    if (errno == EINTR) continue;
    return -errno;
  }

  return 0;
}

/** Tell the kernel to fail a read after `seconds` instead of blocking. */
inline void wire_set_timeout(int fd, int seconds) {
  struct timeval tv = {};

  tv.tv_sec = seconds;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

/** Number of open descriptors of this process, or a negative errno. */
inline int open_fd_count() {
  DIR* dir = opendir("/proc/self/fd");
  int count = 0;

  if (dir == nullptr) return -errno;
  while (readdir(dir) != nullptr) count++;
  closedir(dir);
  return count;
}

/** An AF_UNIX address for `path`, zeroed past its end. */
inline struct sockaddr_un unix_addr(const std::string& path) {
  struct sockaddr_un addr = {};

  addr.sun_family = AF_UNIX;
  std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());
  return addr;
}

/** A descriptor of /dev/null to pass over SCM_RIGHTS, or a negative errno. */
inline int null_fd() {
  int fd = open("/dev/null", O_RDONLY | O_CLOEXEC);

  return fd < 0 ? -errno : fd;
}

/**
 * A thread that runs one blocking client call.
 *
 * A failed ASSERT returns before join(), and a joinable std::thread then ends
 * the whole binary. The destructor instead shuts `peer_fd` down, which makes
 * the blocked call return, and joins.
 */
class wire_worker {
 public:
  template <typename Fn>
  wire_worker(int peer_fd, Fn&& fn) : peer_fd(peer_fd), worker(std::forward<Fn>(fn)) {
  }

  ~wire_worker() {
    if (!worker.joinable()) return;
    shutdown(peer_fd, SHUT_RDWR);
    worker.join();
  }

  wire_worker(const wire_worker&) = delete;
  wire_worker& operator=(const wire_worker&) = delete;

  void join() {
    worker.join();
  }

 private:
  int peer_fd;
  std::thread worker;
};

#endif
