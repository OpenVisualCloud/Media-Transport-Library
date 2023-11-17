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
    if (p->pmd[i] == MTL_PMD_NATIVE_AF_XDP) {
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

#endif