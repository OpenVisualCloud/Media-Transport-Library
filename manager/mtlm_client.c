/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

/*
 * Reference implementation of the client side of mtlm_api.h.
 *
 * Every call is one fixed-size request record and one fixed-size response
 * record. The send and the receive both loop, because a stream socket is free
 * to move fewer bytes than asked.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "mtlm_api.h"

struct mtlm_client {
  int fd;
  char sock_path[MTLM_SOCK_PATH_MAX];
};

static int send_all(int fd, const void* buf, size_t len) {
  const char* p = (const char*)buf;
  size_t done = 0;

  while (done < len) {
    ssize_t ret = send(fd, p + done, len - done, MSG_NOSIGNAL);
    if (ret > 0) {
      done += (size_t)ret;
      continue;
    }
    if (ret < 0 && errno == EINTR) continue;
    /* send() returning 0 on a stream socket with a non-zero length means the
     * peer will take nothing more. Report it as a closed connection. */
    return ret < 0 ? -errno : -EPIPE;
  }

  return 0;
}

static int recv_all(int fd, void* buf, size_t len) {
  char* p = (char*)buf;
  size_t done = 0;

  while (done < len) {
    ssize_t ret = recv(fd, p + done, len - done, 0);
    if (ret > 0) {
      done += (size_t)ret;
      continue;
    }
    if (ret == 0) return -ECONNRESET; /* manager closed mid record */
    if (errno == EINTR) continue;
    return -errno;
  }

  return 0;
}

/* Fill the header of a request. */
static void msg_init(mtl_message_t* msg, mtl_message_type_t type, uint32_t body_len) {
  memset(msg, 0, sizeof(*msg));
  msg->header.magic = htonl(MTL_MANAGER_MAGIC);
  msg->header.type = htonl((uint32_t)type);
  msg->header.body_len = htonl(body_len);
}

/*
 * Send one request and read one record of `expect` type into the same buffer.
 *
 * The body stays as it arrived, because not every reply carries a response
 * field: a heartbeat ack carries a sequence number in the same bytes.
 *
 * @return 0 on success, or a negative errno from the transport.
 */
static int exchange(mtlm_client* client, mtl_message_t* msg, mtl_message_type_t expect) {
  int ret;

  if (client == NULL || client->fd < 0) return -EINVAL;

  ret = send_all(client->fd, msg, MTL_MANAGER_MSG_SIZE);
  if (ret < 0) return ret;

  memset(msg, 0, sizeof(*msg));
  ret = recv_all(client->fd, msg, MTL_MANAGER_MSG_SIZE);
  if (ret < 0) return ret;

  if (ntohl(msg->header.magic) != MTL_MANAGER_MAGIC) return -EBADMSG;
  if (ntohl(msg->header.type) != (uint32_t)expect) return -EBADMSG;

  return 0;
}

/*
 * Send one request and read the response field of the reply.
 *
 * Only for a reply whose body is a mtl_response_message_t. That is every reply
 * but the heartbeat ack, including IF_QUEUE_ID and IF_FLOW_ID, which both carry
 * the id in the response field.
 *
 * @return The response field the manager sent, or a negative errno from the
 *         transport.
 */
static int request(mtlm_client* client, mtl_message_t* msg, mtl_message_type_t expect) {
  int ret = exchange(client, msg, expect);

  if (ret < 0) return ret;

  return (int)ntohl((uint32_t)msg->body.response_msg.response);
}

static int connect_path(const char* path) {
  struct sockaddr_un addr;
  size_t path_len = strlen(path);
  int fd, ret;

  /* An AF_UNIX path that does not fit must fail loudly. A silent truncation
   * connects to a different socket than the caller named. */
  if (path_len == 0 || path_len >= sizeof(addr.sun_path)) return -ENAMETOOLONG;

  fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return -errno;

  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  memcpy(addr.sun_path, path, path_len + 1);

  ret = connect(fd, (struct sockaddr*)&addr, sizeof(addr));
  if (ret < 0) {
    ret = -errno;
    close(fd);
    return ret;
  }

  return fd;
}

