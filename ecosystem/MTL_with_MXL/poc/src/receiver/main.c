/* SPDX-License-Identifier: BSD-3-Clause
 * MTL-MXL POC — Host B: mxl_sink_receiver main
 *
 * Usage:
 *   mxl_sink_receiver \
 *     --mxl-domain /tmp/mxl \
 *     --flow-json ./config/v210_1080p30.json \
 *     --provider tcp \
 *     --bind-ip 10.0.0.2 \
 *     --bind-port 5000 \
 *     --duration 60
 */

#include "poc_types.h"
#include "poc_mxl_sink.h"
#include "poc_fabrics_target.h"
#include "poc_consumer.h"
#include "poc_stats.h"
#include "poc_influxdb.h"
#include "poc_json_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <getopt.h>
#include <pthread.h>
#include <time.h>
#include <sched.h>

static volatile bool g_running = true;

static void sig_handler(int sig)
{
    (void)sig;
    g_running = false;
}

static void print_usage(const char *prog)
{
    printf("Usage: %s [options]\n"
           "  --mxl-domain PATH     MXL domain path (tmpfs), default /tmp/mxl\n"
           "  --flow-json PATH      Path to v210 flow definition JSON\n"
           "  --provider NAME       Fabrics provider: tcp (default) or verbs\n"
           "  --bind-ip IP          Local IP for fabrics endpoint\n"
           "  --bind-port PORT      Local port for fabrics endpoint, default 5000\n"
           "  --thumb-dir DIR       Directory for JPEG thumbnail (enables preview)\n"
           "  --consumer-cpu CPU    Pin consumer thread to CPU core (-1 = no pin)\n"
           "  --duration SEC        Run duration (0 = forever), default 0\n"
           "  --help                Show this help\n",
           prog);
}

static void *consumer_thread_fn(void *arg)
{
    poc_consumer_ctx_t *ctx = (poc_consumer_ctx_t *)arg;
    poc_consumer_run(ctx);
    return NULL;
}

