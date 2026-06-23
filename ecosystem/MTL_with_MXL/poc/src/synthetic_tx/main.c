/* SPDX-License-Identifier: BSD-3-Clause
 * MTL-MXL POC — ST2110-20 TX source (pipeline API)
 *
 * Reads a raw YUV file (yuv422p10le) or generates SMPTE color bars and
 * transmits them as an ST 2110-20 stream via MTL's pipeline API (st20p_tx).
 *
 * The pipeline API handles YUV422P10LE → RFC 4175 (YUV_422_10BIT) conversion
 * internally.  The receiver side should expect YUV_422_10BIT transport format.
 *
 * Usage:
 *   synthetic_st20_tx \
 *     --port 0000:31:01.1 \
 *     --sip 192.168.1.22  \
 *     --tx-ip 239.0.0.1   \
 *     --tx-udp-port 20000 \
 *     --width 1920 --height 1080 \
 *     --fps-num 30000 --fps-den 1001 \
 *     --yuv-file /opt/yuv422p10le_1080p.yuv \
 *     --duration 120
 */

#include <arpa/inet.h>
#include <getopt.h>
#include <immintrin.h>
#include <mtl/mtl_api.h>
#include <mtl/st20_api.h>
#include <mtl/st_pipeline_api.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "poc_thumbnail_tx.h"
#include "poc_types.h"

/* ── Non-temporal memcpy — bypasses writer's L2 cache ──
 * The fill thread writes framebuffers that the MTL TX scheduler on core 1
 * must later read for YUV422P10LE → RFC 4175 conversion + packetisation.
 * NT stores write directly to L3 (LLC), avoiding L2 pollution on the fill
 * thread and letting the scheduler core fetch from L3 instead of paying
 * the cross-core coherence penalty.
 */
static inline void nt_memcpy(void* dst, const void* src, size_t n) {
  const __m128i* s = (const __m128i*)src;
  __m128i* d = (__m128i*)dst;
  size_t chunks = n / sizeof(__m128i);

  for (size_t i = 0; i < chunks; i++) _mm_stream_si128(d + i, _mm_loadu_si128(s + i));

  size_t tail = n & (sizeof(__m128i) - 1);
  if (tail) memcpy((uint8_t*)dst + n - tail, (const uint8_t*)src + n - tail, tail);

  _mm_sfence();
}

/* ────────────────── globals ────────────────── */
static volatile bool g_running = true;

static void sig_handler(int sig) {
  (void)sig;
  g_running = false;
}

/* ────────────────── V210 colour-bar generator ────────────────── */
/*
 * SMPTE EG-1 colour bars (75 %) in 10-bit YCbCr:
 *   White,  Yellow, Cyan, Green, Magenta, Red, Blue, Black
 *
 * V210 packs 6 10-bit components into 4 × 32-bit words (16 bytes for
 * 6 pixels).  Each 32-bit word holds 3 × 10-bit values with 2 bits pad:
 *
 *   word = (comp2 << 20) | (comp1 << 10) | comp0
 *
 * Component order within a 6-pixel group (indices 0..5):
 *   Cb0  Y0  Cr0  Y1  Cb2  Y2  Cr2  Y3  Cb4  Y4  Cr4  Y5
 *   └──word0──┘  └──word1──┘  └──word2──┘  └──word3──┘
 */

/* 75 % colour bars — 10-bit Y, Cb, Cr */
static const uint16_t bars_y[] = {721, 674, 581, 534, 251, 204, 111, 64};
static const uint16_t bars_cb[] = {512, 176, 589, 253, 771, 435, 848, 512};
static const uint16_t bars_cr[] = {512, 543, 176, 207, 817, 848, 481, 512};
#define NUM_BARS 8

static inline uint32_t v210_word(uint16_t c0, uint16_t c1, uint16_t c2) {
  return ((uint32_t)(c2 & 0x3FF) << 20) | ((uint32_t)(c1 & 0x3FF) << 10) |
         ((uint32_t)(c0 & 0x3FF));
}

