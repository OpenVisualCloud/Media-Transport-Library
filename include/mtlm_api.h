/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

/**
 * @file mtlm_api.h
 *
 * Client API of MtlManager.
 *
 * This is the contract the manager implements and every client uses. One
 * function per manager operation, each a blocking request/response round trip
 * over the AF_UNIX socket described in mtl_mproto.h.
 *
 * Link with `pkg-config --libs mtlm_client`.
 *
 * Return values follow the library convention: 0 for success and a negative
 * errno for failure. Three calls return a non-negative resource id on success
 * (mtlm_queue_get, mtlm_flow_add, mtlm_xsk_map_fd).
 *
 * The socket path
 * ---------------
 * No caller should hard-code a path. mtlm_sock_path() gives the path a manager
 * binds, and it does not need root:
 *
 *   1. $MTL_MANAGER_SOCK_PATH, when set and not empty.
 *   2. /var/run/imtl/mtl_manager.sock, when the effective user is root.
 *   3. $XDG_RUNTIME_DIR/imtl/mtl_manager.sock, when that variable is set.
 *   4. /tmp/imtl-<uid>/mtl_manager.sock.
 *
 * A client walks mtlm_sock_path_candidate() in order, so a program that runs
 * without root finds a per-user manager first and a system manager second.
 *
 * Thread safety
 * -------------
 * One mtlm_client is one socket and one outstanding request. It is not
 * thread safe. Give each thread its own client, or serialize the calls.
 */

#ifndef _MTLM_API_H_
#define _MTLM_API_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mtl_mproto.h"

