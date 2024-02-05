/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#include "mt_instance.h"

#ifndef WINDOWSENV

#include <sys/un.h>

#include "../manager/mtl_mproto.h"
#include "mt_log.h"
#include "mt_util.h"

static int instance_send_and_receive_message(int sock, mtl_message_t* msg,
                                             mtl_message_type_t response_type) {
  int ret = send(sock, msg, sizeof(*msg), 0);
  if (ret < 0) {
    err("%s, send message fail\n", __func__);
    return ret;
  }

  memset(msg, 0, sizeof(*msg));
  ret = recv(sock, msg, sizeof(*msg), 0);
  if (ret < 0 || ntohl(msg->header.magic) != MTL_MANAGER_MAGIC ||
      ntohl(msg->header.type) != response_type) {
    err("%s, recv response fail\n", __func__);
    return -EIO;
  }

  return ntohl(msg->body.response_msg.response);
}

int mt_instance_put_lcore(struct mtl_main_impl* impl, uint16_t lcore_id) {
  int sock = impl->instance_fd;

  mtl_message_t msg;
  msg.header.magic = htonl(MTL_MANAGER_MAGIC);
  msg.header.type = htonl(MTL_MSG_TYPE_PUT_LCORE);
  msg.body.lcore_msg.lcore = htons(lcore_id);
  msg.header.body_len = sizeof(msg.body.lcore_msg);

  return instance_send_and_receive_message(sock, &msg, MTL_MSG_TYPE_RESPONSE);
}

int mt_instance_get_lcore(struct mtl_main_impl* impl, uint16_t lcore_id) {
  int sock = impl->instance_fd;

  mtl_message_t msg;
  msg.header.magic = htonl(MTL_MANAGER_MAGIC);
  msg.header.type = htonl(MTL_MSG_TYPE_GET_LCORE);
  msg.body.lcore_msg.lcore = htons(lcore_id);
  msg.header.body_len = htonl(sizeof(mtl_lcore_message_t));

  return instance_send_and_receive_message(sock, &msg, MTL_MSG_TYPE_RESPONSE);
}

int mt_instance_request_xsks_map_fd(struct mtl_main_impl* impl, unsigned int ifindex) {
  int ret;
  int xsks_map_fd = -1;
  int sock = impl->instance_fd;

  mtl_message_t mtl_msg;
  mtl_msg.header.magic = htonl(MTL_MANAGER_MAGIC);
  mtl_msg.header.type = htonl(MTL_MSG_TYPE_IF_XSK_MAP_FD);
  mtl_msg.body.if_msg.ifindex = htonl(ifindex);
  mtl_msg.header.body_len = htonl(sizeof(mtl_if_message_t));

  ret = send(sock, &mtl_msg, sizeof(mtl_msg), 0);
  if (ret < 0) {
    err("%s(%u), send message fail\n", __func__, ifindex);
    return ret;
  }

  char cms[CMSG_SPACE(sizeof(int))];
  struct cmsghdr* cmsg;
  struct msghdr msg;
  struct iovec iov;
  int value;
  int len;

  iov.iov_base = &value;
  iov.iov_len = sizeof(int);

  memset(&msg, 0, sizeof(msg));
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = (caddr_t)cms;
  msg.msg_controllen = sizeof(cms);

  len = recvmsg(sock, &msg, 0);
  if (len < 0) {
    err("%s(%u), recv message fail\n", __func__, ifindex);
    return len;
  }

  cmsg = CMSG_FIRSTHDR(&msg);
  if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS ||
      cmsg->cmsg_len != CMSG_LEN(sizeof(int))) {
    err("%s(%u), invalid cmsg for map fd\n", __func__, ifindex);
    return -EINVAL;
  }

  xsks_map_fd = *(int*)CMSG_DATA(cmsg);
  if (xsks_map_fd < 0) {
    err("%s(%u), get xsks_map_fd fail, %s\n", __func__, ifindex, strerror(errno));
    return errno;
  }

  return xsks_map_fd;
}

