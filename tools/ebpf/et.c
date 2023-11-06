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
  struct sockaddr_un addr;
  int ret = 0;
  int xsks_map_fd[ctx->xdp_if_cnt];
  int sock = -1, conn;
  struct xdp_program* prog = NULL;

  if (ctx->xdp_if_cnt <= 0) {
    printf("please specify interfaces with --ifname <a,b,...>\n");
    return -EIO;
  }

  if (ctx->xdp_path) {
    prog = xdp_program__open_file(ctx->xdp_path, "xdp", NULL);
    ret = libxdp_get_error(prog);
    if (ret) {
      printf("failed to load xdp program\n");
      return -EIO;
    }
  }

  /* load xdp program for each interface */
  for (int i = 0; i < ctx->xdp_if_cnt; i++) {
    int ifindex = ctx->xdp_ifindex[i];
    if (prog) ret = xdp_program__attach(prog, ifindex, XDP_MODE_NATIVE, 0);
    if (ret < 0) {
      printf("xdp_program__attach failed\n");
      goto cleanup;
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
    char ifname[IFNAMSIZ];
    int map_fd = -1;
    recv(conn, ifname, sizeof(ifname), 0);
    printf("request xsk_map_fd for ifname %s\n", ifname);
    int ifindex = if_nametoindex(ifname);
    for (int i = 0; i < ctx->xdp_if_cnt; i++) {
      if (ctx->xdp_ifindex[i] == ifindex) {
        map_fd = xsks_map_fd[i];
        break;
      }
    }
    if (map_fd < 0) {
      printf("xsk_map_fd not found for %s\n", ifname);
      goto cleanup;
    }
    send_fd(conn, map_fd);
    close(conn);
    printf("map_fd %d sent, close conn\n", map_fd);
  }

cleanup:
  if (sock >= 0) close(sock);
  if (prog) {
    for (int i = 0; i < ctx->xdp_if_cnt; i++) {
      int ifindex = ctx->xdp_ifindex[i];
      xdp_program__detach(prog, ifindex, XDP_MODE_NATIVE, 0);
    }
    xdp_program__close(prog);
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