#if defined(__cplusplus)
extern "C" {
#endif

/** Longest socket path the AF_UNIX address can hold, including the zero. */
#define MTLM_SOCK_PATH_MAX (108)

/** Number of paths mtlm_sock_path_candidate() can return. */
#define MTLM_SOCK_PATH_CANDIDATES (2)

/** Opaque connection to a manager. */
typedef struct mtlm_client mtlm_client;

/** Arguments of mtlm_register(). */
struct mtlm_register_args {
  /** Process id to report. 0 asks the API to use getpid(). */
  int pid;
  /** User id to report. -1 asks the API to use getuid(). */
  int uid;
  /** Host name to report. NULL asks the API to use gethostname(). */
  const char* hostname;
  /** Interface indexes the manager must prepare. May be NULL when num_if is 0. */
  const unsigned int* ifindex;
  /** Number of entries in ifindex, at most MTL_MANAGER_MAX_IF. */
  uint16_t num_if;
};

/**
 * Arguments of mtlm_flow_add().
 *
 * The byte order is not the same for the two field kinds, because ethtool
 * wants an address big endian and a port host order:
 *   - src_ip and dst_ip are network byte order, the order of an in_addr.
 *   - src_port and dst_port are host byte order.
 */
struct mtlm_flow {
  unsigned int ifindex;
  /** Receive queue the matching traffic must land on. */
  uint16_t queue_id;
  /** ethtool flow type, for example UDP_V4_FLOW (0x02). */
  uint32_t flow_type;
  /** 0 leaves the field out of the match. */
  uint32_t src_ip;
  uint32_t dst_ip;
  uint16_t src_port;
  uint16_t dst_port;
};

/*
 * Socket path
 */

/**
 * Resolve the path a manager binds, by the rules in the file comment.
 *
 * @param buf   Output buffer.
 * @param len   Size of buf.
 * @return 0 on success, -EINVAL on a bad argument, -ENAMETOOLONG when the
 *         resolved path does not fit in buf or in an AF_UNIX address.
 */
int mtlm_sock_path_resolve(char* buf, size_t len);

/**
 * Same as mtlm_sock_path_resolve() into a static per-process buffer.
 *
 * @return The path, or NULL when it cannot be resolved.
 */
const char* mtlm_sock_path(void);

/**
 * Return candidate number `index` of the client search order.
 *
 * With MTL_MANAGER_SOCK_ENV set there is exactly one candidate. Without it
 * there are two: the per-user path and then the system path. Root sees the
 * system path first.
 *
 * @return 0 on success, -ENOENT when index is past the last candidate.
 */
int mtlm_sock_path_candidate(unsigned int index, char* buf, size_t len);

/**
 * Create every missing component of the directory part of `path`.
 *
 * A manager calls this before it binds. Use 0700 for a per-user socket and
 * 0755 for a socket other users must reach, because a client cannot traverse
 * a directory it cannot read.
 *
 * @param path  Path of the socket, not of the directory.
 * @param mode  Mode of each directory this call creates.
 * @return 0 on success, -EINVAL when path is NULL or holds no '/',
 *         -ENAMETOOLONG when the directory part does not fit an AF_UNIX
 *         address, and the negative errno of mkdir for anything else.
 */
int mtlm_sock_dir_prepare(const char* path, unsigned int mode);

/**
 * Report whether `path` is the system-wide socket path.
 *
 * A manager uses it to choose the directory mode and the socket mode.
 */
bool mtlm_sock_path_is_system(const char* path);

/*
 * Connection
 */

/**
 * Connect to a manager.
 *
 * @param sock_path Path to connect to, or NULL to walk the candidate order.
 * @return A client, or NULL when no manager answers. errno is set.
 */
mtlm_client* mtlm_client_create(const char* sock_path);

/** Close the connection and free the client. Accepts NULL. */
void mtlm_client_destroy(mtlm_client* client);

/** Connected socket, for a caller that polls it. -1 for NULL. */
int mtlm_client_fd(const mtlm_client* client);

/** Path the client connected to, or NULL for NULL. */
const char* mtlm_client_sock_path(const mtlm_client* client);

/**
 * Report whether a manager answers a connect.
 *
 * @param sock_path Path to probe, or NULL to walk the candidate order.
 */
bool mtlm_manager_alive(const char* sock_path);

/** Protocol version this build speaks, as "major.minor". */
const char* mtlm_proto_version(void);

/*
 * Operations. Each one sends a request and waits for the response.
 */

/**
 * Register the instance. Every other operation except mtlm_heartbeat() and
 * mtlm_xsk_map_fd() needs a registered instance.
 */
int mtlm_register(mtlm_client* client, const struct mtlm_register_args* args);

/**
 * Round trip a sequence number, to prove the manager still runs.
 *
 * @param seq     Sequence number to send.
 * @param acked   Receives the number the manager echoed. May be NULL.
 */
int mtlm_heartbeat(mtlm_client* client, uint32_t seq, uint32_t* acked);

/** Claim `lcore_id` for this instance. -EBUSY when another instance holds it. */
int mtlm_lcore_get(mtlm_client* client, uint16_t lcore_id);

/** Release `lcore_id`. -EINVAL when this instance does not hold it. */
int mtlm_lcore_put(mtlm_client* client, uint16_t lcore_id);

/**
 * Reserve a receive queue on `ifindex`.
 *
 * @return The queue id, 1 or greater, or a negative errno. Queue 0 always
 *         stays with the kernel.
 */
int mtlm_queue_get(mtlm_client* client, unsigned int ifindex);

/** Release a queue reserved by mtlm_queue_get(). */
int mtlm_queue_put(mtlm_client* client, unsigned int ifindex, uint16_t queue_id);

/**
 * Insert an ethtool flow rule that steers matching traffic to a queue.
 *
 * @return The flow id, 1 or greater, or a negative errno.
 */
int mtlm_flow_add(mtlm_client* client, const struct mtlm_flow* flow);

/** Delete a flow rule that mtlm_flow_add() returned. */
int mtlm_flow_del(mtlm_client* client, unsigned int ifindex, uint32_t flow_id);

/** Let UDP destination port `dst_port` reach the AF_XDP socket. */
int mtlm_udp_dp_filter_add(mtlm_client* client, unsigned int ifindex, uint16_t dst_port);

/** Undo one mtlm_udp_dp_filter_add(). The manager counts the references. */
int mtlm_udp_dp_filter_del(mtlm_client* client, unsigned int ifindex, uint16_t dst_port);

/**
 * Receive the xsks map descriptor of `ifindex` over SCM_RIGHTS.
 *
 * @return A descriptor the caller must close, or a negative errno. -ENOTSUP
 *         when the manager has no XDP backend.
 */
int mtlm_xsk_map_fd(mtlm_client* client, unsigned int ifindex);

/** Text for a value any of the calls above returned. */
const char* mtlm_strerror(int ret);

#if defined(__cplusplus)
}
#endif

#endif