int mt_instance_update_udp_dp_filter(struct mtl_main_impl* impl, unsigned int ifindex,
                                     uint16_t dst_port, bool add) {
  int sock = impl->instance_fd;

  mtl_message_t msg;
  msg.header.magic = htonl(MTL_MANAGER_MAGIC);
  msg.header.type =
      add ? htonl(MTL_MSG_TYPE_ADD_UDP_DP_FILTER) : htonl(MTL_MSG_TYPE_DEL_UDP_DP_FILTER);
  msg.body.udp_dp_filter_msg.ifindex = htonl(ifindex);
  msg.body.udp_dp_filter_msg.port = htons(dst_port);
  msg.header.body_len = htonl(sizeof(mtl_udp_dp_filter_message_t));

  return instance_send_and_receive_message(sock, &msg, MTL_MSG_TYPE_RESPONSE);
}

int mt_instance_get_queue(struct mtl_main_impl* impl, unsigned int ifindex) {
  int sock = impl->instance_fd;

  mtl_message_t msg;
  msg.header.magic = htonl(MTL_MANAGER_MAGIC);
  msg.header.type = htonl(MTL_MSG_TYPE_IF_GET_QUEUE);
  msg.body.if_msg.ifindex = htonl(ifindex);
  msg.header.body_len = htonl(sizeof(mtl_if_message_t));

  return instance_send_and_receive_message(sock, &msg, MTL_MSG_TYPE_IF_QUEUE_ID);
}

int mt_instance_put_queue(struct mtl_main_impl* impl, unsigned int ifindex,
                          uint16_t queue_id) {
  int sock = impl->instance_fd;

  mtl_message_t msg;
  msg.header.magic = htonl(MTL_MANAGER_MAGIC);
  msg.header.type = htonl(MTL_MSG_TYPE_IF_PUT_QUEUE);
  msg.body.if_msg.ifindex = htonl(ifindex);
  msg.body.if_msg.queue_id = htons(queue_id);
  msg.header.body_len = htonl(sizeof(mtl_if_message_t));

  return instance_send_and_receive_message(sock, &msg, MTL_MSG_TYPE_RESPONSE);
}

int mt_instance_add_flow(struct mtl_main_impl* impl, unsigned int ifindex,
                         uint16_t queue_id, uint32_t flow_type, uint32_t src_ip,
                         uint32_t dst_ip, uint16_t src_port, uint16_t dst_port) {
  int sock = impl->instance_fd;

  mtl_message_t msg;
  msg.header.magic = htonl(MTL_MANAGER_MAGIC);
  msg.header.type = htonl(MTL_MSG_TYPE_IF_ADD_FLOW);
  msg.body.if_msg.ifindex = htonl(ifindex);
  msg.body.if_msg.queue_id = htons(queue_id);
  msg.body.if_msg.flow_type = htonl(flow_type);
  msg.body.if_msg.src_ip = htonl(src_ip);
  msg.body.if_msg.dst_ip = htonl(dst_ip);
  msg.body.if_msg.src_port = htons(src_port);
  msg.body.if_msg.dst_port = htons(dst_port);
  msg.header.body_len = htonl(sizeof(mtl_if_message_t));

  return instance_send_and_receive_message(sock, &msg, MTL_MSG_TYPE_IF_FLOW_ID);
}

int mt_instance_del_flow(struct mtl_main_impl* impl, unsigned int ifindex,
                         uint32_t flow_id) {
  int sock = impl->instance_fd;

  mtl_message_t msg;
  msg.header.magic = htonl(MTL_MANAGER_MAGIC);
  msg.header.type = htonl(MTL_MSG_TYPE_IF_DEL_FLOW);
  msg.body.if_msg.ifindex = htonl(ifindex);
  msg.body.if_msg.flow_id = htonl(flow_id);
  msg.header.body_len = htonl(sizeof(mtl_if_message_t));

  return instance_send_and_receive_message(sock, &msg, MTL_MSG_TYPE_RESPONSE);
}