static void fill_v210_color_bars(uint8_t* frame, uint32_t width, uint32_t height,
                                 uint32_t stride) {
  uint32_t bar_edges[NUM_BARS + 1];
  for (int i = 0; i <= NUM_BARS; i++)
    bar_edges[i] = (uint32_t)((uint64_t)width * i / NUM_BARS);

  for (uint32_t y = 0; y < height; y++) {
    uint32_t* line = (uint32_t*)(frame + (size_t)y * stride);
    uint32_t bar = 0;

    for (uint32_t x = 0; x < width; x += 6) {
      uint16_t yv[6], cb[3], cr[3];

      for (int p = 0; p < 6; p++) {
        uint32_t px = x + p;
        if (px >= width) px = width - 1;
        while (bar < NUM_BARS - 1 && px >= bar_edges[bar + 1]) bar++;
        yv[p] = bars_y[bar];
      }
      for (int p = 0; p < 3; p++) {
        uint32_t px = x + p * 2;
        if (px >= width) px = width - 1;
        uint32_t b = 0;
        while (b < NUM_BARS - 1 && px >= bar_edges[b + 1]) b++;
        cb[p] = bars_cb[b];
        cr[p] = bars_cr[b];
      }

      uint32_t word_idx = (x / 6) * 4;
      line[word_idx + 0] = v210_word(cb[0], yv[0], cr[0]);
      line[word_idx + 1] = v210_word(yv[1], cb[1], yv[2]);
      line[word_idx + 2] = v210_word(cr[1], yv[3], cb[2]);
      line[word_idx + 3] = v210_word(yv[4], cr[2], yv[5]);
    }
  }
}

/* ────────────────── pattern generators ────────────────── */
static void fill_v210_black(uint8_t* buf, uint32_t w, uint32_t h, uint32_t stride) {
  for (uint32_t y = 0; y < h; y++) {
    uint32_t* line = (uint32_t*)(buf + (size_t)y * stride);
    for (uint32_t x = 0; x < w; x += 6) {
      uint32_t i = (x / 6) * 4;
      line[i + 0] = v210_word(512, 64, 512);
      line[i + 1] = v210_word(64, 512, 64);
      line[i + 2] = v210_word(512, 64, 512);
      line[i + 3] = v210_word(64, 512, 64);
    }
  }
}

static void fill_v210_white(uint8_t* buf, uint32_t w, uint32_t h, uint32_t stride) {
  for (uint32_t y = 0; y < h; y++) {
    uint32_t* line = (uint32_t*)(buf + (size_t)y * stride);
    for (uint32_t x = 0; x < w; x += 6) {
      uint32_t i = (x / 6) * 4;
      line[i + 0] = v210_word(512, 940, 512);
      line[i + 1] = v210_word(940, 512, 940);
      line[i + 2] = v210_word(512, 940, 512);
      line[i + 3] = v210_word(940, 512, 940);
    }
  }
}

static void fill_v210_ramp(uint8_t* buf, uint32_t w, uint32_t h, uint32_t stride) {
  for (uint32_t y = 0; y < h; y++) {
    uint32_t* line = (uint32_t*)(buf + (size_t)y * stride);
    for (uint32_t x = 0; x < w; x += 6) {
      uint16_t yv[6];
      for (int p = 0; p < 6; p++) {
        uint32_t px = x + p;
        if (px >= w) px = w - 1;
        yv[p] = (uint16_t)(64 + (876 * px / (w - 1)));
      }
      uint32_t i = (x / 6) * 4;
      line[i + 0] = v210_word(512, yv[0], 512);
      line[i + 1] = v210_word(yv[1], 512, yv[2]);
      line[i + 2] = v210_word(512, yv[3], 512);
      line[i + 3] = v210_word(yv[4], 512, yv[5]);
    }
  }
}

/* ────────────────── pipeline TX context ────────────────── */
typedef struct {
  mtl_handle mtl;
  st20p_tx_handle tx_handle;
  uint8_t* pattern_buf; /* pre-generated test pattern (fallback) */
  size_t frame_size;    /* pipeline frame size */
  uint32_t width;
  uint32_t height;
  /* YUV file playback */
  uint8_t* yuv_mmap; /* mmap'd YUV file (NULL = pattern mode) */
  size_t yuv_file_size;
  size_t yuv_frame_size; /* per-frame size in yuv422p10le */
  uint64_t yuv_total_frames;
  uint64_t yuv_frame_idx;
  volatile bool stop;
  pthread_t tx_thread;
  uint64_t frames_sent;
  char thumb_dir[512];
} tx_ctx_t;

/* ────────────────── Runtime control via control file ────────────────── */
/*
 * The control server writes /tmp/poc_ctrl.json with:
 *   {"mute_p": bool, "mute_r": bool, "corrupt": bool}
 *
 * This function reads that file and calls MTL APIs accordingly.
 * Called from the main stats loop (~1 Hz). Lightweight: no allocs,
 * just a small file read + simple string matching.
 */
