/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#ifndef __ET_H
#define __ET_H

#define ET_XDP_UNIX_SOCKET_PATH "/var/run/et_xdp.sock"

struct udp_send_event {
  int pid;
  int udp_send_cnt;
  unsigned int gso_size;
  unsigned long long duration_ns;
  unsigned int udp_send_bytes;
  int ret;
};

enum et_args_cmd {
  ET_ARG_UNKNOWN = 0,
  ET_ARG_PRINT_LIBBPF = 0x100, /* start from end of ascii */
  ET_ARG_PROG,
  ET_ARG_IFNAME,
  ET_ARG_XDP_PATH,
  ET_ARG_HELP,
};

enum et_prog_type {
  ET_PROG_UNKNOWN = 0,
  ET_PROG_FENTRY,
  ET_PROG_KPROBE,
  ET_PROG_TRACEPOINT,
  ET_PROG_XDP,
};

static const char* prog_type_str[] = {
    [ET_PROG_FENTRY] = "fentry",
    [ET_PROG_KPROBE] = "kprobe",
    [ET_PROG_TRACEPOINT] = "tracepoint",
    [ET_PROG_XDP] = "xdp",
};

struct et_ctx {
  enum et_prog_type prog_type;
  int xdp_ifindex[8];
  int xdp_if_cnt;
  char* xdp_path;
};

#endif /* __ET_H */