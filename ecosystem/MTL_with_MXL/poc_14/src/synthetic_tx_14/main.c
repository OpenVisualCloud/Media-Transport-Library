/* SPDX-License-Identifier: BSD-3-Clause
 * poc_14 — 14-stream ST2110-20 TX source (pipeline API)
 *
 * Creates a single MTL instance with up to 16 TX sessions, each
 * transmitting to a different multicast group. Reads config from
 * a JSON file (streams_14.json). Skips streams marked as "camera".
 *
 * Loads a YUV422P10LE file (same as POC) and copies frames into the
 * pipeline's planar buffers. Generates thumbnails from the planar data.
 *
 * Usage:
 *   synthetic_st20_tx_14 --config poc_14/config/streams_14.json \
 *                        [--yuv-file /path/to/yuv422p10le_1080p.yuv]
 */

#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <immintrin.h>
#include <mtl/mtl_api.h>
#include <mtl/st20_api.h>
#include <mtl/st_pipeline_api.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "config_parser.h"
#include "poc_types.h"
#include "thumbnail_14.h"

#define MAX_STREAMS POC14_MAX_STREAMS

/* Default YUV file — same as POC uses */
#define DEFAULT_YUV_FILE "/nvme/MTL-MXL/yuv422p10le_jellyfish_1080p.yuv"

/* ── Globals ── */
static volatile bool g_running = true;

static void sig_handler(int sig) {
  (void)sig;
  g_running = false;
}

/* ── Shared YUV file data (all streams share the same content) ── */
static uint8_t* g_yuv_mmap = NULL;
static size_t g_yuv_file_size = 0;
static size_t g_yuv_frame_size = 0;
static uint64_t g_yuv_total_frames = 0;

/* ── Non-temporal memcpy — bypasses writer's L2 cache ──
 * Fill threads on cores 21-34 write framebuffers that the MTL TX scheduler
 * on core 5 must later read.  Normal memcpy pollutes the fill thread's L2
 * with data it never reads again, forcing core 5 to snoop/invalidate those
 * remote cache lines (50% L2 miss rate measured via PCM).  NT stores write
 * directly to L3 (LLC), skipping L2 entirely.  Core 5 then fetches from L3
 * at ~10 ns instead of paying the cross-core coherence penalty (~35 ns).
 */
static inline void nt_memcpy(void* dst, const void* src, size_t n) {
  const __m128i* s = (const __m128i*)src;
  __m128i* d = (__m128i*)dst;
  size_t chunks = n / sizeof(__m128i); /* 16-byte chunks */

  for (size_t i = 0; i < chunks; i++) _mm_stream_si128(d + i, _mm_loadu_si128(s + i));

  /* Handle trailing bytes (< 16) with regular store */
  size_t tail = n & (sizeof(__m128i) - 1);
  if (tail) memcpy((uint8_t*)dst + n - tail, (const uint8_t*)src + n - tail, tail);

  _mm_sfence(); /* ensure all NT stores are globally visible */
}

/* ── Per-stream TX context ── */
typedef struct {
  int stream_id;
  st20p_tx_handle tx_handle;
  size_t frame_size;
  volatile bool stop;
  volatile uint64_t frames_sent;
  poc14_thumb_ctx_t thumb;
  bool thumb_enabled;
  uint32_t width;
  uint32_t height;
  uint64_t yuv_frame_idx; /* current frame index in YUV file */
  int pin_cpu;            /* CPU core to pin this worker to */
} tx_stream_ctx_t;

