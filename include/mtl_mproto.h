/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

/**
 * @file mtl_mproto.h
 *
 * Wire protocol between an MTL instance (client) and MtlManager (server).
 *
 * The transport is a SOCK_STREAM AF_UNIX socket. Every request and every
 * response is one fixed-size mtl_message_t record, so framing is "read exactly
 * sizeof(mtl_message_t) bytes". A stream socket may coalesce records or split
 * one, therefore both peers must buffer until a whole record is present and
 * must then consume every whole record in the buffer.
 *
 * All multi-byte fields travel in network byte order.
 *
 * For the callable client API and the socket path rules, see mtlm_api.h.
 */

#ifndef _MTL_MPROTO_HEAD_H_
#define _MTL_MPROTO_HEAD_H_

#include <net/if.h>
#include <stdint.h>
#include <unistd.h>

#if defined(__cplusplus)
extern "C" {
#endif

/**
 * Socket path of a system-wide MtlManager, used when the manager runs as root.
 * Do not hard-code it: call mtlm_sock_path() from mtlm_api.h, which also
 * honours MTL_MANAGER_SOCK_PATH and XDG_RUNTIME_DIR so that a manager and a
 * client can both run without root.
 */
#define MTL_MANAGER_SOCK_PATH "/var/run/imtl/mtl_manager.sock"

/** Directory that holds the system-wide socket. */
#define MTL_MANAGER_SOCK_DIR "/var/run/imtl"

/** File name of the socket, appended to whichever directory is in use. */
#define MTL_MANAGER_SOCK_NAME "mtl_manager.sock"

/** Environment variable that overrides the socket path for both peers. */
#define MTL_MANAGER_SOCK_ENV "MTL_MANAGER_SOCK_PATH"

#define MTL_MANAGER_MAGIC (0x494D544C) /* ASCII representation of "IMTL" */

/**
 * Protocol version. Bump the minor for a backward compatible addition, the
 * major for a change of an existing record layout. It is reported by the
 * manager in its startup log and by mtlm_proto_version().
 */
#define MTL_MANAGER_PROTO_VERSION_MAJOR (1)
#define MTL_MANAGER_PROTO_VERSION_MINOR (1)

/** Number of interface indexes a single register message can carry. */
#define MTL_MANAGER_MAX_IF (8)

/**
 * Number of lcores the manager accounts for. mtlm_lcore_get() and
 * mtlm_lcore_put() answer -EINVAL for an id of this value or more.
 */
#define MTL_MANAGER_MAX_LCORE (128)

/** Length of the hostname field, including the terminating zero. */
#define MTL_MANAGER_HOSTNAME_LEN (64)

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
  MTL_MSG_TYPE_HEARTBEAT_ACK,
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
  char hostname[MTL_MANAGER_HOSTNAME_LEN];
  uint16_t num_if;
  unsigned int ifindex[MTL_MANAGER_MAX_IF];
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

/** Size of one framed record on the wire. */
#define MTL_MANAGER_MSG_SIZE (sizeof(mtl_message_t))

#if defined(__cplusplus)
}
#endif

#endif