mtlm_client* mtlm_client_create(const char* sock_path) {
  char path[MTLM_SOCK_PATH_MAX];
  mtlm_client* client;
  int fd = -ENOENT;
  unsigned int i;

  if (sock_path != NULL) {
    fd = connect_path(sock_path);
    if (fd >= 0) snprintf(path, sizeof(path), "%s", sock_path);
  } else {
    for (i = 0; i < MTLM_SOCK_PATH_CANDIDATES; i++) {
      if (mtlm_sock_path_candidate(i, path, sizeof(path)) < 0) break;
      fd = connect_path(path);
      if (fd >= 0) break;
    }
  }

  if (fd < 0) {
    errno = -fd;
    return NULL;
  }

  client = (mtlm_client*)calloc(1, sizeof(*client));
  if (client == NULL) {
    close(fd);
    errno = ENOMEM;
    return NULL;
  }

  client->fd = fd;
  snprintf(client->sock_path, sizeof(client->sock_path), "%s", path);
  return client;
}

void mtlm_client_destroy(mtlm_client* client) {
  if (client == NULL) return;
  if (client->fd >= 0) close(client->fd);
  free(client);
}

int mtlm_client_fd(const mtlm_client* client) {
  if (client == NULL) return -1;
  return client->fd;
}

const char* mtlm_client_sock_path(const mtlm_client* client) {
  if (client == NULL) return NULL;
  return client->sock_path;
}

bool mtlm_manager_alive(const char* sock_path) {
  mtlm_client* client = mtlm_client_create(sock_path);

  if (client == NULL) return false;
  mtlm_client_destroy(client);
  return true;
}

const char* mtlm_proto_version(void) {
  static char version[16];

  if (version[0] == '\0')
    snprintf(version, sizeof(version), "%d.%d", MTL_MANAGER_PROTO_VERSION_MAJOR,
             MTL_MANAGER_PROTO_VERSION_MINOR);

  return version;
}

int mtlm_register(mtlm_client* client, const struct mtlm_register_args* args) {
  char hostname[MTL_MANAGER_HOSTNAME_LEN];
  mtl_register_message_t* reg;
  mtl_message_t msg;
  uint16_t i;

  if (args == NULL) return -EINVAL;
  if (args->num_if > MTL_MANAGER_MAX_IF) return -EINVAL;
  if (args->num_if > 0 && args->ifindex == NULL) return -EINVAL;

  msg_init(&msg, MTL_MSG_TYPE_REGISTER, sizeof(mtl_register_message_t));
  reg = &msg.body.register_msg;

  reg->pid = (pid_t)htonl((uint32_t)(args->pid != 0 ? args->pid : getpid()));
  reg->uid = (uid_t)htonl((uint32_t)(args->uid >= 0 ? (uid_t)args->uid : getuid()));

  /* The whole field goes on the wire below, so every byte of it must have a
   * value. Without this the padding carries the stack of the caller to the
   * manager, which writes it to the log. */
  memset(hostname, 0, sizeof(hostname));
  if (args->hostname != NULL) {
    snprintf(hostname, sizeof(hostname), "%s", args->hostname);
  } else if (gethostname(hostname, sizeof(hostname)) < 0) {
    snprintf(hostname, sizeof(hostname), "unknown");
  }
  hostname[sizeof(hostname) - 1] = '\0';
  memcpy(reg->hostname, hostname, sizeof(reg->hostname));

  reg->num_if = htons(args->num_if);
  for (i = 0; i < args->num_if; i++) reg->ifindex[i] = htonl(args->ifindex[i]);

  return request(client, &msg, MTL_MSG_TYPE_RESPONSE);
}