/* ────────────────── CLI config (forward for control section) ────────────────── */
typedef struct {
  char port[64];
  uint8_t sip[4];
  uint8_t tx_ip[4];
  uint16_t tx_udp_port;
  uint8_t payload_type;
  uint32_t width;
  uint32_t height;
  uint32_t fps_num;
  uint32_t fps_den;
  uint32_t framebuff_cnt;
  int duration_sec;
  char pattern[32];
  char lcores[128];   /* MTL DPDK lcore list (empty=auto) */
  char yuv_file[512]; /* path to raw yuv422p10le file (empty=pattern) */
  /* ST 2022-7 redundancy */
  bool redundancy;
  char port_r[64];        /* PCIe BDF for redundant DPDK port */
  uint8_t sip_r[4];       /* Source IP for redundant port */
  uint8_t tx_ip_r[4];     /* Dest IP for R path */
  uint16_t tx_udp_port_r; /* UDP port for R path */
  /* TX-side preview thumbnails */
  char thumb_dir[512]; /* dir for thumb_p.jpg / thumb_r.jpg (empty=off) */
} synth_config_t;

#define CTRL_FILE "/dev/shm/poc_ctrl.json"

typedef struct {
  bool mute_p;
  bool mute_r;
  bool corrupt;
} ctrl_state_t;

static ctrl_state_t g_ctrl_prev = {false, false, false};

static bool json_get_bool(const char* json, const char* key) {
  /* Tiny helper: find "key": true/false in a flat JSON object */
  char needle[64];
  snprintf(needle, sizeof(needle), "\"%s\"", key);
  const char* p = strstr(json, needle);
  if (!p) return false;
  p += strlen(needle);
  /* skip : and whitespace */
  while (*p == ' ' || *p == ':' || *p == '\t') p++;
  return (strncmp(p, "true", 4) == 0);
}

/* Dummy destination IPs for muting — two distinct multicast addresses that
 * nobody subscribes to.  Must differ so MTL doesn't reject "same IP for both"
 * when both P and R are muted simultaneously. */
static const uint8_t DUMMY_DIP_P[4] = {239, 255, 255, 253};
static const uint8_t DUMMY_DIP_R[4] = {239, 255, 255, 254};

static void apply_dest_update(st20p_tx_handle h, const synth_config_t* cfg, bool mute_p,
                              bool mute_r) {
  struct st_tx_dest_info dst;
  memset(&dst, 0, sizeof(dst));

  if (mute_p)
    memcpy(dst.dip_addr[MTL_SESSION_PORT_P], DUMMY_DIP_P, MTL_IP_ADDR_LEN);
  else
    memcpy(dst.dip_addr[MTL_SESSION_PORT_P], cfg->tx_ip, MTL_IP_ADDR_LEN);
  dst.udp_port[MTL_SESSION_PORT_P] = cfg->tx_udp_port;

  if (cfg->redundancy) {
    if (mute_r)
      memcpy(dst.dip_addr[MTL_SESSION_PORT_R], DUMMY_DIP_R, MTL_IP_ADDR_LEN);
    else
      memcpy(dst.dip_addr[MTL_SESSION_PORT_R], cfg->tx_ip_r, MTL_IP_ADDR_LEN);
    dst.udp_port[MTL_SESSION_PORT_R] = cfg->tx_udp_port_r;
  }

  int ret = st20p_tx_update_destination(h, &dst);
  if (ret < 0) fprintf(stderr, "[CTRL] st20p_tx_update_destination failed: %d\n", ret);
}

static void poll_ctrl_file(mtl_handle mtl, st20p_tx_handle tx_h,
                           const synth_config_t* cfg) {
  char buf[256];
  FILE* f = fopen(CTRL_FILE, "r");
  if (!f) return; /* no file = no changes */
  size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  if (n == 0) return;
  buf[n] = '\0';

  ctrl_state_t cur;
  cur.mute_p = json_get_bool(buf, "mute_p");
  cur.mute_r = json_get_bool(buf, "mute_r");
  cur.corrupt = json_get_bool(buf, "corrupt");

  /* Apply destination changes only when mute state actually differs */
  if (cur.mute_p != g_ctrl_prev.mute_p || cur.mute_r != g_ctrl_prev.mute_r) {
    apply_dest_update(tx_h, cfg, cur.mute_p, cur.mute_r);
    if (cur.mute_p != g_ctrl_prev.mute_p)
      printf("[CTRL] P-path %s (dest→%s)\n", cur.mute_p ? "MUTED" : "UNMUTED",
             cur.mute_p ? "239.255.255.253" : "original");
    if (cfg->redundancy && cur.mute_r != g_ctrl_prev.mute_r)
      printf("[CTRL] R-path %s (dest→%s)\n", cur.mute_r ? "MUTED" : "UNMUTED",
             cur.mute_r ? "239.255.255.254" : "original");
  }
  if (cur.corrupt != g_ctrl_prev.corrupt) {
    if (cur.corrupt) {
      /* Per-packet corruption: MTL will redirect ~50% of packets on each
       * path to a dummy multicast IP (hash-based, complementary P/R). */
      mtl_set_runtime_tx_packet_loss(mtl, MTL_PORT_P, true, 0, 2);
      if (cfg->redundancy) mtl_set_runtime_tx_packet_loss(mtl, MTL_PORT_R, true, 1, 2);
      printf("[CTRL] Packet corruption ENABLED (per-pkt redirect)\n");
    } else {
      mtl_set_runtime_tx_packet_loss(mtl, MTL_PORT_P, false, 0, 0);
      if (cfg->redundancy) mtl_set_runtime_tx_packet_loss(mtl, MTL_PORT_R, false, 0, 0);
      printf("[CTRL] Packet corruption DISABLED\n");
    }
  }
  g_ctrl_prev = cur;
}

