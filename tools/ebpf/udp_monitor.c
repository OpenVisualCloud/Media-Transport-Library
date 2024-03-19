/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

// clang-format off
#include <linux/if_ether.h>
#include "udp_monitor.h"
// clang-format on

#include <arpa/inet.h>
#include <getopt.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "log.h"
#include "udp_monitor.skel.h"

struct udp_monitor_ctx {
  const char* interface;
};

enum um_args_cmd {
  UM_ARG_UNKNOWN = 0,
  UM_ARG_INTERFACE = 0x100, /* start from end of ascii */
  UM_ARG_HELP,
};

static struct option um_args_options[] = {
    {"interface", required_argument, 0, UM_ARG_INTERFACE},
    {"help", no_argument, 0, UM_ARG_HELP},
    {0, 0, 0, 0}};

static void um_print_help() {
  printf("\n");
  printf("##### Usage: #####\n\n");

  printf(" Params:\n");
  printf("  --interface <if>    Set the network interface\n");
  printf("  --help              Print help info\n");

  printf("\n");
}

static int um_parse_args(struct udp_monitor_ctx* ctx, int argc, char** argv) {
  int cmd = -1, opt_idx = 0;

  while (1) {
    cmd = getopt_long_only(argc, argv, "hv", um_args_options, &opt_idx);
    if (cmd == -1) break;

    switch (cmd) {
      case UM_ARG_INTERFACE:
        ctx->interface = optarg;
        break;
      case UM_ARG_HELP:
      default:
        um_print_help();
        return -1;
    }
  }

  return 0;
}

static int udp_hdr_entry_handler(void* pri, void* data, size_t data_sz) {
  struct udp_monitor_ctx* ctx = pri;
  const struct udp_pkt_entry* e = data;

  uint8_t* dip = (uint8_t*)&e->dst_ip;
  uint8_t* sip = (uint8_t*)&e->src_ip;

  info("%s, %u.%u.%u.%u -> %u.%u.%u.%u\n", __func__, sip[0], sip[1], sip[2], sip[3],
       dip[0], dip[1], dip[2], dip[3]);

  return 0;
}

static int open_raw_sock(const char* if_name) {
  struct sockaddr_ll sll;
  int fd;
  int ret;

  fd = socket(PF_PACKET, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, htons(ETH_P_ALL));
  if (fd < 0) {
    err("%s, failed to create raw socket\n", __func__);
    return -1;
  }

  memset(&sll, 0, sizeof(sll));
  sll.sll_family = AF_PACKET;
  sll.sll_ifindex = if_nametoindex(if_name);
  sll.sll_protocol = htons(ETH_P_ALL);
  ret = bind(fd, (struct sockaddr*)&sll, sizeof(sll));
  if (ret < 0) {
    err("%s, failed to bind to %s: %d, %s\n", __func__, if_name, ret, strerror(errno));
    close(fd);
    return ret;
  }

  return fd;
}

static int enable_promisc(int sock, const char* if_name, int enable) {
  struct ifreq ifr;
  int ret;

  memset(&ifr, 0, sizeof(ifr));
  strncpy(ifr.ifr_name, if_name, sizeof(ifr.ifr_name) - 1);

  ret = ioctl(sock, SIOCGIFFLAGS, &ifr);
  if (ret < 0) {
    err("%s, failed to SIOCGIFFLAGS for %s\n", __func__, if_name);
    return ret;
  }

  if (enable)
    ifr.ifr_flags |= IFF_PROMISC;
  else
    ifr.ifr_flags &= ~IFF_PROMISC;

  ret = ioctl(sock, SIOCSIFFLAGS, &ifr);
  if (ret < 0) {
    err("%s, failed to SIOCSIFFLAGS for %s\n", __func__, if_name);
    return ret;
  }

  return 0;
}

static bool g_um_stop = false;

static void um_sig_handler(int signo) {
  info("%s, signal %d\n", __func__, signo);

  switch (signo) {
    case SIGINT: /* Interrupt from keyboard */
      g_um_stop = true;
      break;
  }

  return;
}

int main(int argc, char** argv) {
  struct udp_monitor_ctx ctx;
  int ret;
  int sock_raw_fd = -1;
  int sock_fd = -1;
  struct udp_monitor_bpf* skel = NULL;
  struct ring_buffer* rb = NULL;
  int prog_fd = -1;

  memset(&ctx, 0, sizeof(ctx));
  ret = um_parse_args(&ctx, argc, argv);
  if (ret < 0) return ret;
  if (!ctx.interface) {
    err("%s, no interface assigned\n", __func__);
    um_print_help();
    return -1;
  }

  /* Create raw socket */
  sock_raw_fd = open_raw_sock(ctx.interface);
  if (sock_raw_fd < 0) {
    err("%s, failed to open raw sock %d\n", __func__, sock_raw_fd);
    goto exit;
  }
  sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock_fd < 0) {
    err("%s, failed to open sock_fd for promisc %d\n", __func__, sock_fd);
    goto exit;
  }

  skel = udp_monitor_bpf__open_and_load();
  if (!skel) {
    err("%s, failed to open and load skeleton\n", __func__);
    goto exit;
  }
  int udp_hdr_rb_fd = bpf_map__fd(skel->maps.udp_hdr_rb);
  rb = ring_buffer__new(udp_hdr_rb_fd, udp_hdr_entry_handler, &ctx, NULL);
  if (!rb) {
    err("%s, create ring buffer fail\n", __func__);
    goto exit;
  }
  ret = udp_monitor_bpf__attach(skel);
  if (ret) {
    err("%s, failed to attach skeleton\n", __func__);
    goto exit;
  }
  info("%s, attach socket skeleton succ\n", __func__);

  /* Attach BPF program to raw socket */
  prog_fd = bpf_program__fd(skel->progs.bpf_socket_handler);
  ret = setsockopt(sock_raw_fd, SOL_SOCKET, SO_ATTACH_BPF, &prog_fd, sizeof(prog_fd));
  if (ret < 0) {
    err("%s, failed attach to raw socket %d\n", __func__, ret);
    goto exit;
  }
  info("%s, attach bpf skeleton to %s succ, sock_raw_fd %d\n", __func__, ctx.interface,
       sock_raw_fd);
  ret = enable_promisc(sock_fd, ctx.interface, 1);
  if (ret < 0) {
    err("%s, failed to enable promisc %d\n", __func__, ret);
    goto exit;
  }
  info("%s, enable promisc for %s succ, sock_fd %d\n", __func__, ctx.interface, sock_fd);

  signal(SIGINT, um_sig_handler);

  info("%s, start to poll udp pkts for %s\n", __func__, ctx.interface);
  while (!g_um_stop) {
    ret = ring_buffer__poll(rb, 100);
    if (ret == -EINTR) {
      ret = 0;
      break;
    }
    if (ret < 0) {
      err("%s, polling fail\n", __func__);
      break;
    }
  }

  info("%s, stop now\n", __func__);
  enable_promisc(sock_fd, ctx.interface, 0);

exit:
  if (rb) ring_buffer__free(rb);
  if (skel) udp_monitor_bpf__destroy(skel);
  if (sock_fd >= 0) close(sock_fd);
  if (sock_raw_fd >= 0) close(sock_raw_fd);
  return 0;
}
