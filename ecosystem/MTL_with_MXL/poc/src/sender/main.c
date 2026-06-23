/* SPDX-License-Identifier: BSD-3-Clause
 * MTL-MXL POC — Host A: mtl_to_mxl_sender main
 *
 * Usage:
 *   mtl_to_mxl_sender \
 *     --port 0000:af:00.0 \
 *     --rx-ip 239.0.0.1 \
 *     --rx-udp-port 20000 \
 *     --width 1920 --height 1080 \
 *     --fps-num 30000 --fps-den 1001 \
 *     --mxl-domain /tmp/mxl \
 *     --flow-json ./config/v210_1080p30.json \
 *     --provider tcp \
 *     --local-ip 10.0.0.1 \
 *     --local-port 0 \
 *     --target-info "$(cat target_info.txt)" \
 *     --duration 60
 */

#include <arpa/inet.h>
#include <getopt.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "poc_frame_queue.h"
#include "poc_influxdb.h"
#include "poc_json_utils.h"
#include "poc_mtl_rx.h"
#include "poc_mxl_bridge.h"
#include "poc_stats.h"
#include "poc_types.h"
#include "poc_v210_utils.h"

static volatile bool g_running = true;

static void sig_handler(int sig) {
  (void)sig;
  g_running = false;
}

static void print_usage(const char* prog) {
  printf(
      "Usage: %s [options]\n"
      "  --port BDF            PCIe BDF for MTL NIC VF, e.g. 0000:31:01.0\n"
      "  --sip IP              Source IP for the DPDK port (e.g. 192.168.1.21)\n"
      "  --rx-ip IP            Multicast/unicast IP for ST2110-20 RX\n"
      "  --rx-udp-port PORT    UDP port for ST2110-20 RX\n"
      "  --payload-type PT     RTP payload type (default 112)\n"
      "  --width W             Video width (default 1920)\n"
      "  --height H            Video height (default 1080)\n"
      "  --fps-num N           FPS numerator (default 30000)\n"
      "  --fps-den D           FPS denominator (default 1001)\n"
      "  --mxl-domain PATH     MXL domain path (default /tmp/mxl)\n"
      "  --flow-json PATH      Path to v210 flow JSON\n"
      "  --provider NAME       Fabrics provider: tcp (default) or verbs\n"
      "  --local-ip IP         Local IP for fabrics endpoint\n"
      "  --local-port PORT     Local port for fabrics (default 0=ephemeral)\n"
      "  --target-info STR     Serialized targetInfo from Host B\n"
      "  --target-info-file F  File containing targetInfo string\n"
      "  --fb-count N          MTL frame buffer count (default 4)\n"
      "  --queue-depth N       Frame queue depth, power of 2 (default 16)\n"
      "  --accept-incomplete   Forward incomplete frames as invalid grains\n"
      "  --worker-cpu CPU      Pin bridge worker to CPU core (-1 = no pin)\n"
      "  --mtl-lcores LIST     Pin MTL/DPDK lcores, e.g. \"2,3\" (empty=auto)\n"
      "  --duration SEC        Run duration (0 = forever, default 0)\n"
      "  --redundancy          Enable ST 2022-7 dual-path\n"
      "  --port-r BDF          PCIe BDF for redundant DPDK port\n"
      "  --sip-r IP            Source IP for redundant port\n"
      "  --rx-ip-r IP          Multicast IP for R path\n"
      "  --rx-udp-port-r PORT  UDP port for R path\n"
      "  --help                Show this help\n",
      prog);
}

static int parse_ip(const char* str, uint8_t out[4]) {
  struct in_addr addr;
  if (inet_pton(AF_INET, str, &addr) != 1) return -1;
  memcpy(out, &addr, 4);
  return 0;
}

static void* bridge_thread_fn(void* arg) {
  poc_mxl_bridge_t* bridge = (poc_mxl_bridge_t*)arg;
  poc_mxl_bridge_run(bridge);
  return NULL;
}