int main(int argc, char **argv)
{
    poc_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    /* Defaults */
    strncpy(cfg.mxl_domain, "/tmp/mxl", sizeof(cfg.mxl_domain)-1);
    strncpy(cfg.fabrics_provider, "tcp", sizeof(cfg.fabrics_provider)-1);
    strncpy(cfg.fabrics_local_ip, "0.0.0.0", sizeof(cfg.fabrics_local_ip)-1);
    strncpy(cfg.fabrics_local_port, "5000", sizeof(cfg.fabrics_local_port)-1);
    cfg.video.width   = 3840;
    cfg.video.height  = 2160;
    cfg.video.fps_num = 30000;
    cfg.video.fps_den = 1001;
    cfg.duration_sec  = 0;
    cfg.consumer_cpu  = -1;

    char thumb_dir[512] = "";   /* empty = disabled */

    static struct option long_opts[] = {
        {"mxl-domain",    required_argument, NULL, 'd'},
        {"flow-json",     required_argument, NULL, 'f'},
        {"provider",      required_argument, NULL, 'p'},
        {"bind-ip",       required_argument, NULL, 'i'},
        {"bind-port",     required_argument, NULL, 'P'},
        {"thumb-dir",     required_argument, NULL, 'T'},
        {"consumer-cpu",  required_argument, NULL, 'w'},
        {"duration",      required_argument, NULL, 't'},
        {"help",          no_argument,       NULL, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "d:f:p:i:P:T:w:t:h", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'd': strncpy(cfg.mxl_domain, optarg, sizeof(cfg.mxl_domain)-1); break;
        case 'f': strncpy(cfg.flow_json_path, optarg, sizeof(cfg.flow_json_path)-1); break;
        case 'p': strncpy(cfg.fabrics_provider, optarg, sizeof(cfg.fabrics_provider)-1); break;
        case 'i': strncpy(cfg.fabrics_local_ip, optarg, sizeof(cfg.fabrics_local_ip)-1); break;
        case 'P': strncpy(cfg.fabrics_local_port, optarg, sizeof(cfg.fabrics_local_port)-1); break;
        case 'T': strncpy(thumb_dir, optarg, sizeof(thumb_dir)-1); break;
        case 'w': cfg.consumer_cpu = atoi(optarg); break;
        case 't': cfg.duration_sec = atoi(optarg); break;
        case 'h':
        default:
            print_usage(argv[0]);
            return (opt == 'h') ? 0 : 1;
        }
    }

    if (!cfg.flow_json_path[0]) {
        fprintf(stderr, "Error: --flow-json is required\n");
        print_usage(argv[0]);
        return 1;
    }

    /* ── Extract video dimensions from the flow JSON ── */
    {
        size_t json_len = 0;
        char *json = poc_load_file(cfg.flow_json_path, &json_len);
        if (json) {
            /* Simple extraction: look for "frame_width": N and "frame_height": N */
            const char *wp = strstr(json, "\"frame_width\"");
            const char *hp = strstr(json, "\"frame_height\"");
            if (wp) {
                wp = strchr(wp, ':');
                if (wp) cfg.video.width = (uint32_t)atoi(wp + 1);
            }
            if (hp) {
                hp = strchr(hp, ':');
                if (hp) cfg.video.height = (uint32_t)atoi(hp + 1);
            }
            /* Also try grain_rate */
            const char *gn = strstr(json, "\"numerator\"");
            const char *gd = strstr(json, "\"denominator\"");
            if (gn) {
                gn = strchr(gn, ':');
                if (gn) cfg.video.fps_num = (uint32_t)atoi(gn + 1);
            }
            if (gd) {
                gd = strchr(gd, ':');
                if (gd) cfg.video.fps_den = (uint32_t)atoi(gd + 1);
            }
            printf("[CONFIG] Parsed from flow JSON: %ux%u @ %u/%u\n",
                   cfg.video.width, cfg.video.height,
                   cfg.video.fps_num, cfg.video.fps_den);
            free(json);
        }
    }

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    printf("══════════════════════════════════════════════\n");
    printf("  MTL-MXL POC — Host B Receiver (Sink)\n");
    printf("══════════════════════════════════════════════\n");
    printf("  MXL domain:    %s\n", cfg.mxl_domain);
    printf("  Flow JSON:     %s\n", cfg.flow_json_path);
    printf("  Provider:      %s\n", cfg.fabrics_provider);
    printf("  Bind:          %s:%s\n", cfg.fabrics_local_ip, cfg.fabrics_local_port);
    printf("  Duration:      %ds\n", cfg.duration_sec);
    printf("══════════════════════════════════════════════\n\n");

    /* ── Step 1: Create MXL Flow sink ── */
    poc_mxl_sink_t sink;
    if (poc_mxl_sink_init(&sink, &cfg) != 0) {
        fprintf(stderr, "Failed to initialise MXL sink\n");
        return 1;
    }

    /* ── Step 2: Create Fabrics target ── */
    poc_fabrics_target_t ft;
    if (poc_fabrics_target_init(&ft, &sink, &cfg) != 0) {
        fprintf(stderr, "Failed to initialise Fabrics target\n");
        poc_mxl_sink_destroy(&sink);
        return 1;
    }

    /* ── Step 3: Start consumer thread ── */
    poc_stats_t stats;
    poc_stats_reset(&stats);
    poc_influxdb_init();

    poc_consumer_ctx_t consumer_ctx = {
        .sink       = &sink,
        .ft         = &ft,
        .stats      = &stats,
        .running    = &g_running,
        .src_width  = cfg.video.width,
        .src_height = cfg.video.height,
        .thumb_dir  = thumb_dir[0] ? thumb_dir : NULL,
    };

    pthread_t consumer_tid;
    if (pthread_create(&consumer_tid, NULL, consumer_thread_fn, &consumer_ctx) != 0) {
        perror("pthread_create consumer");
        poc_fabrics_target_destroy(&ft);
        poc_mxl_sink_destroy(&sink);
        return 1;
    }

    /* Optionally pin consumer thread to physical core + HT sibling.
     * Same NUMA topology as sender: on 2-socket Xeon (36 cores/socket, HT),
     * sibling of logical CPU N (N < 36) is N+72. */
    if (cfg.consumer_cpu >= 0) {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(cfg.consumer_cpu, &cpuset);
        int ht_sibling = cfg.consumer_cpu + 72;  /* NUMA 0 cores 0-35 */
        if (ht_sibling < 144) {
            CPU_SET(ht_sibling, &cpuset);
        }
        pthread_setaffinity_np(consumer_tid, sizeof(cpuset), &cpuset);
        printf("[MAIN] Consumer thread pinned to CPU %d (+HT %d)\n",
               cfg.consumer_cpu, ht_sibling);
    }

    /* ── Main loop: periodic stats + wait for signal or duration ── */
    time_t start = time(NULL);
    uint64_t last_report_ms = 0;
    while (g_running) {
        usleep(500000); /* 500ms */
        if (cfg.duration_sec > 0 && (time(NULL) - start) >= cfg.duration_sec) {
            printf("[MAIN] Duration reached (%ds), shutting down\n", cfg.duration_sec);
            g_running = false;
        }
        /* Stats reporting moved here from consumer thread so the
         * synchronous InfluxDB POST cannot stall RDMA event drain */
        struct timespec ts_now;
        clock_gettime(CLOCK_MONOTONIC, &ts_now);
        uint64_t now_ms = (uint64_t)ts_now.tv_sec * 1000 + ts_now.tv_nsec / 1000000;
        if (now_ms - last_report_ms >= 2000) {
            poc_stats_report(&stats, "receiver");
            last_report_ms = now_ms;
        }
    }

    /* ── Cleanup ── */
    pthread_join(consumer_tid, NULL);

    printf("\n[MAIN] Final stats:\n");
    poc_stats_report(&stats, "receiver");

    poc_influxdb_cleanup();
    poc_fabrics_target_destroy(&ft);
    poc_mxl_sink_destroy(&sink);

    printf("[MAIN] Clean shutdown complete\n");
    return 0;
}
