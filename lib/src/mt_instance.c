/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#include "mt_instance.h"

#ifndef WINDOWSENV

#include <sys/un.h>

#include "../manager/mtl_mproto.h"
#include "mt_log.h"
#include "mt_util.h"

int mt_instance_put_lcore(struct mtl_main_impl* impl, unsigned int lcore_id) {
  int ret;
  int sock = impl->instance_fd;

  mtl_message_t msg;
  msg.header.magic = htonl(MTL_MANAGER_MAGIC);
  msg.header.type = htonl(MTL_MSG_TYPE_PUT_LCORE);
  msg.body.lcore_msg.lcore = htons(lcore_id);
  msg.header.body_len = sizeof(msg.body.lcore_msg);

  ret = send(sock, &msg, sizeof(msg), 0);
  if (ret < 0) {
    err("%s, send message fail\n", __func__);
    return ret;
  }

  return 0;
}

int mt_instance_get_lcore(struct mtl_main_impl* impl, unsigned int lcore_id) {
  int ret;
  int sock = impl->instance_fd;

  mtl_message_t msg;
  msg.header.magic = htonl(MTL_MANAGER_MAGIC);
  msg.header.type = htonl(MTL_MSG_TYPE_GET_LCORE);
  msg.body.lcore_msg.lcore = htons(lcore_id);
  msg.header.body_len = htonl(sizeof(mtl_lcore_message_t));

  ret = send(sock, &msg, sizeof(mtl_message_t), 0);
  if (ret < 0) {
    err("%s, send message fail\n", __func__);
    return ret;
  }

  ret = recv(sock, &msg, sizeof(mtl_message_t), 0);

  if (ret < 0 || ntohl(msg.header.magic) != MTL_MANAGER_MAGIC ||
      ntohl(msg.header.type) != MTL_MSG_TYPE_RESPONSE) {
    err("%s, recv response fail\n", __func__);
    return -EIO;
  }

  int response = msg.body.response_msg.response;

  /* return negative value incase user check with < 0 */
  return -response;
}

int mt_instance_request_xsks_map_fd(struct mtl_main_impl* impl, unsigned int ifindex) {
  int ret;
  int xsks_map_fd = -1;
  int sock = impl->instance_fd;

  mtl_message_t mtl_msg;
  mtl_msg.header.magic = htonl(MTL_MANAGER_MAGIC);
  mtl_msg.header.type = htonl(MTL_MSG_TYPE_REQUEST_MAP_FD);
  mtl_msg.body.request_map_fd_msg.ifindex = htonl(ifindex);
  mtl_msg.header.body_len = htonl(sizeof(mtl_request_map_fd_message_t));

  ret = send(sock, &mtl_msg, sizeof(mtl_message_t), 0);
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
  strncpy(reg_msg->hostname, u_info->hostname, sizeof(reg_msg->hostname));

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

  ret = send(sock, &msg, sizeof(msg), 0);
  if (ret < 0) {
    err("%s, send message fail\n", __func__);
    close(sock);
    return ret;
  }

  ret = recv(sock, &msg, sizeof(msg), 0);
  if (ret < 0 || ntohl(msg.header.magic) != MTL_MANAGER_MAGIC ||
      ntohl(msg.header.type) != MTL_MSG_TYPE_RESPONSE) {
    err("%s, recv response fail\n", __func__);
    close(sock);
    return ret;
  }

  int response = msg.body.response_msg.response;
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

int mt_instance_get_lcore(struct mtl_main_impl* impl, unsigned int lcore_id) {
  MTL_MAY_UNUSED(impl);
  MTL_MAY_UNUSED(lcore_id);
  return -ENOTSUP;
}

int mt_instance_put_lcore(struct mtl_main_impl* impl, unsigned int lcore_id) {
  MTL_MAY_UNUSED(impl);
  MTL_MAY_UNUSED(lcore_id);
  return -ENOTSUP;
}

int mt_instance_request_xsks_map_fd(struct mtl_main_impl* impl, unsigned int ifindex) {
  MTL_MAY_UNUSED(impl);
  MTL_MAY_UNUSED(ifindex);
  return -ENOTSUP;
}

#endif