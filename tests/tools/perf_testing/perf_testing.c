/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Runs N identical st20p RX sessions, with optional TX loopback sessions on a second
 * port, or N TX sessions only, and prints the mtl_sch_get_stats() timing plus the
 * session counters at a fixed interval, as CSV or JSON lines.
 */

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <mtl/mtl_api.h>
#include <mtl/st20_api.h>
#include <mtl/st_pipeline_api.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define PERF_PAYLOAD_TYPE (112)
#define PERF_FRAMEBUFF_CNT (3)
#define PERF_NS_PER_S (1000000000ULL)
#define PERF_RX_TASKLET_NAME "rvs_pkt_rx"

struct perf_cfg {
  char p_port[MTL_PORT_MAX_LEN];
  char t_port[MTL_PORT_MAX_LEN];
  uint8_t p_sip[MTL_IP_ADDR_LEN];
  uint8_t t_sip[MTL_IP_ADDR_LEN];
  uint8_t rx_ip[MTL_IP_ADDR_LEN];
  uint16_t udp_port;
  int sessions;
  uint32_t width;
  uint32_t height;
  enum st_fps fps;
  enum st20_fmt fmt;
  bool tx;
  bool tx_only;
  enum st20_packing packing;
  bool has_dst_mac;
  uint8_t dst_mac[MTL_MAC_ADDR_LEN];
  uint32_t sch_quota;
  bool hdr_split;
  char dma[MTL_DMA_DEV_MAX][MTL_PORT_MAX_LEN];
  uint8_t num_dma;
  bool time_measure;
  int interval_s;
  int duration_s;
  bool json;
  bool reset_each_interval;
  char* lcores;
  enum mtl_log_level log_level;
};

struct perf_session {
  int idx;
  st20p_rx_handle rx;
  st20p_tx_handle tx;
  pthread_t rx_thread;
  pthread_t tx_thread;
  bool rx_thread_up;
  bool tx_thread_up;
  volatile bool stop;
  uint64_t rx_frames;
  uint64_t tx_frames;
};

struct perf_sch_sample {
  bool valid;
  struct mtl_sch_stats stats;
};

static volatile sig_atomic_t g_stop;

static void perf_on_signal(int sig) {
  (void)sig;
  g_stop = 1;
}

static uint64_t perf_now_ns(clockid_t clock) {
  struct timespec ts;
  clock_gettime(clock, &ts);
  return (uint64_t)ts.tv_sec * PERF_NS_PER_S + ts.tv_nsec;
}

static void perf_usage(const char* prog) {
  printf(
      "Usage: %s --p_port <pci> --p_sip <ip> [options]\n"
      "  --p_port <pci>        RX port; the TX port with --tx_only\n"
      "  --p_sip <ip>          IP of --p_port\n"
      "  --rx_ip <ip>          Multicast group, or the sender IP of a unicast stream;\n"
      "                        with --tx a unicast stream runs t_sip -> p_sip;\n"
      "                        the destination IP with --tx_only [239.168.85.20]\n"
      "  --udp_port <n>        UDP port of session 0, session i uses n + i [20000]\n"
      "  --sessions <n>        RX sessions, and TX sessions with --tx;\n"
      "                        only TX sessions with --tx_only [1]\n"
      "  --width <n>           [1920]\n"
      "  --height <n>          [1080]\n"
      "  --fps <p59|p50|p29|p25|p60|p30>  [p59]\n"
      "  --fmt <st20 fmt>      Transport and output format [YUV_422_10bit]\n"
      "  --tx                  Also create TX sessions on --t_port\n"
      "  --t_port <pci>        TX port, with --tx\n"
      "  --t_sip <ip>          TX port IP, with --tx\n"
      "  --tx_only             Only TX sessions on --p_port, sent to --rx_ip;\n"
      "                        excludes --tx, --hdr_split and --dma\n"
      "  --packing <bpm|gpm|gpm_sl>  TX packing mode [bpm]\n"
      "  --tx_dst_mac <mac>    TX destination MAC, skips ARP (aa:bb:cc:dd:ee:ff)\n"
      "  --sch_quota <mbs>     data_quota_mbs_per_sch in Mbit/s, 0 keeps the default\n"
      "  --hdr_split           Request header split RX queues\n"
      "  --dma <pci>           DMA device, repeat for more\n"
      "  --time_measure        Set MTL_FLAG_TASKLET_TIME_MEASURE\n"
      "  --interval <s>        Sample interval [5]\n"
      "  --duration <s>        Run time, 0 runs until SIGINT [30]\n"
      "  --json                One JSON object per interval; default is CSV\n"
      "  --reset_each_interval Reset the scheduler counters before the first interval\n"
      "                        and after each sample\n"
      "  --lcores <list>       Lcore list for MTL, e.g. 4,5,6\n"
      "  --log_level <debug|info|notice|warning|error>  [warning]\n",
      prog);
}

