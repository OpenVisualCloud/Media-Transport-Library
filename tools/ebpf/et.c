/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#include "et.h"

#include <bpf/libbpf.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <net/if.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#include <xdp/libxdp.h>
#include <xdp/xsk.h>

static volatile bool stop = false;

static int libbpf_print_fn(enum libbpf_print_level level, const char* format,
                           va_list args) {
  return vfprintf(stderr, format, args);
}

static void et_sig_handler(int signo) {
  printf("%s, signal %d\n", __func__, signo);

  switch (signo) {
    case SIGINT: /* Interrupt from keyboard */
      stop = true;
      break;
  }

  return;
}

static int udp_send_handler(void* ctx, void* data, size_t data_sz) {
  const struct udp_send_event* e = data;

  printf("%s: pid %d, gso_size %u, bytes %u, duration_ns %llu\n", __func__, e->pid,
         e->gso_size, e->udp_send_bytes, e->duration_ns);
  return 0;
}

static int et_fentry_loop() {
  struct ring_buffer* rb = NULL;
  struct fentry_bpf* skel;
  int ret = 0;

  skel = fentry_bpf__open_and_load();
  if (!skel) {
    printf("failed to open BPF skeleton\n");
    return -1;
  }

  ret = fentry_bpf__attach(skel);
  if (ret) {
    printf("failed to attach BPF skeleton\n");
    goto cleanup;
  }

  printf("fentry_bpf__attach() succeeded\n");

  rb = ring_buffer__new(bpf_map__fd(skel->maps.rb), udp_send_handler, NULL, NULL);
  if (!rb) {
    ret = -1;
    fprintf(stderr, "failed to create ring buffer\n");
    goto cleanup;
  }

  while (!stop) {
    ret = ring_buffer__poll(rb, 100);
    if (ret == -EINTR) {
      ret = 0;
      break;
    }
    if (ret < 0) {
      printf("error polling perf buffer: %d\n", ret);
      break;
    }
  }

cleanup:
  ring_buffer__free(rb);
  fentry_bpf__destroy(skel);
  return ret;
}

static int send_fd(int sock, int fd) {
  struct msghdr msg;
  struct iovec iov[1];
  struct cmsghdr* cmsg = NULL;
  char ctrl_buf[CMSG_SPACE(sizeof(int))];
  char data[1];

  memset(&msg, 0, sizeof(struct msghdr));
  memset(ctrl_buf, 0, CMSG_SPACE(sizeof(int)));

  data[0] = ' ';
  iov[0].iov_base = data;
  iov[0].iov_len = sizeof(data);

  msg.msg_name = NULL;
  msg.msg_namelen = 0;
  msg.msg_iov = iov;
  msg.msg_iovlen = 1;
  msg.msg_controllen = CMSG_SPACE(sizeof(int));
  msg.msg_control = ctrl_buf;

  cmsg = CMSG_FIRSTHDR(&msg);
  cmsg->cmsg_level = SOL_SOCKET;
  cmsg->cmsg_type = SCM_RIGHTS;
  cmsg->cmsg_len = CMSG_LEN(sizeof(int));

  *((int*)CMSG_DATA(cmsg)) = fd;

  return sendmsg(sock, &msg, 0);
}