int mtlm_heartbeat(mtlm_client* client, uint32_t seq, uint32_t* acked) {
  mtl_message_t msg;
  int ret;

  msg_init(&msg, MTL_MSG_TYPE_HEARTBEAT, sizeof(mtl_heartbeat_message_t));
  msg.body.heartbeat_msg.seq = htonl(seq);

  /* exchange(), not request(): the ack carries the sequence number where a
   * response carries its code, so request() would read a sequence number with
   * the top bit set as an error. */
  ret = exchange(client, &msg, MTL_MSG_TYPE_HEARTBEAT_ACK);
  if (ret < 0) return ret;

  if (acked != NULL) *acked = ntohl(msg.body.heartbeat_msg.seq);
  return 0;
}

int mtlm_lcore_get(mtlm_client* client, uint16_t lcore_id) {
  mtl_message_t msg;

  msg_init(&msg, MTL_MSG_TYPE_GET_LCORE, sizeof(mtl_lcore_message_t));
  msg.body.lcore_msg.lcore = htons(lcore_id);

  return request(client, &msg, MTL_MSG_TYPE_RESPONSE);
}

int mtlm_lcore_put(mtlm_client* client, uint16_t lcore_id) {
  mtl_message_t msg;

  msg_init(&msg, MTL_MSG_TYPE_PUT_LCORE, sizeof(mtl_lcore_message_t));
  msg.body.lcore_msg.lcore = htons(lcore_id);

  return request(client, &msg, MTL_MSG_TYPE_RESPONSE);
}

int mtlm_queue_get(mtlm_client* client, unsigned int ifindex) {
  mtl_message_t msg;

  msg_init(&msg, MTL_MSG_TYPE_IF_GET_QUEUE, sizeof(mtl_if_message_t));
  msg.body.if_msg.ifindex = htonl(ifindex);

  return request(client, &msg, MTL_MSG_TYPE_IF_QUEUE_ID);
}

int mtlm_queue_put(mtlm_client* client, unsigned int ifindex, uint16_t queue_id) {
  mtl_message_t msg;

  msg_init(&msg, MTL_MSG_TYPE_IF_PUT_QUEUE, sizeof(mtl_if_message_t));
  msg.body.if_msg.ifindex = htonl(ifindex);
  msg.body.if_msg.queue_id = htons(queue_id);

  return request(client, &msg, MTL_MSG_TYPE_RESPONSE);
}

int mtlm_flow_add(mtlm_client* client, const struct mtlm_flow* flow) {
  mtl_message_t msg;

  if (flow == NULL) return -EINVAL;

  msg_init(&msg, MTL_MSG_TYPE_IF_ADD_FLOW, sizeof(mtl_if_message_t));
  msg.body.if_msg.ifindex = htonl(flow->ifindex);
  msg.body.if_msg.queue_id = htons(flow->queue_id);
  msg.body.if_msg.flow_type = htonl(flow->flow_type);
  /* The two addresses travel as they are. They are already network byte order,
   * which is also the order ethtool wants, so a htonl here would swap them
   * twice on a little endian host and steer the traffic nowhere. */
  msg.body.if_msg.src_ip = flow->src_ip;
  msg.body.if_msg.dst_ip = flow->dst_ip;
  msg.body.if_msg.src_port = htons(flow->src_port);
  msg.body.if_msg.dst_port = htons(flow->dst_port);

  return request(client, &msg, MTL_MSG_TYPE_IF_FLOW_ID);
}

int mtlm_flow_del(mtlm_client* client, unsigned int ifindex, uint32_t flow_id) {
  mtl_message_t msg;

  msg_init(&msg, MTL_MSG_TYPE_IF_DEL_FLOW, sizeof(mtl_if_message_t));
  msg.body.if_msg.ifindex = htonl(ifindex);
  msg.body.if_msg.flow_id = htonl(flow_id);

  return request(client, &msg, MTL_MSG_TYPE_RESPONSE);
}