static int perf_parse_ip(const char* s, uint8_t* ip) {
  if (inet_pton(AF_INET, s, ip) != 1) {
    fprintf(stderr, "invalid ip %s\n", s);
    return -EINVAL;
  }
  return 0;
}

static int perf_parse_fps(const char* s, enum st_fps* fps) {
  static const struct {
    const char* name;
    enum st_fps fps;
  } table[] = {
      {"p59", ST_FPS_P59_94}, {"p50", ST_FPS_P50}, {"p29", ST_FPS_P29_97},
      {"p25", ST_FPS_P25},    {"p60", ST_FPS_P60}, {"p30", ST_FPS_P30},
  };

  for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
    if (!strcmp(s, table[i].name)) {
      *fps = table[i].fps;
      return 0;
    }
  }
  fprintf(stderr, "invalid fps %s\n", s);
  return -EINVAL;
}

/* Accepts both the short form "YUV_422_10bit" and the full "ST20_FMT_YUV_422_10BIT". */
static int perf_parse_fmt(const char* s, enum st20_fmt* fmt) {
  char full[64];

  snprintf(full, sizeof(full), "ST20_FMT_%s", s);
  for (char* c = full; *c; c++) *c = toupper((unsigned char)*c);

  *fmt = st20_name_to_fmt(s);
  if (*fmt == ST20_FMT_MAX) *fmt = st20_name_to_fmt(full);
  if (*fmt == ST20_FMT_MAX) {
    fprintf(stderr, "invalid fmt %s\n", s);
    return -EINVAL;
  }
  return 0;
}

static int perf_parse_packing(const char* s, enum st20_packing* packing) {
  if (!strcmp(s, "bpm"))
    *packing = ST20_PACKING_BPM;
  else if (!strcmp(s, "gpm"))
    *packing = ST20_PACKING_GPM;
  else if (!strcmp(s, "gpm_sl"))
    *packing = ST20_PACKING_GPM_SL;
  else {
    fprintf(stderr, "invalid packing %s\n", s);
    return -EINVAL;
  }
  return 0;
}

static int perf_parse_mac(const char* s, uint8_t* mac) {
  unsigned int m[MTL_MAC_ADDR_LEN];
  char tail;

  if (sscanf(s, "%x:%x:%x:%x:%x:%x%c", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5], &tail) !=
      MTL_MAC_ADDR_LEN) {
    fprintf(stderr, "invalid mac %s\n", s);
    return -EINVAL;
  }
  for (int i = 0; i < MTL_MAC_ADDR_LEN; i++) {
    if (m[i] > 0xFF) {
      fprintf(stderr, "invalid mac %s\n", s);
      return -EINVAL;
    }
    mac[i] = (uint8_t)m[i];
  }
  return 0;
}

static int perf_parse_log_level(const char* s, enum mtl_log_level* level) {
  if (!strcmp(s, "debug"))
    *level = MTL_LOG_LEVEL_DEBUG;
  else if (!strcmp(s, "info"))
    *level = MTL_LOG_LEVEL_INFO;
  else if (!strcmp(s, "notice"))
    *level = MTL_LOG_LEVEL_NOTICE;
  else if (!strcmp(s, "warning"))
    *level = MTL_LOG_LEVEL_WARNING;
  else if (!strcmp(s, "error"))
    *level = MTL_LOG_LEVEL_ERR;
  else {
    fprintf(stderr, "invalid log_level %s\n", s);
    return -EINVAL;
  }
  return 0;
}

enum perf_opt {
  OPT_P_PORT = 0x100,
  OPT_P_SIP,
  OPT_RX_IP,
  OPT_UDP_PORT,
  OPT_SESSIONS,
  OPT_WIDTH,
  OPT_HEIGHT,
  OPT_FPS,
  OPT_FMT,
  OPT_TX,
  OPT_T_PORT,
  OPT_T_SIP,
  OPT_TX_ONLY,
  OPT_PACKING,
  OPT_TX_DST_MAC,
  OPT_SCH_QUOTA,
  OPT_HDR_SPLIT,
  OPT_DMA,
  OPT_TIME_MEASURE,
  OPT_INTERVAL,
  OPT_DURATION,
  OPT_JSON,
  OPT_RESET_EACH_INTERVAL,
  OPT_LCORES,
  OPT_LOG_LEVEL,
  OPT_HELP,
};

