/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Intel Corporation
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
#include <sys/queue.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "log.h"
#include "udp_monitor.skel.h"

#ifndef NS_PER_S
#define NS_PER_S (1000000000)
#endif

struct udp_detect_entry {
  struct udp_pkt_tuple tuple; /* udp tuple identify */
  uint32_t pkt_cnt;
  uint64_t tx_bytes;
  bool sys; /* 224.0.1.129(ptp) or 255.255.255.255 */
  /* linked list */
  TAILQ_ENTRY(udp_detect_entry) next;
};

TAILQ_HEAD(udp_detect_list, udp_detect_entry);

struct udp_monitor_ctx {
  struct udp_detect_list detect;
  const char *interface;
  int dump_period_s;
  bool skip_sys;
  bool promisc;
};

enum um_args_cmd {
  UM_ARG_UNKNOWN = 0,
  UM_ARG_INTERFACE = 0x100, /* start from end of ascii */
  UM_ARG_DUMP_PERIOD_S,
  UM_ARG_NO_SKIP_SYS,
  UM_ARG_NO_PROMISC,
  UM_ARG_HELP,
};

static struct option um_args_options[] = {
    {"interface", required_argument, 0, UM_ARG_INTERFACE},
    {"dump_period_s", required_argument, 0, UM_ARG_DUMP_PERIOD_S},
    {"no_skip_sys", no_argument, 0, UM_ARG_NO_SKIP_SYS},
    {"no_promiscuous", no_argument, 0, UM_ARG_NO_PROMISC},
    {"help", no_argument, 0, UM_ARG_HELP},
    {0, 0, 0, 0}};

static inline void *um_zmalloc(size_t sz) {
  void *p = malloc(sz);
  if (p)
    memset(p, 0x0, sz);
  return p;
}

static inline uint64_t um_timespec_to_ns(const struct timespec *ts) {
  return ((uint64_t)ts->tv_sec * NS_PER_S) + ts->tv_nsec;
}

/* Monotonic time (in nanoseconds) since some unspecified starting point. */
static inline uint64_t um_get_monotonic_time() {
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
  return um_timespec_to_ns(&ts);
}

static void um_print_help() {
  printf("\n");
  printf("##### Usage: #####\n\n");

  printf(" Params:\n");
  printf("  --interface <if>         Set the network interface\n");
  printf("  --dump_period_s <sec>    Set the dump period\n");
  printf("  --no_skip_sys            Not skip the system packets like PTP\n");
  printf("  --no_promiscuous         Not enable promiscuous mode\n");
  printf("  --help                   Print help info\n");

  printf("\n");
}

static int um_parse_args(struct udp_monitor_ctx *ctx, int argc, char **argv) {
  int cmd = -1, opt_idx = 0;

  while (1) {
    cmd = getopt_long_only(argc, argv, "hv", um_args_options, &opt_idx);
    if (cmd == -1)
      break;

    switch (cmd) {
    case UM_ARG_INTERFACE:
      ctx->interface = optarg;
      break;
    case UM_ARG_DUMP_PERIOD_S:
      ctx->dump_period_s = atoi(optarg);
      break;
    case UM_ARG_NO_SKIP_SYS:
      ctx->skip_sys = false;
      break;
    case UM_ARG_NO_PROMISC:
      ctx->promisc = false;
      break;
    case UM_ARG_HELP:
    default:
      um_print_help();
      return -1;
    }
  }

  return 0;
}

static int udp_hdr_list_dump(struct udp_monitor_ctx *ctx, bool clear,
                             bool skip_sys, double dump_period_s) {
  struct udp_detect_list *list = &ctx->detect;
  struct udp_detect_entry *entry;

  TAILQ_FOREACH(entry, list, next) {
    if (!entry->pkt_cnt)
      continue;
    if (skip_sys && entry->sys)
      continue;

    uint8_t *dip = (uint8_t *)&entry->tuple.dst_ip;
    uint8_t *sip = (uint8_t *)&entry->tuple.src_ip;
    double rate_m = (double)entry->tx_bytes * 8 / dump_period_s / (1000 * 1000);

    info("%u.%u.%u.%u:%u -> %u.%u.%u.%u:%u, %f Mb/s pkts %u\n", sip[0], sip[1],
         sip[2], sip[3], ntohs(entry->tuple.src_port), dip[0], dip[1], dip[2],
         dip[3], ntohs(entry->tuple.dst_port), rate_m, entry->pkt_cnt);
    if (clear) {
      entry->pkt_cnt = 0;
      entry->tx_bytes = 0;
    }
  }

  return 0;
}

static int udp_hdr_entry_handler(void *pri, void *data, size_t data_sz) {
  struct udp_monitor_ctx *ctx = pri;
  const struct udp_pkt_entry *e = data;
  struct udp_detect_list *list = &ctx->detect;
  struct udp_detect_entry *entry;

  uint8_t *dip = (uint8_t *)&e->tuple.dst_ip;
  uint8_t *sip = (uint8_t *)&e->tuple.src_ip;
  dbg("%s, %u.%u.%u.%u:%u -> %u.%u.%u.%u:%u, len %u\n", __func__, sip[0],
      sip[1], sip[2], sip[3], ntohs(e->tuple.src_port), dip[0], dip[1], dip[2],
      dip[3], ntohs(e->tuple.dst_port), e->len);

  /* check if any exist */
  TAILQ_FOREACH(entry, list, next) {
    if (0 == memcmp(&e->tuple, &entry->tuple, sizeof(entry->tuple))) {
      entry->pkt_cnt++;
      entry->tx_bytes += e->len;
      return 0; /* found */
    }
  }

  entry = um_zmalloc(sizeof(*entry));
  if (!entry) {
    err("%s, entry malloc fail\n", __func__);
    return -ENOMEM;
  }
  memcpy(&entry->tuple, &e->tuple, sizeof(entry->tuple));
  entry->pkt_cnt++;
  entry->tx_bytes += e->len;
  /* 224.0.1.129 */
  if (dip[0] == 224 && dip[1] == 0 && dip[2] == 1 && dip[3] == 129)
    entry->sys = true;
  /* 255.255.255.255 */
  if (dip[0] == 255 && dip[1] == 255 && dip[2] == 255 && dip[3] == 255)
    entry->sys = true;
  /* add to list */
  TAILQ_INSERT_TAIL(list, entry, next);
  info("%s, new detected stream: %u.%u.%u.%u:%u -> %u.%u.%u.%u:%u, len %u\n",
       __func__, sip[0], sip[1], sip[2], sip[3], ntohs(e->tuple.src_port),
       dip[0], dip[1], dip[2], dip[3], ntohs(e->tuple.dst_port), e->len);

  return 0;
}