/* TX thread — get frame from pipeline, fill with YUV data or pattern, put back.
 * MTL internally converts YUV422P10LE → RFC 4175 before packetising. */
static void* tx_thread_fn(void* arg) {
  tx_ctx_t* ctx = (tx_ctx_t*)arg;

  printf("[SYNTH_TX] TX thread started\n");

  while (!ctx->stop && g_running) {
    struct st_frame* frame = st20p_tx_get_frame(ctx->tx_handle);
    if (!frame) {
      /* timeout or shutting down */
      continue;
    }

    if (ctx->yuv_mmap) {
      /* Read next frame from YUV file, loop at end */
      size_t offset = ctx->yuv_frame_idx * ctx->yuv_frame_size;
      /* YUV422P10LE planar layout: Y, Cb, Cr planes */
      size_t y_plane = (size_t)ctx->width * ctx->height * 2;
      size_t uv_plane = (size_t)(ctx->width / 2) * ctx->height * 2;
      const uint8_t* src = ctx->yuv_mmap + offset;
      /* NT stores: fill thread → scheduler core (cross-core) */
      nt_memcpy(frame->addr[0], src, y_plane);
      nt_memcpy(frame->addr[1], src + y_plane, uv_plane);
      nt_memcpy(frame->addr[2], src + y_plane + uv_plane, uv_plane);
      ctx->yuv_frame_idx = (ctx->yuv_frame_idx + 1) % ctx->yuv_total_frames;
    } else {
      /* Fallback: pre-generated pattern (single plane) */
      memcpy(frame->addr[0], ctx->pattern_buf, ctx->frame_size);
    }

    /* Generate P/R preview thumbnails from the actual frame data */
    if (ctx->thumb_dir[0]) {
      void* planes[3] = {frame->addr[0], frame->addr[1], frame->addr[2]};
      poc_thumb_tx_write(planes, g_ctrl_prev.corrupt, g_ctrl_prev.mute_p,
                         g_ctrl_prev.mute_r);
    }

    st20p_tx_put_frame(ctx->tx_handle, frame);
    ctx->frames_sent++;
  }

  printf("[SYNTH_TX] TX thread stopped\n");
  return NULL;
}

/* ────────────────── CLI ────────────────── */
static void print_usage(const char* prog) {
  printf(
      "Usage: %s [options]\n"
      "  --port BDF           PCIe BDF for MTL NIC VF (e.g. 0000:31:01.1)\n"
      "  --sip IP             Source IP for DPDK port\n"
      "  --tx-ip IP           Destination IP (multicast or unicast)\n"
      "  --tx-udp-port PORT   UDP port (default 20000)\n"
      "  --payload-type PT    RTP payload type (default 112)\n"
      "  --width W            Video width (default 1920)\n"
      "  --height H           Video height (default 1080)\n"
      "  --fps-num N          FPS numerator (default 30000)\n"
      "  --fps-den D          FPS denominator (default 1001)\n"
      "  --fb-count N         Frame-buffer count (default 3)\n"
      "  --pattern PAT        Test pattern: bars (default), black, white, ramp\n"
      "  --yuv-file PATH      Raw YUV422P10LE file (overrides --pattern)\n"
      "  --duration SEC       Run duration (0 = forever, default 0)\n"
      "  --redundancy         Enable ST 2022-7 dual-port TX\n"
      "  --port-r BDF         PCIe BDF for redundant DPDK port\n"
      "  --sip-r IP           Source IP for redundant port\n"
      "  --tx-ip-r IP         Destination IP for R path\n"
      "  --tx-udp-port-r PORT UDP port for R path\n"
      "  --thumb-dir DIR      Directory for P/R preview thumbnails\n"
      "  --help               Show this help\n",
      prog);
}