static int perf_parse_args(struct perf_cfg* cfg, int argc, char** argv) {
  static const struct option options[] = {
      {"p_port", required_argument, 0, OPT_P_PORT},
      {"p_sip", required_argument, 0, OPT_P_SIP},
      {"rx_ip", required_argument, 0, OPT_RX_IP},
      {"udp_port", required_argument, 0, OPT_UDP_PORT},
      {"sessions", required_argument, 0, OPT_SESSIONS},
      {"width", required_argument, 0, OPT_WIDTH},
      {"height", required_argument, 0, OPT_HEIGHT},
      {"fps", required_argument, 0, OPT_FPS},
      {"fmt", required_argument, 0, OPT_FMT},
      {"tx", no_argument, 0, OPT_TX},
      {"t_port", required_argument, 0, OPT_T_PORT},
      {"t_sip", required_argument, 0, OPT_T_SIP},
      {"tx_only", no_argument, 0, OPT_TX_ONLY},
      {"packing", required_argument, 0, OPT_PACKING},
      {"tx_dst_mac", required_argument, 0, OPT_TX_DST_MAC},
      {"sch_quota", required_argument, 0, OPT_SCH_QUOTA},
      {"hdr_split", no_argument, 0, OPT_HDR_SPLIT},
      {"dma", required_argument, 0, OPT_DMA},
      {"time_measure", no_argument, 0, OPT_TIME_MEASURE},
      {"interval", required_argument, 0, OPT_INTERVAL},
      {"duration", required_argument, 0, OPT_DURATION},
      {"json", no_argument, 0, OPT_JSON},
      {"reset_each_interval", no_argument, 0, OPT_RESET_EACH_INTERVAL},
      {"lcores", required_argument, 0, OPT_LCORES},
      {"log_level", required_argument, 0, OPT_LOG_LEVEL},
      {"help", no_argument, 0, OPT_HELP},
      {0, 0, 0, 0},
  };
  bool has_p_port = false, has_p_sip = false, has_t_port = false, has_t_sip = false;
  int ret = 0;

  memset(cfg, 0, sizeof(*cfg));
  perf_parse_ip("239.168.85.20", cfg->rx_ip);
  cfg->udp_port = 20000;
  cfg->sessions = 1;
  cfg->width = 1920;
  cfg->height = 1080;
  cfg->fps = ST_FPS_P59_94;
  cfg->fmt = ST20_FMT_YUV_422_10BIT;
  cfg->interval_s = 5;
  cfg->duration_s = 30;
  cfg->log_level = MTL_LOG_LEVEL_WARNING;

  while (!ret) {
    int opt = getopt_long(argc, argv, "", options, NULL);
    if (opt == -1) break;
    switch (opt) {
      case OPT_P_PORT:
        snprintf(cfg->p_port, sizeof(cfg->p_port), "%s", optarg);
        has_p_port = true;
        break;
      case OPT_P_SIP:
        ret = perf_parse_ip(optarg, cfg->p_sip);
        has_p_sip = true;
        break;
      case OPT_RX_IP:
        ret = perf_parse_ip(optarg, cfg->rx_ip);
        break;
      case OPT_UDP_PORT:
        cfg->udp_port = atoi(optarg);
        break;
      case OPT_SESSIONS:
        cfg->sessions = atoi(optarg);
        break;
      case OPT_WIDTH:
        cfg->width = atoi(optarg);
        break;
      case OPT_HEIGHT:
        cfg->height = atoi(optarg);
        break;
      case OPT_FPS:
        ret = perf_parse_fps(optarg, &cfg->fps);
        break;
      case OPT_FMT:
        ret = perf_parse_fmt(optarg, &cfg->fmt);
        break;
      case OPT_TX:
        cfg->tx = true;
        break;
      case OPT_T_PORT:
        snprintf(cfg->t_port, sizeof(cfg->t_port), "%s", optarg);
        has_t_port = true;
        break;
      case OPT_T_SIP:
        ret = perf_parse_ip(optarg, cfg->t_sip);
        has_t_sip = true;
        break;
      case OPT_TX_ONLY:
        cfg->tx_only = true;
        break;
      case OPT_PACKING:
        ret = perf_parse_packing(optarg, &cfg->packing);
        break;
      case OPT_TX_DST_MAC:
        ret = perf_parse_mac(optarg, cfg->dst_mac);
        cfg->has_dst_mac = true;
        break;
      case OPT_SCH_QUOTA:
        cfg->sch_quota = (uint32_t)atoi(optarg);
        break;
      case OPT_HDR_SPLIT:
        cfg->hdr_split = true;
        break;
      case OPT_DMA:
        if (cfg->num_dma >= MTL_DMA_DEV_MAX) {
          fprintf(stderr, "too many --dma, max %d\n", MTL_DMA_DEV_MAX);
          ret = -EINVAL;
          break;
        }
        snprintf(cfg->dma[cfg->num_dma], MTL_PORT_MAX_LEN, "%s", optarg);
        cfg->num_dma++;
        break;
      case OPT_TIME_MEASURE:
        cfg->time_measure = true;
        break;
      case OPT_INTERVAL:
        cfg->interval_s = atoi(optarg);
        break;
      case OPT_DURATION:
        cfg->duration_s = atoi(optarg);
        break;
      case OPT_JSON:
        cfg->json = true;
        break;
      case OPT_RESET_EACH_INTERVAL:
        cfg->reset_each_interval = true;
        break;
      case OPT_LCORES:
        cfg->lcores = optarg;
        break;
      case OPT_LOG_LEVEL:
        ret = perf_parse_log_level(optarg, &cfg->log_level);
        break;
      case OPT_HELP:
        perf_usage(argv[0]);
        exit(0);
      default:
        perf_usage(argv[0]);
        ret = -EINVAL;
        break;
    }
  }
  if (ret) return ret;

  if (!has_p_port || !has_p_sip) {
    fprintf(stderr, "--p_port and --p_sip are required\n");
    return -EINVAL;
  }
  if (cfg->tx && (!has_t_port || !has_t_sip)) {
    fprintf(stderr, "--tx needs --t_port and --t_sip\n");
    return -EINVAL;
  }
  if (cfg->tx && cfg->tx_only) {
    fprintf(stderr, "--tx and --tx_only exclude each other\n");
    return -EINVAL;
  }
  if (cfg->tx_only && (cfg->hdr_split || cfg->num_dma)) {
    fprintf(stderr, "--hdr_split and --dma are RX options, not for --tx_only\n");
    return -EINVAL;
  }
  if (cfg->sessions < 1 || cfg->interval_s < 1 || cfg->duration_s < 0) {
    fprintf(stderr, "sessions and interval must be >= 1, duration >= 0\n");
    return -EINVAL;
  }
  if (cfg->udp_port + cfg->sessions - 1 > UINT16_MAX) {
    fprintf(stderr, "udp_port + sessions exceeds %u\n", UINT16_MAX);
    return -EINVAL;
  }
  return 0;
}