static int et_xdp_loop(struct et_ctx* ctx) {
  int ret = 0;
  int if_cnt = ctx->xdp_if_cnt;

  if (if_cnt <= 0) {
    printf("please specify interfaces with --ifname <a,b,...>\n");
    return -EIO;
  }

  struct sockaddr_un addr;
  int xsks_map_fd[if_cnt];
  int sock = -1, conn;
  struct xdp_program* prog[if_cnt];

  /* load xdp program for each interface */
  for (int i = 0; i < if_cnt; i++) {
    if (ctx->xdp_path) {
      prog[i] = xdp_program__open_file(ctx->xdp_path, "xdp", NULL);
      ret = libxdp_get_error(prog[i]);
      if (ret) {
        printf("failed to load xdp program\n");
        goto cleanup;
      }
    }
    int ifindex = ctx->xdp_ifindex[i];
    if (prog[i]) {
      ret = xdp_program__attach(prog[i], ifindex, XDP_MODE_NATIVE, 0);
      if (ret < 0) {
        printf("xdp_program__attach failed\n");
        goto cleanup;
      }
    }

    ret = xsk_setup_xdp_prog(ifindex, &xsks_map_fd[i]);
    if (ret || xsks_map_fd[i] < 0) {
      printf("xsk_socket__bind failed\n");
      goto cleanup;
    }
  }

  sock = socket(AF_UNIX, SOCK_STREAM, 0);
  unlink(ET_XDP_UNIX_SOCKET_PATH);

  int flags = fcntl(sock, F_GETFL, 0);
  fcntl(sock, F_SETFL, flags | O_NONBLOCK);

  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strcpy(addr.sun_path, ET_XDP_UNIX_SOCKET_PATH);
  bind(sock, (struct sockaddr*)&addr, sizeof(addr));

  chmod(ET_XDP_UNIX_SOCKET_PATH, 0666); /* allow non-root user to connect */

  listen(sock, 1);

  printf("waiting socket connection...\n");
  while (!stop) {
    conn = accept(sock, NULL, 0);
    if (conn < 0) {
      if (errno != EAGAIN && errno != EWOULDBLOCK) {
        perror("accept error");
        ret = -1;
        goto cleanup;
      }
      sleep(1);
      continue;
    }
    printf("\nsocket connection %d accepted\n", conn);
    char command[64];
    recv(conn, command, sizeof(command), 0);
    if (command[0]) {
      printf("command: %s\n", command);
      char* magic = strtok(command, ":");
      if (strncmp(magic, "imtl", strlen("imtl")) == 0) {
        char* type = strtok(NULL, ":");
        if (strncmp(type, "if", strlen("if")) == 0) {
          char* ifname = strtok(NULL, ":");
          int ifindex = if_nametoindex(ifname);
          int if_id = -1;
          for (int i = 0; i < if_cnt; i++) {
            if (ctx->xdp_ifindex[i] == ifindex) {
              if_id = i;
              break;
            }
          }
          if (if_id != -1) {
            char* action = strtok(NULL, ":");
            if (strncmp(action, "get_xsk_map", strlen("get_xsk_map")) == 0) {
              int map_fd = xsks_map_fd[if_id];
              if (map_fd >= 0) {
                send_fd(conn, map_fd);
                printf("map_fd %d sent\n", map_fd);
              }
            } else if (strncmp(action, "dp_add_filter", strlen("dp_add_filter")) == 0 ||
                       strncmp(action, "dp_del_filter", strlen("dp_del_filter")) == 0) {
              /* update dest port for udp4_dp_filter array map */
              char* port = strtok(NULL, ":");
              int port_num = atoi(port);
              if (prog[if_id] && port_num > 0 && port_num < 65535) {
                int map_fd = bpf_map__fd(bpf_object__find_map_by_name(
                    xdp_program__bpf_obj(prog[if_id]), "udp4_dp_filter"));
                if (map_fd >= 0) {
                  int value = 1;
                  if (strncmp(type, "dp_del_filter", strlen("dp_del_filter")) == 0)
                    value = 0;
                  ret = bpf_map_update_elem(map_fd, &port_num, &value, BPF_ANY);
                  if (ret < 0) printf("bpf_map_update_elem failed\n");
                }
              }
            }
          }
        } else if (strncmp(type, "ping", strlen("ping")) == 0) {
          char buf[5];
          snprintf(buf, sizeof(buf), "pong");
          send(conn, buf, 5, 0);
        }
      }
    }

    close(conn);
  }

cleanup:
  if (sock >= 0) close(sock);
  for (int i = 0; i < if_cnt; i++) {
    int ifindex = ctx->xdp_ifindex[i];
    if (prog[i] && !libxdp_get_error(prog[i])) {
      xdp_program__detach(prog[i], ifindex, XDP_MODE_NATIVE, 0);
      xdp_program__close(prog[i]);
      prog[i] = NULL;
    }
  }

  return ret;
}

static struct option et_args_options[] = {
    {"print", no_argument, 0, ET_ARG_PRINT_LIBBPF},
    {"prog", required_argument, 0, ET_ARG_PROG},
    {"ifname", required_argument, 0, ET_ARG_IFNAME},
    {"xdp_path", required_argument, 0, ET_ARG_XDP_PATH},
    {"help", no_argument, 0, ET_ARG_HELP},
    {0, 0, 0, 0}};

static void et_print_help() {
  printf("\n");
  printf("##### Usage: #####\n\n");

  printf(" Params:\n");
  printf("  --help                                  Print this help information\n");
  printf("  --print                                 Print libbpf output\n");

  printf("\n Prog Commands:\n");
  printf("  --prog <type>                           Attach to program of <type>\n");
  printf(
      "  --prog xdp --ifname <name1,name2>       Attach XDP program to specified "
      "interface names\n");
  printf(
      "  --prog xdp --xdp_path /path/to/xdp.o    Load a custom XDP kernel program from "
      "the specified path\n");

  printf("\n");
}

static int et_parse_args(struct et_ctx* ctx, int argc, char** argv) {
  int cmd = -1, opt_idx = 0;

  while (1) {
    cmd = getopt_long_only(argc, argv, "hv", et_args_options, &opt_idx);
    if (cmd == -1) break;

    switch (cmd) {
      case ET_ARG_PROG:
        if (strcmp(optarg, "fentry") == 0) {
          ctx->prog_type = ET_PROG_FENTRY;
        } else if (strcmp(optarg, "xdp") == 0) {
          ctx->prog_type = ET_PROG_XDP;
        }
        break;
      case ET_ARG_PRINT_LIBBPF:
        libbpf_set_print(libbpf_print_fn);
        break;
      case ET_ARG_IFNAME:
        char* ifname;
        ctx->xdp_if_cnt = 0;
        ifname = strtok(optarg, ",");
        while (ifname) {
          ctx->xdp_ifindex[ctx->xdp_if_cnt++] = if_nametoindex(ifname);
          ifname = strtok(NULL, ",");
        }
        break;
      case ET_ARG_XDP_PATH:
        ctx->xdp_path = optarg;
        break;
      case ET_ARG_HELP:
      default:
        et_print_help();
        return -1;
    }
  }

  return 0;
}

int main(int argc, char** argv) {
  struct et_ctx ctx;
  memset(&ctx, 0, sizeof(ctx));
  et_parse_args(&ctx, argc, argv);
  signal(SIGINT, et_sig_handler);

  printf("prog type is %s\n", prog_type_str[ctx.prog_type]);
  switch (ctx.prog_type) {
    case ET_PROG_FENTRY:
      et_fentry_loop();
      break;
    case ET_PROG_XDP:
      et_xdp_loop(&ctx);
      break;
    default:
      break;
  }

  return 0;
}