static int parse_ip(const char* str, uint8_t out[4]) {
  struct in_addr addr;
  if (inet_pton(AF_INET, str, &addr) != 1) return -1;
  memcpy(out, &addr, 4);
  return 0;
}

static enum st_fps fps_to_mtl(uint32_t num, uint32_t den) {
  double fps = (double)num / (double)den;
  if (fps > 119.5) return ST_FPS_P120;
  if (fps > 59.5) return ST_FPS_P60;
  if (fps > 49.5) return ST_FPS_P50;
  if (fps > 29.5 && den == 1) return ST_FPS_P30;
  if (fps > 29.5) return ST_FPS_P29_97;
  if (fps > 24.5 && den == 1) return ST_FPS_P25;
  if (fps > 23.5) return ST_FPS_P24;
  return ST_FPS_P30;
}

/* ────────────────── main ────────────────── */
int main(int argc, char** argv) {
  synth_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));

  /* defaults */
  strncpy(cfg.port, "0000:31:01.1", sizeof(cfg.port) - 1);
  cfg.sip[0] = 192;
  cfg.sip[1] = 168;
  cfg.sip[2] = 1;
  cfg.sip[3] = 22;
  cfg.tx_ip[0] = 239;
  cfg.tx_ip[1] = 0;
  cfg.tx_ip[2] = 0;
  cfg.tx_ip[3] = 1;
  cfg.tx_udp_port = 20000;
  cfg.payload_type = 112;
  cfg.width = 3840;
  cfg.height = 2160;
  cfg.fps_num = 30000;
  cfg.fps_den = 1001;
  cfg.framebuff_cnt = 3;
  cfg.duration_sec = 0;
  strncpy(cfg.pattern, "bars", sizeof(cfg.pattern) - 1);

  static struct option long_opts[] = {{"port", required_argument, NULL, 'b'},
                                      {"sip", required_argument, NULL, 's'},
                                      {"tx-ip", required_argument, NULL, 't'},
                                      {"tx-udp-port", required_argument, NULL, 'u'},
                                      {"payload-type", required_argument, NULL, 'y'},
                                      {"width", required_argument, NULL, 'W'},
                                      {"height", required_argument, NULL, 'H'},
                                      {"fps-num", required_argument, NULL, 'N'},
                                      {"fps-den", required_argument, NULL, 'D'},
                                      {"fb-count", required_argument, NULL, 'F'},
                                      {"pattern", required_argument, NULL, 'P'},
                                      {"duration", required_argument, NULL, 'd'},
                                      {"lcores", required_argument, NULL, 'L'},
                                      {"yuv-file", required_argument, NULL, 'Y'},
                                      {"redundancy", no_argument, NULL, 'R'},
                                      {"port-r", required_argument, NULL, 1001},
                                      {"sip-r", required_argument, NULL, 1002},
                                      {"tx-ip-r", required_argument, NULL, 1003},
                                      {"tx-udp-port-r", required_argument, NULL, 1004},
                                      {"thumb-dir", required_argument, NULL, 1005},
                                      {"help", no_argument, NULL, 'h'},
                                      {NULL, 0, NULL, 0}};

  int opt;
  while ((opt = getopt_long(argc, argv, "h", long_opts, NULL)) != -1) {
    switch (opt) {
      case 'b':
        strncpy(cfg.port, optarg, sizeof(cfg.port) - 1);
        break;
      case 's':
        parse_ip(optarg, cfg.sip);
        break;
      case 't':
        parse_ip(optarg, cfg.tx_ip);
        break;
      case 'u':
        cfg.tx_udp_port = (uint16_t)atoi(optarg);
        break;
      case 'y':
        cfg.payload_type = (uint8_t)atoi(optarg);
        break;
      case 'W':
        cfg.width = (uint32_t)atoi(optarg);
        break;
      case 'H':
        cfg.height = (uint32_t)atoi(optarg);
        break;
      case 'N':
        cfg.fps_num = (uint32_t)atoi(optarg);
        break;
      case 'D':
        cfg.fps_den = (uint32_t)atoi(optarg);
        break;
      case 'F':
        cfg.framebuff_cnt = (uint32_t)atoi(optarg);
        break;
      case 'P':
        strncpy(cfg.pattern, optarg, sizeof(cfg.pattern) - 1);
        break;
      case 'd':
        cfg.duration_sec = atoi(optarg);
        break;
      case 'L':
        strncpy(cfg.lcores, optarg, sizeof(cfg.lcores) - 1);
        break;
      case 'Y':
        strncpy(cfg.yuv_file, optarg, sizeof(cfg.yuv_file) - 1);
        break;
      case 'R':
        cfg.redundancy = true;
        break;
      case 1001:
        strncpy(cfg.port_r, optarg, sizeof(cfg.port_r) - 1);
        break;
      case 1002:
        parse_ip(optarg, cfg.sip_r);
        break;
      case 1003:
        parse_ip(optarg, cfg.tx_ip_r);
        break;
      case 1004:
        cfg.tx_udp_port_r = (uint16_t)atoi(optarg);
        break;
      case 1005:
        strncpy(cfg.thumb_dir, optarg, sizeof(cfg.thumb_dir) - 1);
        break;
      case 'h':
        print_usage(argv[0]);
        return 0;
      default:
        print_usage(argv[0]);
        return 1;
    }
  }

  signal(SIGINT, sig_handler);
  signal(SIGTERM, sig_handler);

  /* ── banner ── */
  char sip_str[INET_ADDRSTRLEN], dip_str[INET_ADDRSTRLEN];
  inet_ntop(AF_INET, cfg.sip, sip_str, sizeof(sip_str));
  inet_ntop(AF_INET, cfg.tx_ip, dip_str, sizeof(dip_str));

  bool use_yuv_file = (cfg.yuv_file[0] != '\0');
  /* yuv422p10le frame size: Y(w*h*2) + Cb(w/2*h*2) + Cr(w/2*h*2) */
  size_t yuv_frame_size =
      (size_t)cfg.width * cfg.height * 2 + (size_t)(cfg.width / 2) * cfg.height * 2 * 2;
  double fps_val = (double)cfg.fps_num / cfg.fps_den;

  char sip_r_str[INET_ADDRSTRLEN] = "", dip_r_str[INET_ADDRSTRLEN] = "";
  if (cfg.redundancy) {
    inet_ntop(AF_INET, cfg.sip_r, sip_r_str, sizeof(sip_r_str));
    inet_ntop(AF_INET, cfg.tx_ip_r, dip_r_str, sizeof(dip_r_str));
  }

  printf(
      "\n"
      "══════════════════════════════════════════════\n"
      "  ST2110-20 TX (pipeline API)%s\n"
      "══════════════════════════════════════════════\n"
      "  NIC port P:  %s\n"
      "  Source IP P: %s\n"
      "  Dest P:      %s:%u\n",
      cfg.redundancy ? " [ST 2022-7]" : "", cfg.port, sip_str, dip_str, cfg.tx_udp_port);
  if (cfg.redundancy) {
    printf(
        "  NIC port R:  %s\n"
        "  Source IP R: %s\n"
        "  Dest R:      %s:%u\n",
        cfg.port_r, sip_r_str, dip_r_str, cfg.tx_udp_port_r);
  }
  printf(
      "  Resolution:  %ux%u @ %.2f fps\n"
      "  Input:       %s\n"
      "  Transport:   YUV_422_10BIT (RFC 4175)\n"
      "  Duration:    %s\n"
      "══════════════════════════════════════════════\n\n",
      cfg.width, cfg.height, fps_val, use_yuv_file ? cfg.yuv_file : cfg.pattern,
      cfg.duration_sec > 0 ? "" : "infinite (Ctrl-C to stop)");

  if (cfg.duration_sec > 0) printf("  (will run for %d seconds)\n\n", cfg.duration_sec);

  /* ── MTL init ── */
  struct mtl_init_params p;
  memset(&p, 0, sizeof(p));

  p.num_ports = cfg.redundancy ? 2 : 1;
  snprintf(p.port[MTL_PORT_P], MTL_PORT_MAX_LEN, "%s", cfg.port);
  memcpy(p.sip_addr[MTL_PORT_P], cfg.sip, MTL_IP_ADDR_LEN);

  if (cfg.redundancy) {
    snprintf(p.port[MTL_PORT_R], MTL_PORT_MAX_LEN, "%s", cfg.port_r);
    memcpy(p.sip_addr[MTL_PORT_R], cfg.sip_r, MTL_IP_ADDR_LEN);
  }

  p.flags = MTL_FLAG_DEV_AUTO_START_STOP;
  p.pacing = ST21_TX_PACING_WAY_TSC; /* skip broken iavf TM rate limiter training */
  p.log_level = MTL_LOG_LEVEL_INFO;
  mtl_para_tx_queues_cnt_set(&p, MTL_PORT_P, 1);
  mtl_para_rx_queues_cnt_set(&p, MTL_PORT_P, 0);
  if (cfg.redundancy) {
    mtl_para_tx_queues_cnt_set(&p, MTL_PORT_R, 1);
    mtl_para_rx_queues_cnt_set(&p, MTL_PORT_R, 0);
  }

  /* Pin DPDK lcores if configured */
  if (cfg.lcores[0]) {
    p.lcores = cfg.lcores;
    printf("[SYNTH_TX] Pinning MTL lcores to: %s\n", cfg.lcores);
  }

  mtl_handle mtl = mtl_init(&p);
  if (!mtl) {
    fprintf(stderr, "[SYNTH_TX] mtl_init failed (port=%s)\n", cfg.port);
    return 1;
  }
  printf("[SYNTH_TX] MTL initialised on port %s\n", cfg.port);

  /* ── Pipeline TX session ── */
  tx_ctx_t ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.mtl = mtl;
  ctx.width = cfg.width;
  ctx.height = cfg.height;

  struct st20p_tx_ops ops;
  memset(&ops, 0, sizeof(ops));

  ops.name = "synth_tx";
  ops.priv = &ctx;

  /* Port config — st20p uses struct st_tx_port */
  ops.port.num_port = cfg.redundancy ? 2 : 1;
  memcpy(ops.port.dip_addr[MTL_SESSION_PORT_P], cfg.tx_ip, MTL_IP_ADDR_LEN);
  snprintf(ops.port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s", cfg.port);
  ops.port.udp_port[MTL_SESSION_PORT_P] = cfg.tx_udp_port;
  ops.port.payload_type = cfg.payload_type;

  if (cfg.redundancy) {
    memcpy(ops.port.dip_addr[MTL_SESSION_PORT_R], cfg.tx_ip_r, MTL_IP_ADDR_LEN);
    snprintf(ops.port.port[MTL_SESSION_PORT_R], MTL_PORT_MAX_LEN, "%s", cfg.port_r);
    ops.port.udp_port[MTL_SESSION_PORT_R] = cfg.tx_udp_port_r;
  }

  /* Video params */
  ops.width = cfg.width;
  ops.height = cfg.height;
  ops.fps = fps_to_mtl(cfg.fps_num, cfg.fps_den);
  ops.interlaced = false;

  /* Input format: YUV422P10LE (planar, matches ffmpeg yuv422p10le)
   * Transport:    YUV_422_10BIT (RFC 4175 on the wire — standard ST 2110-20)
   * MTL handles YUV422P10LE → RFC 4175 conversion internally. */
  ops.input_fmt = ST_FRAME_FMT_YUV422PLANAR10LE;
  ops.transport_fmt = ST20_FMT_YUV_422_10BIT;
  ops.transport_pacing = ST21_PACING_NARROW;
  ops.transport_packing = ST20_PACKING_BPM;
  ops.device = ST_PLUGIN_DEVICE_AUTO;

  ops.framebuff_cnt = (uint16_t)cfg.framebuff_cnt;
  if (ops.framebuff_cnt < 2) ops.framebuff_cnt = 3;

  /* Use blocking get — the TX thread will block until a frame is available */
  ops.flags = ST20P_TX_FLAG_BLOCK_GET;

  ctx.tx_handle = st20p_tx_create(mtl, &ops);
  if (!ctx.tx_handle) {
    fprintf(stderr, "[SYNTH_TX] st20p_tx_create failed\n");
    mtl_uninit(mtl);
    return 1;
  }

  ctx.frame_size = st20p_tx_frame_size(ctx.tx_handle);
  printf(
      "[SYNTH_TX] Pipeline TX session created: %ux%u, YUV422P10LE→RFC4175, "
      "frame_size=%zu\n",
      cfg.width, cfg.height, ctx.frame_size);

  /* ── TX-side preview thumbnails ── */
  if (cfg.thumb_dir[0]) {
    strncpy(ctx.thumb_dir, cfg.thumb_dir, sizeof(ctx.thumb_dir) - 1);
    if (poc_thumb_tx_init(cfg.thumb_dir, cfg.width, cfg.height) != 0) {
      fprintf(stderr, "[SYNTH_TX] WARNING: thumbnail init failed, previews disabled\n");
      ctx.thumb_dir[0] = '\0';
    }
  }

  /* ── Load YUV file or generate test pattern ── */
  if (use_yuv_file) {
    /* mmap the YUV file for zero-copy reads */
    struct stat st;
    if (stat(cfg.yuv_file, &st) != 0) {
      fprintf(stderr, "[SYNTH_TX] Cannot stat %s: %m\n", cfg.yuv_file);
      st20p_tx_free(ctx.tx_handle);
      mtl_uninit(mtl);
      return 1;
    }
    ctx.yuv_file_size = (size_t)st.st_size;
    ctx.yuv_frame_size = yuv_frame_size;
    ctx.yuv_total_frames = ctx.yuv_file_size / ctx.yuv_frame_size;
    if (ctx.yuv_total_frames == 0) {
      fprintf(stderr, "[SYNTH_TX] YUV file too small (need >= %zu bytes for one frame)\n",
              ctx.yuv_frame_size);
      st20p_tx_free(ctx.tx_handle);
      mtl_uninit(mtl);
      return 1;
    }
    FILE* fp = fopen(cfg.yuv_file, "rb");
    if (!fp) {
      fprintf(stderr, "[SYNTH_TX] Cannot open %s: %m\n", cfg.yuv_file);
      st20p_tx_free(ctx.tx_handle);
      mtl_uninit(mtl);
      return 1;
    }
    ctx.yuv_mmap = malloc(ctx.yuv_file_size);
    if (!ctx.yuv_mmap) {
      fprintf(stderr, "[SYNTH_TX] malloc(%zu) failed\n", ctx.yuv_file_size);
      fclose(fp);
      st20p_tx_free(ctx.tx_handle);
      mtl_uninit(mtl);
      return 1;
    }
    size_t rd = fread(ctx.yuv_mmap, 1, ctx.yuv_file_size, fp);
    fclose(fp);
    if (rd != ctx.yuv_file_size) {
      fprintf(stderr, "[SYNTH_TX] Short read: %zu/%zu\n", rd, ctx.yuv_file_size);
    }
    ctx.yuv_frame_idx = 0;
    printf(
        "[SYNTH_TX] YUV file loaded: %s\n"
        "           %lu frames, %zu bytes/frame (%.2f MB total)\n",
        cfg.yuv_file, ctx.yuv_total_frames, ctx.yuv_frame_size,
        (double)ctx.yuv_file_size / (1024.0 * 1024.0));
  } else {
    /* Generate a zero-filled pattern buffer (pattern gen uses V210 which
     * is now incompatible with YUV422P10LE input format, so just send
     * black frames as fallback when no YUV file is provided) */
    ctx.pattern_buf = calloc(1, ctx.frame_size);
    if (!ctx.pattern_buf) {
      fprintf(stderr, "[SYNTH_TX] malloc failed for pattern buffer\n");
      st20p_tx_free(ctx.tx_handle);
      mtl_uninit(mtl);
      return 1;
    }
    printf("[SYNTH_TX] No YUV file; sending black frames (%zu bytes)\n", ctx.frame_size);
  }

  /* ── Start TX thread ── */
  ctx.stop = false;
  int ret = pthread_create(&ctx.tx_thread, NULL, tx_thread_fn, &ctx);
  if (ret != 0) {
    fprintf(stderr, "[SYNTH_TX] pthread_create failed: %d\n", ret);
    free(ctx.pattern_buf);
    st20p_tx_free(ctx.tx_handle);
    mtl_uninit(mtl);
    return 1;
  }

  printf("[SYNTH_TX] Transmitting...\n\n");

  /* Write initial control state so the control server has a clean baseline */
  {
    FILE* cf = fopen(CTRL_FILE, "w");
    if (cf) {
      fprintf(cf, "{\"mute_p\": false, \"mute_r\": false, \"corrupt\": false}\n");
      fclose(cf);
    }
  }

  /* ── Main loop — print stats + poll control file ── */
  time_t start = time(NULL);
  uint64_t last_frames = 0;

  while (g_running) {
    sleep(1);

    /* Poll control file for mute/corrupt commands (~1 Hz) */
    poll_ctrl_file(mtl, ctx.tx_handle, &cfg);

    time_t elapsed = time(NULL) - start;
    uint64_t f = ctx.frames_sent;
    printf("[SYNTH_TX] %lds — %lu frames sent (+%lu)\n", (long)elapsed, f,
           f - last_frames);
    last_frames = f;

    if (cfg.duration_sec > 0 && elapsed >= cfg.duration_sec) break;
  }

  /* ── Cleanup ── */
  printf("\n[SYNTH_TX] Shutting down — total frames sent: %lu\n", ctx.frames_sent);
  ctx.stop = true;
  st20p_tx_wake_block(ctx.tx_handle);
  pthread_join(ctx.tx_thread, NULL);

  /* Restore original destinations before teardown */
  apply_dest_update(ctx.tx_handle, &cfg, false, false);
  unlink(CTRL_FILE);

  poc_thumb_tx_cleanup();
  free(ctx.pattern_buf);
  free(ctx.yuv_mmap);
  st20p_tx_free(ctx.tx_handle);
  mtl_uninit(mtl);

  return 0;
}
