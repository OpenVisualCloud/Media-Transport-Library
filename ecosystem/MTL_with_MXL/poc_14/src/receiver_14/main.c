/* SPDX-License-Identifier: BSD-3-Clause
 * poc_14 — 14-stream MXL sink receiver
 *
 * Creates one MXL sink + fabrics target + consumer per stream.
 * Prints target_info strings for each stream (collected by sender).
 *
 * Usage:
 *   mxl_sink_receiver_14 --config poc_14/config/streams_14.json \
 *       [--target-info-dir /tmp/target_info_16/]
 */

#include <errno.h>
#include <getopt.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "config_parser.h"
#include "influxdb_14.h"
#include "poc_consumer.h"
#include "poc_fabrics_target.h"
#include "poc_json_utils.h"
#include "poc_mxl_sink.h"
#include "poc_stats.h"
#include "poc_types.h"
#include "poc_v210_utils.h"
#include "thumbnail_14.h"

#define MAX_STREAMS POC14_MAX_STREAMS

/* ── Globals ── */
static volatile bool g_running = true;
static void sig_handler(int sig) {
  (void)sig;
  g_running = false;
}

/* ── Per-stream receiver context ── */
typedef struct {
  int stream_id;
  poc_mxl_sink_t sink;
  poc_fabrics_target_t ft;
  poc_stats_t stats;
  poc_consumer_ctx_t consumer_ctx;

  /* Threads */
  pthread_t consumer_tid;
  bool consumer_running;

  /* Thumbnail */
  poc14_thumb_ctx_t thumb;
  bool thumb_enabled;
} receiver_stream_t;

/* ── Consumer thread wrapper ── */
static void* consumer_thread_fn(void* arg) {
  poc_consumer_ctx_t* ctx = (poc_consumer_ctx_t*)arg;
  poc_consumer_run(ctx);
  return NULL;
}

/* ── Write target info to file ── */
static int write_target_info(const char* dir, int stream_id, const char* info) {
  if (!dir || !dir[0]) return 0;

  mkdir(dir, 0755); /* ensure directory exists */

  char path[1024];
  snprintf(path, sizeof(path), "%s/target_info_%d.txt", dir, stream_id);

  FILE* f = fopen(path, "w");
  if (!f) {
    fprintf(stderr, "[RECEIVER-%d] Cannot write target info to %s\n", stream_id, path);
    return -1;
  }
  fprintf(f, "%s\n", info);
  fclose(f);
  printf("[RECEIVER-%d] Target info written to %s\n", stream_id, path);
  return 0;
}