/* ── TX thread per stream ── */
static void* tx_thread_fn(void* arg) {
  tx_stream_ctx_t* ctx = (tx_stream_ctx_t*)arg;

  /* Pin this worker thread to a specific CPU core */
  if (ctx->pin_cpu >= 0) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(ctx->pin_cpu, &cpuset);
    int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
    if (rc == 0)
      printf("[TX-%d] Pinned to CPU %d\n", ctx->stream_id, ctx->pin_cpu);
    else
      fprintf(stderr, "[TX-%d] Failed to pin to CPU %d: %s\n", ctx->stream_id,
              ctx->pin_cpu, strerror(rc));
  }

  printf("[TX-%d] Thread started\n", ctx->stream_id);

  /* YUV422P10LE plane sizes */
  size_t y_plane_sz = (size_t)ctx->width * ctx->height * 2;
  size_t uv_plane_sz = (size_t)(ctx->width / 2) * ctx->height * 2;

  while (!ctx->stop && g_running) {
    struct st_frame* frame = st20p_tx_get_frame(ctx->tx_handle);
    if (!frame) continue;

    if (g_yuv_mmap && g_yuv_total_frames > 0) {
      /* NT-store: bypass this thread's L2 → data lands in LLC directly.
       * Core 5 (TX scheduler) reads it from L3 without cross-core snoop. */
      size_t offset = ctx->yuv_frame_idx * g_yuv_frame_size;
      const uint8_t* src = g_yuv_mmap + offset;
      nt_memcpy(frame->addr[0], src, y_plane_sz);
      nt_memcpy(frame->addr[1], src + y_plane_sz, uv_plane_sz);
      nt_memcpy(frame->addr[2], src + y_plane_sz + uv_plane_sz, uv_plane_sz);
      ctx->yuv_frame_idx = (ctx->yuv_frame_idx + 1) % g_yuv_total_frames;
    } else {
      /* No YUV file — send black frames */
      memset(frame->addr[0], 0, ctx->frame_size);
    }

    /* Thumbnail every 3rd frame (~20fps preview per stream):
     *  - Reads from frame->addr[].  After NT-stores the data sits in
     *    LLC (L3).  The thumbnail read brings it into this core's L1/L2
     *    but that's fine — this core doesn't need to keep it, and core 5
     *    already has an L3 path.
     *  - Only holds the pipeline frame 1/3 of the time; the other 2/3
     *    of frames are released immediately after nt_memcpy. */
    if (ctx->thumb_enabled && (ctx->frames_sent % 3) == 0) {
      void* planes[3] = {frame->addr[0], frame->addr[1], frame->addr[2]};
      poc14_thumb_write_planar(&ctx->thumb, planes);
    }

    st20p_tx_put_frame(ctx->tx_handle, frame);
    ctx->frames_sent++;
  }

  printf("[TX-%d] Thread stopped, frames=%lu\n", ctx->stream_id,
         (unsigned long)ctx->frames_sent);
  return NULL;
}

/* ── FPS enum helper ── */
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