int main(int argc, char** argv) {
  poc_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));

  /* Defaults */
  strncpy(cfg.mtl_port, "0000:31:01.0", sizeof(cfg.mtl_port) - 1);
  cfg.sip[0] = 192;
  cfg.sip[1] = 168;
  cfg.sip[2] = 1;
  cfg.sip[3] = 21;
  cfg.rx_ip[0] = 239;
  cfg.rx_ip[1] = 0;
  cfg.rx_ip[2] = 0;
  cfg.rx_ip[3] = 1;
  cfg.rx_udp_port = 20000;
  cfg.payload_type = 112;
  cfg.video.width = 3840;
  cfg.video.height = 2160;
  cfg.video.fps_num = 30000;
  cfg.video.fps_den = 1001;
  strncpy(cfg.mxl_domain, "/tmp/mxl", sizeof(cfg.mxl_domain) - 1);
  strncpy(cfg.fabrics_provider, "tcp", sizeof(cfg.fabrics_provider) - 1);
  strncpy(cfg.fabrics_local_ip, "0.0.0.0", sizeof(cfg.fabrics_local_ip) - 1);
  strncpy(cfg.fabrics_local_port, "0", sizeof(cfg.fabrics_local_port) - 1);
  cfg.framebuff_cnt = 8;
  cfg.queue_depth = 16;
  cfg.worker_cpu = -1;
  cfg.duration_sec = 0;

  char target_info_file[512] = {0};

  static struct option long_opts[] = {{"port", required_argument, NULL, 'b'},
                                      {"sip", required_argument, NULL, 's'},
                                      {"rx-ip", required_argument, NULL, 'r'},
                                      {"rx-udp-port", required_argument, NULL, 'u'},
                                      {"payload-type", required_argument, NULL, 'y'},
                                      {"width", required_argument, NULL, 'W'},
                                      {"height", required_argument, NULL, 'H'},
                                      {"fps-num", required_argument, NULL, 'N'},
                                      {"fps-den", required_argument, NULL, 'D'},
                                      {"mxl-domain", required_argument, NULL, 'd'},
                                      {"flow-json", required_argument, NULL, 'f'},
                                      {"provider", required_argument, NULL, 'p'},
                                      {"local-ip", required_argument, NULL, 'i'},
                                      {"local-port", required_argument, NULL, 'P'},
                                      {"target-info", required_argument, NULL, 'T'},
                                      {"target-info-file", required_argument, NULL, 'F'},
                                      {"fb-count", required_argument, NULL, 'c'},
                                      {"queue-depth", required_argument, NULL, 'q'},
                                      {"accept-incomplete", no_argument, NULL, 'I'},
                                      {"worker-cpu", required_argument, NULL, 'w'},
                                      {"mtl-lcores", required_argument, NULL, 'L'},
                                      {"duration", required_argument, NULL, 't'},
                                      {"redundancy", no_argument, NULL, 'R'},
                                      {"port-r", required_argument, NULL, 1001},
                                      {"sip-r", required_argument, NULL, 1002},
                                      {"rx-ip-r", required_argument, NULL, 1003},
                                      {"rx-udp-port-r", required_argument, NULL, 1004},
                                      {"help", no_argument, NULL, 'h'},
                                      {0, 0, 0, 0}};

  int opt;
  while ((opt = getopt_long(argc, argv, "b:s:r:u:y:W:H:N:D:d:f:p:i:P:T:F:c:q:Iw:L:t:Rh",
                            long_opts, NULL)) != -1) {
    switch (opt) {
      case 'b':
        strncpy(cfg.mtl_port, optarg, sizeof(cfg.mtl_port) - 1);
        break;
      case 's':
        parse_ip(optarg, cfg.sip);
        break;
      case 'r':
        parse_ip(optarg, cfg.rx_ip);
        break;
      case 'u':
        cfg.rx_udp_port = (uint16_t)atoi(optarg);
        break;
      case 'y':
        cfg.payload_type = (uint8_t)atoi(optarg);
        break;
      case 'W':
        cfg.video.width = (uint32_t)atoi(optarg);
        break;
      case 'H':
        cfg.video.height = (uint32_t)atoi(optarg);
        break;
      case 'N':
        cfg.video.fps_num = (uint32_t)atoi(optarg);
        break;
      case 'D':
        cfg.video.fps_den = (uint32_t)atoi(optarg);
        break;
      case 'd':
        strncpy(cfg.mxl_domain, optarg, sizeof(cfg.mxl_domain) - 1);
        break;
      case 'f':
        strncpy(cfg.flow_json_path, optarg, sizeof(cfg.flow_json_path) - 1);
        break;
      case 'p':
        strncpy(cfg.fabrics_provider, optarg, sizeof(cfg.fabrics_provider) - 1);
        break;
      case 'i':
        strncpy(cfg.fabrics_local_ip, optarg, sizeof(cfg.fabrics_local_ip) - 1);
        break;
      case 'P':
        strncpy(cfg.fabrics_local_port, optarg, sizeof(cfg.fabrics_local_port) - 1);
        break;
      case 'T':
        strncpy(cfg.target_info_str, optarg, sizeof(cfg.target_info_str) - 1);
        break;
      case 'F':
        strncpy(target_info_file, optarg, sizeof(target_info_file) - 1);
        break;
      case 'c':
        cfg.framebuff_cnt = (uint32_t)atoi(optarg);
        break;
      case 'q':
        cfg.queue_depth = (uint32_t)atoi(optarg);
        break;
      case 'I':
        cfg.accept_incomplete = true;
        break;
      case 'w':
        cfg.worker_cpu = atoi(optarg);
        break;
      case 'L':
        strncpy(cfg.mtl_lcores, optarg, sizeof(cfg.mtl_lcores) - 1);
        break;
      case 't':
        cfg.duration_sec = atoi(optarg);
        break;
      case 'R':
        cfg.redundancy = true;
        break;
      case 1001:
        strncpy(cfg.mtl_port_r, optarg, sizeof(cfg.mtl_port_r) - 1);
        break;
      case 1002:
        parse_ip(optarg, cfg.sip_r);
        break;
      case 1003:
        parse_ip(optarg, cfg.rx_ip_r);
        break;
      case 1004:
        cfg.rx_udp_port_r = (uint16_t)atoi(optarg);
        break;
      case 'h':
      default:
        print_usage(argv[0]);
        return (opt == 'h') ? 0 : 1;
    }
  }

  /* Load target info from file if specified */
  if (target_info_file[0] && !cfg.target_info_str[0]) {
    char* info = poc_load_file(target_info_file, NULL);
    if (info) {
      strncpy(cfg.target_info_str, info, sizeof(cfg.target_info_str) - 1);
      /* Strip trailing whitespace */
      size_t len = strlen(cfg.target_info_str);
      while (len > 0 && (cfg.target_info_str[len - 1] == '\n' ||
                         cfg.target_info_str[len - 1] == '\r' ||
                         cfg.target_info_str[len - 1] == ' '))
        cfg.target_info_str[--len] = '\0';
      free(info);
    } else {
      fprintf(stderr, "Failed to load target-info file: %s\n", target_info_file);
      return 1;
    }
  }

  /* Compute derived video params */
  cfg.video.v210_stride = poc_v210_line_stride(cfg.video.width);
  cfg.video.frame_size = poc_v210_frame_size(cfg.video.width, cfg.video.height);

  /* Validation */
  if (!cfg.flow_json_path[0]) {
    fprintf(stderr, "Error: --flow-json is required\n");
    print_usage(argv[0]);
    return 1;
  }
  if (!cfg.target_info_str[0]) {
    fprintf(stderr, "Error: --target-info or --target-info-file is required\n");
    print_usage(argv[0]);
    return 1;
  }

  signal(SIGINT, sig_handler);
  signal(SIGTERM, sig_handler);

  printf("══════════════════════════════════════════════\n");
  printf("  MTL-MXL POC — Host A Sender\n");
  printf("══════════════════════════════════════════════\n");
  printf("  MTL port:      %s\n", cfg.mtl_port);
  printf("  SIP:           %d.%d.%d.%d\n", cfg.sip[0], cfg.sip[1], cfg.sip[2],
         cfg.sip[3]);
  printf("  RX:            %d.%d.%d.%d:%u\n", cfg.rx_ip[0], cfg.rx_ip[1], cfg.rx_ip[2],
         cfg.rx_ip[3], cfg.rx_udp_port);
  if (cfg.redundancy) {
    printf("  RX (R):        %d.%d.%d.%d:%u  (ST 2022-7)\n", cfg.rx_ip_r[0],
           cfg.rx_ip_r[1], cfg.rx_ip_r[2], cfg.rx_ip_r[3], cfg.rx_udp_port_r);
    printf("  Port (R):      %s  SIP: %d.%d.%d.%d\n", cfg.mtl_port_r, cfg.sip_r[0],
           cfg.sip_r[1], cfg.sip_r[2], cfg.sip_r[3]);
  }
  printf("  Video:         %ux%u @ %u/%u fps, v210 stride=%u\n", cfg.video.width,
         cfg.video.height, cfg.video.fps_num, cfg.video.fps_den, cfg.video.v210_stride);
  printf("  MXL domain:    %s\n", cfg.mxl_domain);
  printf("  Flow JSON:     %s\n", cfg.flow_json_path);
  printf("  Provider:      %s\n", cfg.fabrics_provider);
  printf("  FB count:      %u, Queue depth: %u\n", cfg.framebuff_cnt, cfg.queue_depth);
  printf("  Duration:      %ds\n", cfg.duration_sec);
  printf("══════════════════════════════════════════════\n\n");

  /* ── Step 1: Create frame queue ── */
  poc_frame_queue_t queue;
  if (poc_queue_init(&queue, cfg.queue_depth) != 0) {
    fprintf(stderr, "Failed to init frame queue (depth=%u, must be power of 2)\n",
            cfg.queue_depth);
    return 1;
  }

  /* ── Step 2: Stats ── */
  poc_stats_t stats;
  poc_stats_reset(&stats);
  poc_influxdb_init();

  /* ── Step 3: Initialise MTL transport (DPDK init) ──
   * This creates the DPDK EAL and port but does NOT create
   * the ST20 RX session yet — we need MXL grain addresses first
   * for zero-copy ext_frame setup. */
  poc_mtl_rx_t mtl_rx;
  if (poc_mtl_rx_init_transport(&mtl_rx, &cfg) != 0) {
    fprintf(stderr, "Failed to initialise MTL transport\n");
    return 1;
  }

  /* ── Step 4: Initialise MXL Bridge ──
   * Creates MXL FlowWriter, pre-computes grain payload addresses,
   * DMA-maps them for NIC access, sets up Fabrics initiator.
   * Does NOT connect to target yet. */
  poc_mxl_bridge_t bridge;
  if (poc_mxl_bridge_init(&bridge, &cfg, mtl_rx.mtl, &queue, &stats, &g_running) != 0) {
    fprintf(stderr, "Failed to initialise MXL bridge\n");
    poc_mtl_rx_destroy(&mtl_rx);
    return 1;
  }

  /* ── Step 5: Create ST20 RX session with zero-copy ext_frame ──
   * Now that grain addresses are known and DMA-mapped, create the
   * RX session with query_ext_frame pointing to MXL grain buffers.
   * MTL will DMA received packets directly into grain payloads. */
  if (poc_mtl_rx_create_session(&mtl_rx, &cfg, &queue, &stats, &bridge,
                                poc_mxl_bridge_query_ext_frame) != 0) {
    fprintf(stderr, "Failed to create MTL RX session\n");
    poc_mxl_bridge_destroy(&bridge);
    poc_mtl_rx_destroy(&mtl_rx);
    return 1;
  }

  /* Set rx_handle on bridge (needed for st20_rx_put_framebuff) */
  bridge.rx_handle = mtl_rx.rx_handle;

  /* ── Step 6: Connect fabrics to remote target ── */
  if (poc_mxl_bridge_connect(&bridge, &cfg) != 0) {
    fprintf(stderr, "Failed to connect fabrics\n");
    poc_mxl_bridge_destroy(&bridge);
    poc_mtl_rx_destroy(&mtl_rx);
    return 1;
  }

  /* ── Step 7: Start bridge worker thread ── */
  pthread_t bridge_tid;
  if (pthread_create(&bridge_tid, NULL, bridge_thread_fn, &bridge) != 0) {
    perror("pthread_create bridge");
    poc_mxl_bridge_destroy(&bridge);
    poc_mtl_rx_destroy(&mtl_rx);
    return 1;
  }

  /* Optionally pin worker thread to physical core + HT sibling.
   * On this 2-socket Xeon (36 cores/socket, HT), sibling of
   * logical CPU N (N < 36) is N+72.  Pin both so no other
   * thread lands on the sibling and causes cache contention. */
  if (cfg.worker_cpu >= 0) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cfg.worker_cpu, &cpuset);
    int ht_sibling = cfg.worker_cpu + 72; /* works for NUMA 0 cores 0-35 */
    if (ht_sibling < 144) {
      CPU_SET(ht_sibling, &cpuset);
    }
    pthread_setaffinity_np(bridge_tid, sizeof(cpuset), &cpuset);
    printf("[MAIN] Bridge worker pinned to CPU %d (+HT %d)\n", cfg.worker_cpu,
           ht_sibling);
  }

  /* ── Main loop: periodic stats + wait for signal or duration ── */
  time_t start = time(NULL);
  uint64_t last_report_ms = 0;
  while (g_running) {
    usleep(500000); /* 500ms — fine granularity for stats, coarse enough to idle */
    if (cfg.duration_sec > 0 && (time(NULL) - start) >= cfg.duration_sec) {
      printf("[MAIN] Duration reached (%ds), shutting down\n", cfg.duration_sec);
      g_running = false;
    }
    /* Stats reporting moved here from bridge worker so the
     * synchronous InfluxDB POST cannot stall RDMA progress */
    struct timespec ts_now;
    clock_gettime(CLOCK_MONOTONIC, &ts_now);
    uint64_t now_ms = (uint64_t)ts_now.tv_sec * 1000 + ts_now.tv_nsec / 1000000;
    if (now_ms - last_report_ms >= 2000) {
      poc_stats_report(&stats, "sender");
      last_report_ms = now_ms;
    }
  }

  /* ── Cleanup ── */
  pthread_join(bridge_tid, NULL);

  printf("\n[MAIN] Final stats:\n");
  poc_stats_report(&stats, "sender");

  poc_influxdb_cleanup();
  poc_mxl_bridge_destroy(&bridge);
  poc_mtl_rx_destroy(&mtl_rx);

  printf("[MAIN] Clean shutdown complete\n");
  return 0;
}
