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
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

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

#endif