/* ── Main ── */
int main(int argc, char** argv) {
  const char* config_path = NULL;
  const char* yuv_file = DEFAULT_YUV_FILE;

  static struct option long_opts[] = {{"config", required_argument, NULL, 'c'},
                                      {"yuv-file", required_argument, NULL, 'Y'},
                                      {"help", no_argument, NULL, 'h'},
                                      {NULL, 0, NULL, 0}};

  int opt;
  while ((opt = getopt_long(argc, argv, "c:Y:h", long_opts, NULL)) != -1) {
    switch (opt) {
      case 'c':
        config_path = optarg;
        break;
      case 'Y':
        yuv_file = optarg;
        break;
      case 'h':
      default:
        printf("Usage: %s --config <streams_14.json> [--yuv-file <path>]\n", argv[0]);
        return (opt == 'h') ? 0 : 1;
    }
  }

  if (!config_path) {
    fprintf(stderr, "Error: --config is required\n");
    return 1;
  }

  /* Parse config */
  poc14_config_t cfg;
  if (poc14_config_parse(config_path, &cfg) != 0) return 1;

  /* Count synthetic streams (skip camera sources) */
  int synth_count = 0;
  int synth_indices[MAX_STREAMS];
  for (uint32_t i = 0; i < cfg.num_streams; i++) {
    if (strcmp(cfg.streams[i].source, "camera") != 0) {
      synth_indices[synth_count++] = (int)i;
    }
  }

  if (synth_count == 0) {
    printf("[SYNTH_TX_16] No synthetic streams in config, nothing to do\n");
    return 0;
  }

  signal(SIGINT, sig_handler);
  signal(SIGTERM, sig_handler);

  printf("\n══════════════════════════════════════════════════════\n");
  printf("  ST2110-20 TX — %d-Stream Pattern Generator\n", synth_count);
  printf("══════════════════════════════════════════════════════\n");
  poc14_config_print(&cfg);

  /* ── MTL init ── */
  struct mtl_init_params p;
  memset(&p, 0, sizeof(p));

  p.num_ports = 1; /* non-redundant */
  snprintf(p.port[MTL_PORT_P], MTL_PORT_MAX_LEN, "%s",
           cfg.tx_port[0] ? cfg.tx_port : cfg.mtl_port);
  memcpy(p.sip_addr[MTL_PORT_P],
         cfg.tx_sip[0] || cfg.tx_sip[1] || cfg.tx_sip[2] || cfg.tx_sip[3] ? cfg.tx_sip
                                                                          : cfg.sip,
         MTL_IP_ADDR_LEN);

  p.flags = MTL_FLAG_DEV_AUTO_START_STOP;
  p.pacing = ST21_TX_PACING_WAY_TSC; /* skip broken iavf TM rate limiter training */
  p.log_level = MTL_LOG_LEVEL_INFO;
  p.data_quota_mbs_per_sch = 20000; /* split 14 sessions across 2 schedulers */
  mtl_para_tx_queues_cnt_set(&p, MTL_PORT_P, (uint16_t)synth_count);
  mtl_para_rx_queues_cnt_set(&p, MTL_PORT_P, 0);

  const char* lcores = cfg.tx_lcores[0] ? cfg.tx_lcores : cfg.mtl_lcores;
  if (lcores[0]) {
    p.lcores = (char*)lcores;
    printf("[SYNTH_TX_16] Pinning MTL lcores to: %s\n", lcores);
  }

  mtl_handle mtl = mtl_init(&p);
  if (!mtl) {
    fprintf(stderr, "[SYNTH_TX_16] mtl_init failed\n");
    return 1;
  }
  printf("[SYNTH_TX_16] MTL initialised (%d TX queues)\n", synth_count);

  /* ── Load YUV file (shared across all streams) ── */
  /* yuv422p10le frame size: Y(w*h*2) + Cb(w/2*h*2) + Cr(w/2*h*2) */
  g_yuv_frame_size = (size_t)cfg.video_width * cfg.video_height * 2 +
                     (size_t)(cfg.video_width / 2) * cfg.video_height * 2 * 2;

  struct stat yuv_st;
  if (stat(yuv_file, &yuv_st) == 0) {
    g_yuv_file_size = (size_t)yuv_st.st_size;
    g_yuv_total_frames = g_yuv_file_size / g_yuv_frame_size;
    if (g_yuv_total_frames > 0) {
      FILE* fp = fopen(yuv_file, "rb");
      if (fp) {
        g_yuv_mmap = malloc(g_yuv_file_size);
        if (g_yuv_mmap) {
          size_t rd = fread(g_yuv_mmap, 1, g_yuv_file_size, fp);
          if (rd != g_yuv_file_size)
            fprintf(stderr, "[SYNTH_TX_16] Short read: %zu/%zu\n", rd, g_yuv_file_size);
          printf(
              "[SYNTH_TX_16] YUV file loaded: %s\n"
              "              %lu frames, %zu bytes/frame (%.2f MB total)\n",
              yuv_file, (unsigned long)g_yuv_total_frames, g_yuv_frame_size,
              (double)g_yuv_file_size / (1024.0 * 1024.0));
        } else {
          fprintf(stderr, "[SYNTH_TX_16] malloc(%zu) failed for YUV data\n",
                  g_yuv_file_size);
        }
        fclose(fp);
      } else {
        fprintf(stderr, "[SYNTH_TX_16] Cannot open %s: %m\n", yuv_file);
      }
    } else {
      fprintf(stderr, "[SYNTH_TX_16] YUV file too small (need >= %zu bytes)\n",
              g_yuv_frame_size);
    }
  } else {
    fprintf(stderr, "[SYNTH_TX_16] YUV file not found: %s — sending black frames\n",
            yuv_file);
  }

  /* ── Create per-stream TX sessions ── */
  tx_stream_ctx_t streams[MAX_STREAMS];
  memset(streams, 0, sizeof(streams));

  for (int si = 0; si < synth_count; si++) {
    int idx = synth_indices[si];
    const poc14_stream_config_t* sc = &cfg.streams[idx];
    tx_stream_ctx_t* ctx = &streams[si];

    ctx->stream_id = sc->id;
    ctx->width = cfg.video_width;
    ctx->height = cfg.video_height;

    /* Pipeline TX session */
    struct st20p_tx_ops ops;
    memset(&ops, 0, sizeof(ops));

    char name[32];
    snprintf(name, sizeof(name), "tx16_%d", sc->id);
    ops.name = name;
    ops.priv = ctx;

    ops.port.num_port = 1;
    memcpy(ops.port.dip_addr[MTL_SESSION_PORT_P], sc->multicast_ip, MTL_IP_ADDR_LEN);
    snprintf(ops.port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
             cfg.tx_port[0] ? cfg.tx_port : cfg.mtl_port);
    ops.port.udp_port[MTL_SESSION_PORT_P] = sc->udp_port;
    ops.port.payload_type = cfg.payload_type ? cfg.payload_type : 112;

    ops.width = cfg.video_width;
    ops.height = cfg.video_height;
    ops.fps = fps_to_mtl(cfg.fps_num, cfg.fps_den);
    ops.interlaced = false;

    ops.input_fmt = ST_FRAME_FMT_YUV422PLANAR10LE;
    ops.transport_fmt = ST20_FMT_YUV_422_10BIT;
    ops.transport_pacing = ST21_PACING_NARROW;
    ops.transport_packing = ST20_PACKING_BPM;
    ops.device = ST_PLUGIN_DEVICE_AUTO;

    ops.framebuff_cnt = (uint16_t)(cfg.framebuff_cnt > 2 ? cfg.framebuff_cnt : 3);
    ops.flags = ST20P_TX_FLAG_BLOCK_GET;

    ctx->tx_handle = st20p_tx_create(mtl, &ops);
    if (!ctx->tx_handle) {
      fprintf(stderr, "[SYNTH_TX_16] st20p_tx_create failed for stream %d\n", sc->id);
      goto cleanup;
    }

    ctx->frame_size = st20p_tx_frame_size(ctx->tx_handle);

    /* Stagger YUV frame index so each stream shows a different frame */
    if (g_yuv_total_frames > 0)
      ctx->yuv_frame_idx =
          (uint64_t)si * (g_yuv_total_frames / synth_count) % g_yuv_total_frames;

    /* Pin worker thread to its dedicated CPU core */
    ctx->pin_cpu = cfg.auto_cpu_start + si;

    /* Thumbnail */
    if (poc14_thumb_init(&ctx->thumb, sc->id, "tx", (int)cfg.video_width,
                         (int)cfg.video_height) == 0) {
      ctx->thumb_enabled = true;
    }

    char dip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, sc->multicast_ip, dip_str, sizeof(dip_str));
    printf("[SYNTH_TX_16] Stream %d → %s:%u  frame_size=%zu\n", sc->id, dip_str,
           sc->udp_port, ctx->frame_size);
  }

  /* ── Start TX threads ── */
  pthread_t tids[MAX_STREAMS];
  for (int si = 0; si < synth_count; si++) {
    streams[si].stop = false;
    if (pthread_create(&tids[si], NULL, tx_thread_fn, &streams[si]) != 0) {
      fprintf(stderr, "[SYNTH_TX_16] pthread_create failed for stream %d\n",
              streams[si].stream_id);
      goto cleanup;
    }
  }

  printf("\n[SYNTH_TX_16] All %d TX threads running\n\n", synth_count);

  /* ── Main loop — stats ── */
  time_t start = time(NULL);
  uint64_t last_frames[MAX_STREAMS];
  memset(last_frames, 0, sizeof(last_frames));

  while (g_running) {
    sleep(2);

    time_t elapsed = time(NULL) - start;
    printf("[SYNTH_TX_16] %lds —", (long)elapsed);
    for (int si = 0; si < synth_count; si++) {
      uint64_t f = streams[si].frames_sent;
      printf(" s%d:%lu(+%lu)", streams[si].stream_id, (unsigned long)f,
             (unsigned long)(f - last_frames[si]));
      last_frames[si] = f;
    }
    printf("\n");

    if (cfg.duration_sec > 0 && elapsed >= cfg.duration_sec) break;
  }

cleanup:
  g_running = false;

  for (int si = 0; si < synth_count; si++) {
    streams[si].stop = true;
    if (streams[si].tx_handle) st20p_tx_wake_block(streams[si].tx_handle);
  }
  for (int si = 0; si < synth_count; si++) {
    if (streams[si].tx_handle) pthread_join(tids[si], NULL);
  }

  printf("\n[SYNTH_TX_16] Final frame counts:");
  for (int si = 0; si < synth_count; si++) {
    printf(" s%d:%lu", streams[si].stream_id, (unsigned long)streams[si].frames_sent);
  }
  printf("\n");

  for (int si = 0; si < synth_count; si++) {
    if (streams[si].thumb_enabled) poc14_thumb_cleanup(&streams[si].thumb);
    if (streams[si].tx_handle) st20p_tx_free(streams[si].tx_handle);
  }
  free(g_yuv_mmap);
  g_yuv_mmap = NULL;
  mtl_uninit(mtl);

  printf("[SYNTH_TX_16] Clean shutdown\n");
  return 0;
}