static int udp_dp_filter(mtlm_client* client, unsigned int ifindex, uint16_t dst_port,
                         bool add) {
  mtl_message_t msg;

  msg_init(&msg, add ? MTL_MSG_TYPE_ADD_UDP_DP_FILTER : MTL_MSG_TYPE_DEL_UDP_DP_FILTER,
           sizeof(mtl_udp_dp_filter_message_t));
  msg.body.udp_dp_filter_msg.ifindex = htonl(ifindex);
  msg.body.udp_dp_filter_msg.port = htons(dst_port);

  return request(client, &msg, MTL_MSG_TYPE_RESPONSE);
}

int mtlm_udp_dp_filter_add(mtlm_client* client, unsigned int ifindex, uint16_t dst_port) {
  return udp_dp_filter(client, ifindex, dst_port, true);
}

int mtlm_udp_dp_filter_del(mtlm_client* client, unsigned int ifindex, uint16_t dst_port) {
  return udp_dp_filter(client, ifindex, dst_port, false);
}

int mtlm_xsk_map_fd(mtlm_client* client, unsigned int ifindex) {
  char control[CMSG_SPACE(sizeof(int))];
  struct cmsghdr* cmsg;
  struct msghdr hdr;
  mtl_message_t msg;
  struct iovec iov;
  char data[1] = {0};
  int ret, fd, taken;

  if (client == NULL || client->fd < 0) return -EINVAL;

  msg_init(&msg, MTL_MSG_TYPE_IF_XSK_MAP_FD, sizeof(mtl_if_message_t));
  msg.body.if_msg.ifindex = htonl(ifindex);

  ret = send_all(client->fd, &msg, MTL_MANAGER_MSG_SIZE);
  if (ret < 0) return ret;

  memset(&hdr, 0, sizeof(hdr));
  memset(control, 0, sizeof(control));
  iov.iov_base = data;
  iov.iov_len = sizeof(data);
  hdr.msg_iov = &iov;
  hdr.msg_iovlen = 1;
  hdr.msg_control = control;
  hdr.msg_controllen = sizeof(control);

  /* MSG_CMSG_CLOEXEC: the map of an AF_XDP port is not for a child process, and
   * the caller runs one. Without it every descriptor the manager hands over
   * stays open across each exec the caller makes. */
  do {
    ret = (int)recvmsg(client->fd, &hdr, MSG_CMSG_CLOEXEC);
  } while (ret < 0 && errno == EINTR);

  if (ret < 0) return -errno;
  if (ret == 0) return -ECONNRESET;

  /* The kernel puts every descriptor of the control data in this process before
   * the call returns, whatever the count. So close each one that is not the
   * answer, or the reply of a manager of another version leaks a descriptor on
   * every call, and an instance that opens ports in a loop runs out.
   *
   * The answer is one descriptor. Any other count means the peer is not a
   * manager this version can talk to, so keep none of it. */
  fd = -1;
  taken = 0;
  for (cmsg = CMSG_FIRSTHDR(&hdr); cmsg != NULL; cmsg = CMSG_NXTHDR(&hdr, cmsg)) {
    size_t i, count;

    if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) continue;
    if (cmsg->cmsg_len < CMSG_LEN(sizeof(int))) continue;

    count = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
    for (i = 0; i < count; i++) {
      int got;

      memcpy(&got, (char*)CMSG_DATA(cmsg) + i * sizeof(int), sizeof(got));
      if (got < 0) continue;
      taken++;
      if (fd < 0)
        fd = got;
      else
        close(got);
    }
  }

  /* MSG_CTRUNC says the control data did not fit, so more came than arrived. */
  if (taken != 1 || (hdr.msg_flags & MSG_CTRUNC)) {
    if (fd >= 0) close(fd);
    /* The manager answers with one byte and no control data when it has no
     * descriptor to pass, because SCM_RIGHTS cannot carry -1. */
    return -ENOTSUP;
  }

  return fd;
}

const char* mtlm_strerror(int ret) {
  if (ret >= 0) return "success";
  return strerror(-ret);
}