static int open_raw_sock(const char *if_name) {
  struct sockaddr_ll sll;
  int fd;
  int ret;

  fd = socket(PF_PACKET, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC,
              htons(ETH_P_ALL));
  if (fd < 0) {
    err("%s, failed to create raw socket\n", __func__);
    return -1;
  }

  memset(&sll, 0, sizeof(sll));
  sll.sll_family = AF_PACKET;
  sll.sll_ifindex = if_nametoindex(if_name);
  sll.sll_protocol = htons(ETH_P_ALL);
  ret = bind(fd, (struct sockaddr *)&sll, sizeof(sll));
  if (ret < 0) {
    err("%s, failed to bind to %s: %d, %s\n", __func__, if_name, ret,
        strerror(errno));
    close(fd);
    return ret;
  }

  return fd;
}

static int enable_promisc(int sock, const char *if_name, int enable) {
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

int main(int argc, char **argv) {
  struct udp_monitor_ctx ctx;
  int ret;
  int sock_raw_fd = -1;
  int sock_fd = -1;
  struct udp_monitor_bpf *skel = NULL;
  struct ring_buffer *rb = NULL;
  int prog_fd = -1;
  uint64_t last_ns, cur_ns, ns_diff;

  memset(&ctx, 0, sizeof(ctx));
  ctx.dump_period_s = 5;
  ctx.skip_sys = true;
  ctx.promisc = true;

  ret = um_parse_args(&ctx, argc, argv);
  if (ret < 0)
    return ret;
  if (!ctx.interface) {
    err("%s, no interface assigned\n", __func__);
    um_print_help();
    return -1;
  }

  TAILQ_INIT(&ctx.detect);

  /* Create raw socket */
  sock_raw_fd = open_raw_sock(ctx.interface);
  if (sock_raw_fd < 0) {
    err("%s, failed to open raw sock %d\n", __func__, sock_raw_fd);
    goto exit;
  }
  if (ctx.promisc) {
    sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_fd < 0) {
      err("%s, failed to open sock_fd for promisc %d\n", __func__, sock_fd);
      goto exit;
    }
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
  ret = setsockopt(sock_raw_fd, SOL_SOCKET, SO_ATTACH_BPF, &prog_fd,
                   sizeof(prog_fd));
  if (ret < 0) {
    err("%s, failed attach to raw socket %d\n", __func__, ret);
    goto exit;
  }
  info("%s, attach bpf skeleton to %s succ, sock_raw_fd %d\n", __func__,
       ctx.interface, sock_raw_fd);
  if (ctx.promisc) {
    ret = enable_promisc(sock_fd, ctx.interface, 1);
    if (ret < 0) {
      err("%s, failed to enable promisc %d\n", __func__, ret);
      goto exit;
    }
    info("%s, enable promisc for %s succ, sock_fd %d\n", __func__,
         ctx.interface, sock_fd);
  }

  signal(SIGINT, um_sig_handler);

  info("%s, start to poll udp pkts for %s, dump period %ds\n", __func__,
       ctx.interface, ctx.dump_period_s);
  last_ns = um_get_monotonic_time();
  while (!g_um_stop) {
    ret = ring_buffer__poll(rb, 100);
    dbg("%s, polling ret %d\n", __func__, ret);
    if (ret == -EINTR) {
      ret = 0;
      break;
    }
    if (ret < 0) {
      err("%s, polling fail\n", __func__);
      break;
    }
    cur_ns = um_get_monotonic_time();
    ns_diff = cur_ns - last_ns;
    if (ns_diff > ((uint64_t)ctx.dump_period_s * NS_PER_S)) {
      double dump_period_s = (double)ns_diff / NS_PER_S;
      /* report status now */
      info("\n----- DUMP UDP STAT EVERY %ds -----\n", ctx.dump_period_s);
      udp_hdr_list_dump(&ctx, true, ctx.skip_sys, dump_period_s);
      last_ns = cur_ns;
    }
  }

  info("%s, stop now\n", __func__);
  if (sock_fd >= 0)
    enable_promisc(sock_fd, ctx.interface, 0);

exit:
  if (rb)
    ring_buffer__free(rb);
  if (skel)
    udp_monitor_bpf__destroy(skel);
  if (sock_fd >= 0)
    close(sock_fd);
  if (sock_raw_fd >= 0)
    close(sock_raw_fd);

  /* free all entries in list */
  struct udp_detect_entry *entry;
  while ((entry = TAILQ_FIRST(&ctx.detect))) {
    TAILQ_REMOVE(&ctx.detect, entry, next);
    free(entry);
  }
  return 0;
}