static bool perf_ip_is_mcast(const uint8_t* ip) {
  return (ip[0] & 0xF0) == 0xE0;
}

static void* perf_rx_thread(void* arg) {
  struct perf_session* s = arg;

  while (!s->stop) {
    struct st_frame* frame = st20p_rx_get_frame(s->rx);
    if (!frame) continue;
    __atomic_store_n(&s->rx_frames, s->rx_frames + 1, __ATOMIC_RELAXED);
    st20p_rx_put_frame(s->rx, frame);
  }
  return NULL;
}

static void* perf_tx_thread(void* arg) {
  struct perf_session* s = arg;

  while (!s->stop) {
    struct st_frame* frame = st20p_tx_get_frame(s->tx);
    if (!frame) continue;
    __atomic_store_n(&s->tx_frames, s->tx_frames + 1, __ATOMIC_RELAXED);
    st20p_tx_put_frame(s->tx, frame);
  }
  return NULL;
}

static int perf_session_create(struct perf_cfg* cfg, mtl_handle mt,
                               struct perf_session* s) {
  struct st20p_rx_ops rx_ops;
  char name[32];
  bool mcast = perf_ip_is_mcast(cfg->rx_ip);
  /* a unicast RX session filters on the sender IP, which is the TX port with --tx */
  const uint8_t* rx_src = (mcast || !cfg->tx) ? cfg->rx_ip : cfg->t_sip;
  /* --tx_only sends to --rx_ip, the IP of the RX process */
  const uint8_t* tx_dst = (mcast || cfg->tx_only) ? cfg->rx_ip : cfg->p_sip;
  const char* tx_port = cfg->tx_only ? cfg->p_port : cfg->t_port;

  memset(&rx_ops, 0, sizeof(rx_ops));
  snprintf(name, sizeof(name), "perf_rx_%d", s->idx);
  rx_ops.name = name;
  rx_ops.priv = s;
  rx_ops.port.num_port = 1;
  memcpy(rx_ops.port.ip_addr[MTL_SESSION_PORT_P], rx_src, MTL_IP_ADDR_LEN);
  snprintf(rx_ops.port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s", cfg->p_port);
  rx_ops.port.udp_port[MTL_SESSION_PORT_P] = cfg->udp_port + s->idx;
  rx_ops.port.payload_type = PERF_PAYLOAD_TYPE;
  rx_ops.width = cfg->width;
  rx_ops.height = cfg->height;
  rx_ops.fps = cfg->fps;
  rx_ops.transport_fmt = cfg->fmt;
  rx_ops.output_fmt = st_frame_fmt_from_transport(cfg->fmt);
  rx_ops.device = ST_PLUGIN_DEVICE_AUTO;
  rx_ops.framebuff_cnt = PERF_FRAMEBUFF_CNT;
  rx_ops.flags = ST20P_RX_FLAG_BLOCK_GET;
  if (cfg->num_dma) rx_ops.flags |= ST20P_RX_FLAG_DMA_OFFLOAD;
  if (cfg->hdr_split) rx_ops.flags |= ST20P_RX_FLAG_HDR_SPLIT;

  if (!cfg->tx_only) {
    s->rx = st20p_rx_create(mt, &rx_ops);
    if (!s->rx) {
      fprintf(stderr, "st20p_rx_create(%d) fail\n", s->idx);
      return -EIO;
    }
  }

  if (!cfg->tx && !cfg->tx_only) return 0;

  struct st20p_tx_ops tx_ops;
  memset(&tx_ops, 0, sizeof(tx_ops));
  snprintf(name, sizeof(name), "perf_tx_%d", s->idx);
  tx_ops.name = name;
  tx_ops.priv = s;
  tx_ops.port.num_port = 1;
  memcpy(tx_ops.port.dip_addr[MTL_SESSION_PORT_P], tx_dst, MTL_IP_ADDR_LEN);
  snprintf(tx_ops.port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s", tx_port);
  tx_ops.port.udp_port[MTL_SESSION_PORT_P] = cfg->udp_port + s->idx;
  tx_ops.port.payload_type = PERF_PAYLOAD_TYPE;
  tx_ops.width = cfg->width;
  tx_ops.height = cfg->height;
  tx_ops.fps = cfg->fps;
  tx_ops.transport_fmt = cfg->fmt;
  tx_ops.input_fmt = st_frame_fmt_from_transport(cfg->fmt);
  tx_ops.device = ST_PLUGIN_DEVICE_AUTO;
  tx_ops.framebuff_cnt = PERF_FRAMEBUFF_CNT;
  tx_ops.transport_packing = cfg->packing;
  tx_ops.flags = ST20P_TX_FLAG_BLOCK_GET;
  if (cfg->has_dst_mac) {
    memcpy(tx_ops.tx_dst_mac[MTL_SESSION_PORT_P], cfg->dst_mac, MTL_MAC_ADDR_LEN);
    tx_ops.flags |= ST20P_TX_FLAG_USER_P_MAC;
  }

  s->tx = st20p_tx_create(mt, &tx_ops);
  if (!s->tx) {
    fprintf(stderr, "st20p_tx_create(%d) fail\n", s->idx);
    return -EIO;
  }
  return 0;
}

static int perf_session_start(struct perf_session* s) {
  if (s->rx) {
    if (pthread_create(&s->rx_thread, NULL, perf_rx_thread, s)) return -EIO;
    s->rx_thread_up = true;
  }
  if (!s->tx) return 0;
  if (pthread_create(&s->tx_thread, NULL, perf_tx_thread, s)) return -EIO;
  s->tx_thread_up = true;
  return 0;
}

static void perf_session_stop(struct perf_session* s) {
  s->stop = true;
  if (s->rx) st20p_rx_wake_block(s->rx);
  if (s->tx) st20p_tx_wake_block(s->tx);
  if (s->rx_thread_up) pthread_join(s->rx_thread, NULL);
  if (s->tx_thread_up) pthread_join(s->tx_thread, NULL);
  s->rx_thread_up = false;
  s->tx_thread_up = false;
}

static void perf_session_free(struct perf_session* s) {
  if (s->tx) st20p_tx_free(s->tx);
  if (s->rx) st20p_rx_free(s->rx);
  s->tx = NULL;
  s->rx = NULL;
}

static void perf_time_delta(const struct mtl_time_stats* now,
                            const struct mtl_time_stats* prev, struct mtl_time_stats* d) {
  d->cnt = now->cnt - prev->cnt;
  d->sum_ns = now->sum_ns - prev->sum_ns;
  /* min and max are since the last reset, the API has no per-interval extremes */
  d->min_ns = now->min_ns;
  d->max_ns = now->max_ns;
}

static uint64_t perf_avg_ns(const struct mtl_time_stats* d) {
  return d->cnt ? d->sum_ns / d->cnt : 0;
}

static double perf_busy_pct(uint64_t sum_ns, uint64_t interval_ns) {
  return interval_ns ? 100.0 * (double)sum_ns / (double)interval_ns : 0;
}

static void perf_print_time_csv(double ts, const struct mtl_sch_stats* st, int sch_idx,
                                int tasklet_idx, const char* kind, const char* name,
                                const struct mtl_time_stats* d,
                                const struct mtl_time_stats* w, uint64_t interval_ns) {
  printf("%.3f,%s,%d,%s,%d,%d,%d,%s,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
         ",%.2f,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%.2f\n",
         ts, kind, sch_idx, st->name, (int)st->lcore, st->time_measure_enabled,
         tasklet_idx, name, d->cnt, perf_avg_ns(d), d->min_ns, d->max_ns,
         perf_busy_pct(d->sum_ns, interval_ns), st->avg_ns_per_loop, w->cnt,
         perf_avg_ns(w), perf_busy_pct(w->sum_ns, interval_ns));
}

static void perf_print_time_json(const struct mtl_time_stats* d, uint64_t interval_ns) {
  printf("{\"cnt\":%" PRIu64 ",\"avg_ns\":%" PRIu64 ",\"min_ns\":%" PRIu64
         ",\"max_ns\":%" PRIu64 ",\"busy_pct\":%.2f}",
         d->cnt, perf_avg_ns(d), d->min_ns, d->max_ns,
         perf_busy_pct(d->sum_ns, interval_ns));
}

static void perf_print_sch(struct perf_cfg* cfg, double ts, int sch_idx,
                           const struct mtl_sch_stats* st, struct perf_sch_sample* prev,
                           uint64_t interval_ns, bool* first, uint64_t* rx_work_ns) {
  static const struct mtl_time_stats zero = {0};
  struct mtl_time_stats d, w;

  bool same = prev->valid && st->loop_time.cnt >= prev->stats.loop_time.cnt;
  perf_time_delta(&st->loop_time, same ? &prev->stats.loop_time : &zero, &d);
  if (cfg->json) {
    printf(
        "%s{\"idx\":%d,\"name\":\"%s\",\"lcore\":%d,\"time_measure\":%s,"
        "\"avg_ns_per_loop\":%" PRIu64 ",\"loop\":",
        *first ? "" : ",", sch_idx, st->name, (int)st->lcore,
        st->time_measure_enabled ? "true" : "false", st->avg_ns_per_loop);
    perf_print_time_json(&d, interval_ns);
    printf(",\"tasklets\":[");
  } else {
    perf_print_time_csv(ts, st, sch_idx, -1, "loop", "loop", &d, &zero, interval_ns);
  }
  *first = false;

  for (int i = 0; i < st->nb_tasklets; i++) {
    const struct mtl_sch_tasklet_stats* t = &st->tasklets[i];
    /* a new name or a smaller count means a new tasklet in this slot: no delta base */
    same = prev->valid && i < prev->stats.nb_tasklets &&
           !strcmp(t->name, prev->stats.tasklets[i].name) &&
           t->time.cnt >= prev->stats.tasklets[i].time.cnt &&
           t->work.cnt >= prev->stats.tasklets[i].work.cnt;
    perf_time_delta(&t->time, same ? &prev->stats.tasklets[i].time : &zero, &d);
    perf_time_delta(&t->work, same ? &prev->stats.tasklets[i].work : &zero, &w);
    if (!strcmp(t->name, PERF_RX_TASKLET_NAME)) *rx_work_ns += w.sum_ns;
    if (cfg->json) {
      printf("%s{\"idx\":%d,\"name\":\"%s\",\"time\":", i ? "," : "", i, t->name);
      perf_print_time_json(&d, interval_ns);
      printf(",\"work\":");
      perf_print_time_json(&w, interval_ns);
      printf("}");
    } else {
      perf_print_time_csv(ts, st, sch_idx, i, "tasklet", t->name, &d, &w, interval_ns);
    }
  }
  if (cfg->json) printf("]}");
}

static uint64_t perf_rx_pkts(struct perf_session* s) {
  struct st20_rx_user_stats rx;

  memset(&rx, 0, sizeof(rx));
  if (s->rx) st20p_rx_get_session_stats(s->rx, &rx);
  return rx.common.stat_pkts_received;
}

static uint64_t perf_print_session(struct perf_cfg* cfg, double ts,
                                   struct perf_session* s, bool* first) {
  struct st20_rx_user_stats rx;
  struct st20_tx_user_stats tx;
  uint64_t rx_frames = __atomic_load_n(&s->rx_frames, __ATOMIC_RELAXED);
  uint64_t tx_frames = __atomic_load_n(&s->tx_frames, __ATOMIC_RELAXED);

  memset(&rx, 0, sizeof(rx));
  if (s->rx) {
    st20p_rx_get_session_stats(s->rx, &rx);
    if (cfg->json) {
      printf("%s{\"dir\":\"rx\",\"idx\":%d,\"frames\":%" PRIu64 ",\"pkts\":%" PRIu64
             ",\"lost\":%" PRIu64 ",\"unrecovered\":%" PRIu64 "}",
             *first ? "" : ",", s->idx, rx_frames, rx.common.stat_pkts_received,
             rx.common.stat_lost_packets, rx.common.stat_pkts_unrecovered);
    } else {
      printf("%.3f,session,rx,%d,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n", ts,
             s->idx, rx_frames, rx.common.stat_pkts_received, rx.common.stat_lost_packets,
             rx.common.stat_pkts_unrecovered);
    }
    *first = false;
  }

  if (!s->tx) return rx.common.stat_pkts_received;
  memset(&tx, 0, sizeof(tx));
  st20p_tx_get_session_stats(s->tx, &tx);
  if (cfg->json) {
    printf("%s{\"dir\":\"tx\",\"idx\":%d,\"frames\":%" PRIu64 ",\"pkts\":%" PRIu64
           ",\"lost\":0,\"unrecovered\":0}",
           *first ? "" : ",", s->idx, tx_frames,
           tx.common.port[MTL_SESSION_PORT_P].packets);
  } else {
    printf("%.3f,session,tx,%d,%" PRIu64 ",%" PRIu64 ",0,0\n", ts, s->idx, tx_frames,
           tx.common.port[MTL_SESSION_PORT_P].packets);
  }
  *first = false;
  return rx.common.stat_pkts_received;
}

static void perf_print_rx_cost(struct perf_cfg* cfg, double ts, uint64_t pkts,
                               uint64_t work_ns, uint64_t interval_ns) {
  double ns_per_pkt = pkts ? (double)work_ns / (double)pkts : 0;
  double busy = perf_busy_pct(work_ns, interval_ns);

  if (cfg->json) {
    printf(",\"rx_cost\":{\"pkts\":%" PRIu64 ",\"work_ns\":%" PRIu64
           ",\"ns_per_pkt\":%.2f,\"work_busy_pct\":%.2f}",
           pkts, work_ns, ns_per_pkt, busy);
  } else {
    printf("%.3f,rxcost,all,%" PRIu64 ",%" PRIu64 ",%.2f,%.2f\n", ts, pkts, work_ns,
           ns_per_pkt, busy);
  }
}

static void perf_sample(struct perf_cfg* cfg, mtl_handle mt,
                        struct perf_session* sessions, struct perf_sch_sample* prev,
                        int sch_count, uint64_t interval_ns, uint64_t* rx_pkts_prev) {
  double ts = (double)perf_now_ns(CLOCK_REALTIME) / (double)PERF_NS_PER_S;
  bool first = true;
  uint64_t rx_work_ns = 0, rx_pkts = 0;

  if (cfg->json)
    printf("{\"ts\":%.3f,\"interval_ns\":%" PRIu64 ",\"sch\":[", ts, interval_ns);

  for (int i = 0; i < sch_count; i++) {
    struct mtl_sch_stats st;
    int ret = mtl_sch_get_stats(mt, i, &st);
    if (ret == -ENOENT) {
      prev[i].valid = false;
      continue;
    }
    if (ret < 0) {
      fprintf(stderr, "mtl_sch_get_stats(%d) fail %d\n", i, ret);
      continue;
    }
    perf_print_sch(cfg, ts, i, &st, &prev[i], interval_ns, &first, &rx_work_ns);
    if (cfg->reset_each_interval) {
      mtl_sch_reset_stats(mt, i);
      prev[i].valid = false;
    } else {
      prev[i].stats = st;
      prev[i].valid = true;
    }
  }

  first = true;
  if (cfg->json) printf("],\"sessions\":[");
  for (int i = 0; i < cfg->sessions; i++)
    rx_pkts += perf_print_session(cfg, ts, &sessions[i], &first);
  if (cfg->json) printf("]");
  uint64_t pkts = rx_pkts >= *rx_pkts_prev ? rx_pkts - *rx_pkts_prev : 0;
  if (!cfg->tx_only) perf_print_rx_cost(cfg, ts, pkts, rx_work_ns, interval_ns);
  *rx_pkts_prev = rx_pkts;
  if (cfg->json) printf("}\n");
  fflush(stdout);
}

static void perf_sleep_until(uint64_t deadline_ns) {
  while (!g_stop) {
    uint64_t now = perf_now_ns(CLOCK_MONOTONIC);
    if (now >= deadline_ns) return;
    uint64_t left = deadline_ns - now;
    usleep(left > 100000000ULL ? 100000 : (useconds_t)(left / 1000));
  }
}

int main(int argc, char** argv) {
  struct perf_cfg cfg;
  struct mtl_init_params p;
  struct perf_session* sessions = NULL;
  struct perf_sch_sample* prev = NULL;
  mtl_handle mt = NULL;
  int ret, sch_count = 0, created = 0;
  uint64_t rx_pkts_prev = 0;

  ret = perf_parse_args(&cfg, argc, argv);
  if (ret < 0) return 1;

  memset(&p, 0, sizeof(p));
  snprintf(p.port[MTL_PORT_P], MTL_PORT_MAX_LEN, "%s", cfg.p_port);
  memcpy(p.sip_addr[MTL_PORT_P], cfg.p_sip, MTL_IP_ADDR_LEN);
  p.pmd[MTL_PORT_P] = MTL_PMD_DPDK_USER;
  p.rx_queues_cnt[MTL_PORT_P] = cfg.tx_only ? 0 : cfg.sessions;
  if (cfg.tx_only) p.tx_queues_cnt[MTL_PORT_P] = cfg.sessions;
  p.num_ports = 1;
  if (cfg.tx) {
    snprintf(p.port[MTL_PORT_R], MTL_PORT_MAX_LEN, "%s", cfg.t_port);
    memcpy(p.sip_addr[MTL_PORT_R], cfg.t_sip, MTL_IP_ADDR_LEN);
    p.pmd[MTL_PORT_R] = MTL_PMD_DPDK_USER;
    p.tx_queues_cnt[MTL_PORT_R] = cfg.sessions;
    p.num_ports = 2;
  }
  if (cfg.hdr_split) p.nb_rx_hdr_split_queues = cfg.sessions;
  for (int i = 0; i < cfg.num_dma; i++)
    snprintf(p.dma_dev_port[i], MTL_PORT_MAX_LEN, "%s", cfg.dma[i]);
  p.num_dma_dev_port = cfg.num_dma;
  p.data_quota_mbs_per_sch = cfg.sch_quota;
  p.flags = MTL_FLAG_BIND_NUMA;
  if (cfg.time_measure) p.flags |= MTL_FLAG_TASKLET_TIME_MEASURE;
  p.log_level = cfg.log_level;
  p.lcores = cfg.lcores;

  signal(SIGINT, perf_on_signal);
  signal(SIGTERM, perf_on_signal);

  mt = mtl_init(&p);
  if (!mt) {
    fprintf(stderr, "mtl_init fail\n");
    return 1;
  }

  sessions = calloc(cfg.sessions, sizeof(*sessions));
  sch_count = mtl_sch_get_count(mt);
  prev = sch_count > 0 ? calloc(sch_count, sizeof(*prev)) : NULL;
  if (!sessions || !prev) {
    ret = -ENOMEM;
    goto exit;
  }

  /* before the sessions: the RX port answers the ARP of a unicast TX create only then */
  ret = mtl_start(mt);
  if (ret < 0) {
    fprintf(stderr, "mtl_start fail %d\n", ret);
    goto exit;
  }
  for (created = 0; created < cfg.sessions; created++) {
    sessions[created].idx = created;
    ret = perf_session_create(&cfg, mt, &sessions[created]);
    if (ret < 0) {
      created++;
      goto stop;
    }
  }
  for (int i = 0; i < cfg.sessions; i++) {
    ret = perf_session_start(&sessions[i]);
    if (ret < 0) goto stop;
  }

  if (!cfg.json) {
    printf(
        "# sch: ts,kind,sch_idx,sch_name,lcore,time_measure,tasklet_idx,tasklet_name,"
        "cnt,avg_ns,min_ns,max_ns,busy_pct,avg_ns_per_loop,work_cnt,work_avg_ns,"
        "work_busy_pct\n");
    printf("# session: ts,kind,dir,idx,frames,pkts,lost,unrecovered\n");
    if (!cfg.tx_only)
      printf("# rxcost: ts,kind,scope,rx_pkts,rx_work_ns,ns_per_pkt,rx_work_busy_pct\n");
  }

  /* the first interval starts here, not at mtl_start() */
  for (int i = 0; i < sch_count; i++) {
    if (cfg.reset_each_interval)
      mtl_sch_reset_stats(mt, i);
    else
      prev[i].valid = !mtl_sch_get_stats(mt, i, &prev[i].stats);
  }
  for (int i = 0; i < cfg.sessions; i++) rx_pkts_prev += perf_rx_pkts(&sessions[i]);

  uint64_t start_ns = perf_now_ns(CLOCK_MONOTONIC);
  uint64_t end_ns =
      cfg.duration_s ? start_ns + (uint64_t)cfg.duration_s * PERF_NS_PER_S : 0;
  uint64_t last_ns = start_ns;
  uint64_t next_ns = start_ns + (uint64_t)cfg.interval_s * PERF_NS_PER_S;
  while (!g_stop) {
    perf_sleep_until(end_ns && end_ns < next_ns ? end_ns : next_ns);
    uint64_t now = perf_now_ns(CLOCK_MONOTONIC);
    perf_sample(&cfg, mt, sessions, prev, sch_count, now - last_ns, &rx_pkts_prev);
    last_ns = now;
    next_ns += (uint64_t)cfg.interval_s * PERF_NS_PER_S;
    if (end_ns && now >= end_ns) break;
  }
  ret = 0;

stop:
  for (int i = 0; i < cfg.sessions; i++) perf_session_stop(&sessions[i]);
  for (int i = created - 1; i >= 0; i--) perf_session_free(&sessions[i]);
  mtl_stop(mt);
exit:
  free(prev);
  free(sessions);
  mtl_uninit(mt);
  return ret < 0 ? 1 : 0;
}