int main(int argc, char** argv) {
  const char* config_path = NULL;
  const char* target_info_dir = NULL;

  static struct option long_opts[] = {{"config", required_argument, NULL, 'c'},
                                      {"target-info-dir", required_argument, NULL, 'd'},
                                      {"help", no_argument, NULL, 'h'},
                                      {NULL, 0, NULL, 0}};

  int opt;
  while ((opt = getopt_long(argc, argv, "c:d:h", long_opts, NULL)) != -1) {
    switch (opt) {
      case 'c':
        config_path = optarg;
        break;
      case 'd':
        target_info_dir = optarg;
        break;
      case 'h':
      default:
        printf("Usage: %s --config <streams_14.json> [--target-info-dir <dir>]\n",
               argv[0]);
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

  signal(SIGINT, sig_handler);
  signal(SIGTERM, sig_handler);

  uint32_t num_streams = cfg.num_streams;

  printf("\n══════════════════════════════════════════════════════\n");
  printf("  MTL-MXL Receiver — %u-Stream Sink\n", num_streams);
  printf("══════════════════════════════════════════════════════\n");
  poc14_config_print(&cfg);

  /* ── Per-stream setup ── */
  receiver_stream_t streams[MAX_STREAMS];
  memset(streams, 0, sizeof(streams));

  int active = 0;

  for (uint32_t i = 0; i < num_streams; i++) {
    receiver_stream_t* rs = &streams[active];
    const poc14_stream_config_t* sc = &cfg.streams[i];

    rs->stream_id = sc->id;
    poc_stats_reset(&rs->stats);

    /* Build per-stream poc_config_t for existing sink/fabrics API */
    poc_config_t per_cfg;
    memset(&per_cfg, 0, sizeof(per_cfg));
    strncpy(per_cfg.mxl_domain, cfg.mxl_domain, sizeof(per_cfg.mxl_domain) - 1);
    strncpy(per_cfg.flow_json_path, sc->flow_json_path,
            sizeof(per_cfg.flow_json_path) - 1);
    strncpy(per_cfg.fabrics_provider, cfg.fabrics_provider,
            sizeof(per_cfg.fabrics_provider) - 1);
    strncpy(per_cfg.fabrics_local_ip, cfg.fabrics_local_ip,
            sizeof(per_cfg.fabrics_local_ip) - 1);
    strncpy(per_cfg.fabrics_local_port, sc->fabrics_local_port,
            sizeof(per_cfg.fabrics_local_port) - 1);
    per_cfg.video.width = cfg.video_width;
    per_cfg.video.height = cfg.video_height;
    per_cfg.video.fps_num = cfg.fps_num;
    per_cfg.video.fps_den = cfg.fps_den;
    per_cfg.video.v210_stride = poc_v210_line_stride(cfg.video_width);
    per_cfg.video.frame_size = poc_v210_frame_size(cfg.video_width, cfg.video_height);

    /* MXL Sink (FlowWriter + FlowReader) */
    if (poc_mxl_sink_init(&rs->sink, &per_cfg) != 0) {
      fprintf(stderr, "[RECEIVER-%d] MXL sink init failed\n", sc->id);
      continue;
    }

    /* Fabrics Target */
    if (poc_fabrics_target_init(&rs->ft, &rs->sink, &per_cfg) != 0) {
      fprintf(stderr, "[RECEIVER-%d] Fabrics target init failed\n", sc->id);
      poc_mxl_sink_destroy(&rs->sink);
      continue;
    }

    /* Print & write serialized target info */
    if (rs->ft.target_info_str) {
      printf("TARGET_INFO[%d]: %s\n", sc->id, rs->ft.target_info_str);
      write_target_info(target_info_dir, sc->id, rs->ft.target_info_str);
    }

    /* Thumbnail dir (now on tmpfs — see thumbnail_14.c) */
    char thumb_dir[64];
    snprintf(thumb_dir, sizeof(thumb_dir), "/dev/shm/poc14_thumbs");

    /* Consumer context */
    rs->consumer_ctx.sink = &rs->sink;
    rs->consumer_ctx.ft = &rs->ft;
    rs->consumer_ctx.stats = &rs->stats;
    rs->consumer_ctx.running = &g_running;
    rs->consumer_ctx.src_width = cfg.video_width;
    rs->consumer_ctx.src_height = cfg.video_height;
    rs->consumer_ctx.thumb_dir = NULL; /* Use poc14_thumb instead of original */
    rs->consumer_ctx.thumb16 = NULL;   /* Set after thumb init */

    /* Start consumer thread */
    if (pthread_create(&rs->consumer_tid, NULL, consumer_thread_fn, &rs->consumer_ctx) !=
        0) {
      fprintf(stderr, "[RECEIVER-%d] pthread_create failed\n", sc->id);
      poc_fabrics_target_destroy(&rs->ft);
      poc_mxl_sink_destroy(&rs->sink);
      continue;
    }
    rs->consumer_running = true;

    /* Pin consumer thread to CPU */
    if (sc->consumer_cpu >= 0) {
      cpu_set_t cpuset;
      CPU_ZERO(&cpuset);
      CPU_SET(sc->consumer_cpu, &cpuset);
      int ht = sc->consumer_cpu + 72;
      if (ht < 144) CPU_SET(ht, &cpuset);
      pthread_setaffinity_np(rs->consumer_tid, sizeof(cpuset), &cpuset);
      printf("[RECEIVER-%d] Consumer pinned to CPU %d (+HT %d)\n", sc->id,
             sc->consumer_cpu, ht);
    }

    /* Thumbnail (receiver side) */
    if (poc14_thumb_init(&rs->thumb, sc->id, "rx", (int)cfg.video_width,
                         (int)cfg.video_height) == 0) {
      rs->thumb_enabled = true;
      rs->consumer_ctx.thumb16 = (struct poc14_thumb_ctx*)&rs->thumb;
    }

    printf("[RECEIVER-%d] Stream %d active on fabrics port %s\n", sc->id, sc->id,
           sc->fabrics_local_port);

    active++;
  }

  if (active == 0) {
    fprintf(stderr, "[RECEIVER_16] No streams could be started\n");
    return 1;
  }

  printf("\n[RECEIVER_16] %d/%u streams active, waiting for connections...\n\n", active,
         num_streams);

  /* ── InfluxDB init ── */
  poc14_influxdb_init();

  /* ── Main loop: stats ── */
  time_t start = time(NULL);
  while (g_running) {
    sleep(2);

    time_t elapsed = time(NULL) - start;
    printf("[RECEIVER_16] %lds —", (long)elapsed);

    for (int i = 0; i < active; i++) {
      receiver_stream_t* rs = &streams[i];
      uint64_t te = atomic_load(&rs->stats.target_events);
      uint64_t cf = atomic_load(&rs->stats.consumer_frames);
      printf(" s%d:ev=%lu/con=%lu", rs->stream_id, (unsigned long)te, (unsigned long)cf);

      poc14_influxdb_push(&rs->stats, "receiver", rs->stream_id);
    }
    printf("\n");

    if (cfg.duration_sec > 0 && elapsed >= cfg.duration_sec) {
      printf("[RECEIVER_16] Duration reached (%ds)\n", cfg.duration_sec);
      break;
    }
  }

  /* ── Cleanup ── */
  g_running = false;

  for (int i = 0; i < active; i++) {
    receiver_stream_t* rs = &streams[i];
    if (rs->consumer_running) pthread_join(rs->consumer_tid, NULL);
    if (rs->thumb_enabled) poc14_thumb_cleanup(&rs->thumb);
    poc_fabrics_target_destroy(&rs->ft);
    poc_mxl_sink_destroy(&rs->sink);
  }

  poc14_influxdb_cleanup();

  printf("[RECEIVER_16] Clean shutdown (%d streams)\n", active);
  return 0;
}
