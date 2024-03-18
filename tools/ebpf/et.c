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
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <xdp/libxdp.h>
#include <xdp/xsk.h>

#include "fentry.skel.h"

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

static int et_xdp_loop(struct et_ctx* ctx) {
  int ret = 0;
  int if_cnt = ctx->xdp_if_cnt;

  if (if_cnt <= 0) {
    printf("please specify interfaces with --ifname <a,b,...>\n");
    return -EIO;
  }

  struct xdp_program* prog[if_cnt];

  /* load xdp program for each interface */
  for (int i = 0; i < if_cnt; i++) {
    prog[i] = xdp_program__open_file(ctx->xdp_path, "xdp", NULL);
    ret = libxdp_get_error(prog[i]);
    if (ret) {
      printf("xdp_program__open_file failed, please specify the right path\n");
      goto cleanup;
    }

    int ifindex = ctx->xdp_ifindex[i];
    ret = xdp_program__attach(prog[i], ifindex, XDP_MODE_NATIVE, 0);
    if (ret < 0) {
      printf("xdp_program__attach failed\n");
      goto cleanup;
    }
  }

  while (!stop) {
    sleep(1);
  }

cleanup:
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
      "  --prog xdp --ifname <name1,name2> --xdp_path /path/to/xdp.o       Load a custom "
      "XDP kernel program from the specified path and attach it to specified "
      "interfaces\n");

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