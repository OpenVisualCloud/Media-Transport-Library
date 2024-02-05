/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#ifndef _MTL_MPROTO_HEAD_H_
#define _MTL_MPROTO_HEAD_H_

#include <net/if.h>
#include <stdint.h>
#include <unistd.h>

#if defined(__cplusplus)
extern "C" {
#endif

#define MTL_MANAGER_SOCK_PATH "/var/run/imtl/mtl_manager.sock"

#define MTL_MANAGER_MAGIC (0x494D544C) /* ASCII representation of "IMTL" */

#pragma pack(push, 1)

/* message type */
typedef enum {
  /* bidirectional */
  MTL_MSG_TYPE_RAW = 0,
  /* client to server */
  MTL_MSG_TYPE_CS = 100,
  MTL_MSG_TYPE_REGISTER,
  MTL_MSG_TYPE_HEARTBEAT,
  MTL_MSG_TYPE_GET_LCORE,
  MTL_MSG_TYPE_PUT_LCORE,
  MTL_MSG_TYPE_ADD_UDP_DP_FILTER,
  MTL_MSG_TYPE_DEL_UDP_DP_FILTER,
  MTL_MSG_TYPE_IF_XSK_MAP_FD,
  MTL_MSG_TYPE_IF_GET_QUEUE,
  MTL_MSG_TYPE_IF_PUT_QUEUE,
  MTL_MSG_TYPE_IF_ADD_FLOW,
  MTL_MSG_TYPE_IF_DEL_FLOW,
  /* server to client */
  MTL_MSG_TYPE_SC = 200,
  MTL_MSG_TYPE_RESPONSE,
  MTL_MSG_TYPE_IF_QUEUE_ID,
  MTL_MSG_TYPE_IF_FLOW_ID,
} mtl_message_type_t;

/* message header */
typedef struct {
  uint32_t magic;
  mtl_message_type_t type;
  uint32_t body_len;
} mtl_message_header_t;

typedef struct {
  pid_t pid;
  uid_t uid;
  char hostname[64];
  uint16_t num_if;
  unsigned int ifindex[8];
} mtl_register_message_t;

typedef struct {
  uint32_t seq;
} mtl_heartbeat_message_t;

typedef struct {
  unsigned int ifindex;
  uint16_t queue_id;
  uint32_t flow_id;   /* location in ethtool */
  uint32_t flow_type; /* flow type in ethtool */
  uint32_t src_ip;
  uint32_t dst_ip;
  uint16_t src_port;
  uint16_t dst_port;
} mtl_if_message_t;

typedef struct {
  uint16_t lcore;
} mtl_lcore_message_t;

typedef struct {
  unsigned int ifindex;
  uint16_t port;
} mtl_udp_dp_filter_message_t;

typedef struct {
  int response; /* 0 for success, negative for error, positive for other use */
} mtl_response_message_t;

typedef struct {
  mtl_message_header_t header;
  union {
    mtl_register_message_t register_msg;
    mtl_heartbeat_message_t heartbeat_msg;
    mtl_if_message_t if_msg;
    mtl_lcore_message_t lcore_msg;
    mtl_udp_dp_filter_message_t udp_dp_filter_msg;
    mtl_response_message_t response_msg;
  } body;
} mtl_message_t;

#pragma pack(pop)

#if defined(__cplusplus)
}
#endif

#endif