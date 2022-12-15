/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "ufd_main.h"

#include "../mt_log.h"

int mufd_socket(int domain, int type, int protocol) {
  return 0;
}

int mufd_close(int sockfd) {
  return 0;
}

int mufd_bind(int sockfd, const struct sockaddr* addr, socklen_t addrlen) {
  return 0;
}

ssize_t mufd_sendto(int sockfd, const void* buf, size_t len, int flags,
                    const struct sockaddr* dest_addr, socklen_t addrlen) {
  return 0;
}

int mufd_poll(struct pollfd* fds, nfds_t nfds, int timeout) {
  return 0;
}

ssize_t mufd_recvfrom(int sockfd, void* buf, size_t len, int flags,
                      struct sockaddr* src_addr, socklen_t* addrlen) {
  return 0;
}