int mt_instance_init(struct mtl_main_impl* impl, struct mtl_init_params* p) {
  impl->instance_fd = -1;
  int sock = socket(AF_UNIX, SOCK_STREAM, 0);
  if (sock < 0) {
    err("%s, create socket fail %d\n", __func__, sock);
    return sock;
  }

  struct sockaddr_un addr;
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, MTL_MANAGER_SOCK_PATH, sizeof(addr.sun_path) - 1);
  int ret = connect(sock, (struct sockaddr*)&addr, sizeof(addr));
  if (ret < 0) {
    warn("%s, connect to manager fail, assume single instance mode\n", __func__);
    close(sock);
    return ret;
  }

  struct mt_user_info* u_info = &impl->u_info;

  mtl_message_t msg;
  msg.header.magic = htonl(MTL_MANAGER_MAGIC);
  msg.header.type = htonl(MTL_MSG_TYPE_REGISTER);
  msg.header.body_len = sizeof(mtl_register_message_t);

  mtl_register_message_t* reg_msg = &msg.body.register_msg;
  reg_msg->pid = htonl(u_info->pid);
  reg_msg->uid = htonl(getuid());
  strncpy(reg_msg->hostname, u_info->hostname, sizeof(reg_msg->hostname) - 1);
  reg_msg->hostname[sizeof(reg_msg->hostname) - 1] = '\0';

  /* manager will load xdp program for afxdp interfaces */
  uint16_t num_xdp_if = 0;
  for (int i = 0; i < p->num_ports; i++) {
    if (mtl_pmd_is_af_xdp(p->pmd[i])) {
      const char* if_name = mt_native_afxdp_port2if(p->port[i]);
      reg_msg->ifindex[i] = htonl(if_nametoindex(if_name));
      num_xdp_if++;
    }
  }
  reg_msg->num_if = htons(num_xdp_if);

  int response = instance_send_and_receive_message(sock, &msg, MTL_MSG_TYPE_RESPONSE);
  if (response != 0) {
    err("%s, register fail\n", __func__);
    close(sock);
    return -EIO;
  }

  impl->instance_fd = sock;

  info("%s, succ\n", __func__);

  return 0;
}

int mt_instance_uinit(struct mtl_main_impl* impl) {
  int sock = impl->instance_fd;
  if (sock <= 0) return -EIO;

  return close(sock);
}

#else /* not supported on Windows */

int mt_instance_init(struct mtl_main_impl* impl, struct mtl_init_params* p) {
  impl->instance_fd = -1;
  MTL_MAY_UNUSED(p);
  return -ENOTSUP;
}

int mt_instance_uinit(struct mtl_main_impl* impl) {
  MTL_MAY_UNUSED(impl);
  return -ENOTSUP;
}

int mt_instance_get_lcore(struct mtl_main_impl* impl, uint16_t lcore_id) {
  MTL_MAY_UNUSED(impl);
  MTL_MAY_UNUSED(lcore_id);
  return -ENOTSUP;
}

int mt_instance_put_lcore(struct mtl_main_impl* impl, uint16_t lcore_id) {
  MTL_MAY_UNUSED(impl);
  MTL_MAY_UNUSED(lcore_id);
  return -ENOTSUP;
}

int mt_instance_request_xsks_map_fd(struct mtl_main_impl* impl, unsigned int ifindex) {
  MTL_MAY_UNUSED(impl);
  MTL_MAY_UNUSED(ifindex);
  return -ENOTSUP;
}

int mt_instance_get_queue(struct mtl_main_impl* impl, unsigned int ifindex) {
  MTL_MAY_UNUSED(impl);
  MTL_MAY_UNUSED(ifindex);
  return -ENOTSUP;
}

int mt_instance_put_queue(struct mtl_main_impl* impl, unsigned int ifindex,
                          uint16_t queue_id) {
  MTL_MAY_UNUSED(impl);
  MTL_MAY_UNUSED(ifindex);
  MTL_MAY_UNUSED(queue_id);
  return -ENOTSUP;
}

int mt_instance_add_flow(struct mtl_main_impl* impl, unsigned int ifindex,
                         uint16_t queue_id, uint32_t flow_type, uint32_t src_ip,
                         uint32_t dst_ip, uint16_t src_port, uint16_t dst_port) {
  MTL_MAY_UNUSED(impl);
  MTL_MAY_UNUSED(ifindex);
  MTL_MAY_UNUSED(queue_id);
  MTL_MAY_UNUSED(flow_type);
  MTL_MAY_UNUSED(src_ip);
  MTL_MAY_UNUSED(dst_ip);
  MTL_MAY_UNUSED(src_port);
  MTL_MAY_UNUSED(dst_port);
  return -ENOTSUP;
}

int mt_instance_del_flow(struct mtl_main_impl* impl, unsigned int ifindex,
                         uint32_t flow_id) {
  MTL_MAY_UNUSED(impl);
  MTL_MAY_UNUSED(ifindex);
  MTL_MAY_UNUSED(flow_id);
  return -ENOTSUP;
}

#endif