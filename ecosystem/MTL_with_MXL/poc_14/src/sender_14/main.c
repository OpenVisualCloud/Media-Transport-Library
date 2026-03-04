/* SPDX-License-Identifier: BSD-3-Clause
 * poc_14 — 14-stream MTL ST2110-20 RX → MXL bridge sender
 *
 * Bypasses poc_mtl_rx.c (which hardcodes rx_sessions_cnt_max=1).
 * Creates a single MTL instance with up to 16 RX sessions, each
 * feeding its own MXL bridge instance via a dedicated frame queue.
 *
 * Usage:
 *   mtl_to_mxl_sender_14 --config poc_14/config/streams_14.json \
 *       --target-info-dir /tmp/target_info_16/
 */

#include "poc_types.h"
#include <sched.h>
#include "poc_frame_queue.h"
#include "poc_mxl_bridge.h"
#include "poc_v210_utils.h"
#include "poc_json_utils.h"
#include "poc_stats.h"

#include "config_parser.h"
#include "influxdb_14.h"
/* thumbnail_14.h removed — sender no longer generates thumbnails;
 * synth_tx thumbnails (thumb_tx_*.jpg) are used by the HTML dashboard */

#include <mtl/st20_api.h>
#include <mtl/mtl_api.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <getopt.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <time.h>

#define MAX_STREAMS POC14_MAX_STREAMS

/* ── Globals ── */
static volatile bool g_running = true;
static void sig_handler(int sig) { (void)sig; g_running = false; }

/* ── Helper: convert fps to MTL st_fps enum ── */
static enum st_fps fps_to_mtl(uint32_t num, uint32_t den)
{
    double fps = (double)num / (double)den;
    if (fps > 119.5)              return ST_FPS_P120;
    if (fps > 59.5)               return ST_FPS_P60;
    if (fps > 49.5)               return ST_FPS_P50;
    if (fps > 29.5 && den == 1)   return ST_FPS_P30;
    if (fps > 29.5)               return ST_FPS_P29_97;
    if (fps > 24.5 && den == 1)   return ST_FPS_P25;
    if (fps > 23.5)               return ST_FPS_P24;
    return ST_FPS_P30;
}

/* ── Per-stream context ── */
typedef struct {
    int                   stream_id;
    poc_frame_queue_t     queue;
    poc_mxl_bridge_t      bridge;
    poc_stats_t           stats;
    st20_rx_handle        rx_handle;

    /* Config references */
    poc14_stream_config_t stream_cfg;
    poc14_config_t       *global_cfg;

    /* Threads */
    pthread_t             bridge_tid;
    bool                  bridge_running;

} sender_stream_t;

/* ── RX frame callback — one per session ── */
static int rx_frame_ready_cb(void *priv, void *frame, struct st20_rx_frame_meta *meta)
{
    sender_stream_t *ss = (sender_stream_t *)priv;

    atomic_fetch_add(&ss->stats.rx_frames, 1);

    bool complete = st_is_frame_complete(meta->status);
    if (!complete) {
        atomic_fetch_add(&ss->stats.rx_incomplete, 1);
        if (!ss->global_cfg->accept_incomplete) {
            /* Release grain slot claimed by query_ext_frame */
            if (meta->opaque) {
                poc_grain_slot_t *gs = (poc_grain_slot_t *)meta->opaque;
                atomic_store_explicit(&gs->in_use, false, memory_order_release);
            }
            st20_rx_put_framebuff(ss->rx_handle, frame);
            return -1;
        }
    }

    /* Extract grain flow index */
    uint64_t flow_index = 0;
    if (meta->opaque) {
        poc_grain_slot_t *gs = (poc_grain_slot_t *)meta->opaque;
        flow_index = gs->flow_index;
    }

    poc_frame_entry_t entry = {
        .frame_addr      = frame,
        .timestamp        = meta->timestamp,
        .flow_index       = flow_index,
        .frame_recv_size  = (uint32_t)meta->frame_recv_size,
        .frame_total_size = (uint32_t)meta->frame_total_size,
        .complete         = complete,
        .enqueue_ns       = poc_now_realtime_ns(),
    };

    if (poc_queue_push(&ss->queue, &entry) != 0) {
        atomic_fetch_add(&ss->stats.rx_drops, 1);
        st20_rx_put_framebuff(ss->rx_handle, frame);
        return -1;
    }

    return 0;
}

/* ── Query ext_frame wrapper — dispatches to bridge's implementation ── */
static int query_ext_frame_wrapper(void *priv, struct st20_ext_frame *ext_frame,
                                   struct st20_rx_frame_meta *meta)
{
    sender_stream_t *ss = (sender_stream_t *)priv;
    return poc_mxl_bridge_query_ext_frame(&ss->bridge, ext_frame, meta);
}

/* ── Bridge worker thread ── */
static void *bridge_thread_fn(void *arg)
{
    sender_stream_t *ss = (sender_stream_t *)arg;
    printf("[SENDER-%d] Bridge thread started\n", ss->stream_id);
    poc_mxl_bridge_run(&ss->bridge);
    printf("[SENDER-%d] Bridge thread stopped\n", ss->stream_id);
    return NULL;
}

/* ── Load target_info string from file ── */
static char *load_target_info(const char *dir, int stream_id)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/target_info_%d.txt", dir, stream_id);

    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "[SENDER] Cannot open target info: %s\n", path);
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buf = malloc(len + 1);
    if (!buf) { fclose(f); return NULL; }

    size_t n = fread(buf, 1, len, f);
    buf[n] = '\0';
    fclose(f);

    /* Strip trailing whitespace */
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r' || buf[n-1] == ' '))
        buf[--n] = '\0';

    return buf;
}

int main(int argc, char **argv)
{
    const char *config_path = NULL;
    const char *target_info_dir = NULL;
    int cpu_start_override = -1;

    static struct option long_opts[] = {
        {"config",          required_argument, NULL, 'c'},
        {"target-info-dir", required_argument, NULL, 'd'},
        {"cpu-start",       required_argument, NULL, 's'},
        {"help",            no_argument,       NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "c:d:s:h", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'c': config_path = optarg; break;
        case 'd': target_info_dir = optarg; break;
        case 's': cpu_start_override = atoi(optarg); break;
        case 'h':
        default:
            printf("Usage: %s --config <streams_14.json> --target-info-dir <dir> [--cpu-start N]\n",
                   argv[0]);
            return (opt == 'h') ? 0 : 1;
        }
    }

    if (!config_path || !target_info_dir) {
        fprintf(stderr, "Error: --config and --target-info-dir are required\n");
        return 1;
    }

    /* Parse config */
    poc14_config_t cfg;
    if (poc14_config_parse(config_path, &cfg) != 0)
        return 1;

    /* Override CPU start if specified (avoids collision with synth TX) */
    if (cpu_start_override >= 0) {
        int offset = cpu_start_override - cfg.auto_cpu_start;
        for (uint32_t i = 0; i < cfg.num_streams; i++) {
            cfg.streams[i].worker_cpu += offset;
            cfg.streams[i].consumer_cpu += offset;
        }
        cfg.auto_cpu_start = cpu_start_override;
        printf("[SENDER_16] CPU start overridden to %d\n", cpu_start_override);
    }

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    uint32_t num_streams = cfg.num_streams;

    printf("\n══════════════════════════════════════════════════════\n");
    printf("  MTL-MXL Sender — %u-Stream Bridge\n", num_streams);
    printf("══════════════════════════════════════════════════════\n");
    poc14_config_print(&cfg);

    /* ── MTL init (single instance, N RX queues) ── */
    struct mtl_init_params p;
    memset(&p, 0, sizeof(p));

    p.num_ports = 1;
    snprintf(p.port[MTL_PORT_P], MTL_PORT_MAX_LEN, "%s", cfg.mtl_port);
    memcpy(p.sip_addr[MTL_PORT_P], cfg.sip, MTL_IP_ADDR_LEN);

    p.flags = MTL_FLAG_DEV_AUTO_START_STOP;
    p.log_level = MTL_LOG_LEVEL_INFO;
    p.nb_rx_desc = 4096;  /* double default (2048) to reduce HW drops */
    p.data_quota_mbs_per_sch = 20000;  /* 2 schedulers for 14 RX sessions */
    mtl_para_rx_queues_cnt_set(&p, MTL_PORT_P, (uint16_t)num_streams);
    mtl_para_tx_queues_cnt_set(&p, MTL_PORT_P, 0);

    if (cfg.mtl_lcores[0]) {
        p.lcores = (char *)cfg.mtl_lcores;
        printf("[SENDER_16] Pinning MTL lcores to: %s\n", cfg.mtl_lcores);
    }

    mtl_handle mtl = mtl_init(&p);
    if (!mtl) {
        fprintf(stderr, "[SENDER_16] mtl_init failed\n");
        return 1;
    }
    printf("[SENDER_16] MTL initialised (%u RX queues)\n", num_streams);

    /* ── Per-stream setup ── */
    sender_stream_t streams[MAX_STREAMS];
    memset(streams, 0, sizeof(streams));

    /* Compute video params */
    poc_video_params_t vp = {
        .width      = cfg.video_width,
        .height     = cfg.video_height,
        .fps_num    = cfg.fps_num,
        .fps_den    = cfg.fps_den,
        .v210_stride = poc_v210_line_stride(cfg.video_width),
    };
    vp.frame_size = poc_v210_frame_size(cfg.video_width, cfg.video_height);

    int active = 0;

    for (uint32_t i = 0; i < num_streams; i++) {
        sender_stream_t *ss = &streams[active];
        const poc14_stream_config_t *sc = &cfg.streams[i];

        ss->stream_id   = sc->id;
        ss->stream_cfg  = *sc;
        ss->global_cfg  = &cfg;
        poc_stats_reset(&ss->stats);

        /* Build per-stream poc_config_t for bridge init (reuses existing API) */
        poc_config_t per_cfg;
        memset(&per_cfg, 0, sizeof(per_cfg));
        strncpy(per_cfg.mtl_port, cfg.mtl_port, sizeof(per_cfg.mtl_port)-1);
        memcpy(per_cfg.sip, cfg.sip, 4);
        memcpy(per_cfg.rx_ip, sc->multicast_ip, 4);
        per_cfg.rx_udp_port   = sc->udp_port;
        per_cfg.payload_type  = cfg.payload_type ? cfg.payload_type : 112;
        per_cfg.video         = vp;
        strncpy(per_cfg.mxl_domain, cfg.mxl_domain, sizeof(per_cfg.mxl_domain)-1);
        strncpy(per_cfg.flow_json_path, sc->flow_json_path, sizeof(per_cfg.flow_json_path)-1);
        strncpy(per_cfg.fabrics_provider, cfg.fabrics_provider, sizeof(per_cfg.fabrics_provider)-1);
        strncpy(per_cfg.fabrics_local_ip, cfg.fabrics_local_ip, sizeof(per_cfg.fabrics_local_ip)-1);
        strncpy(per_cfg.fabrics_local_port, sc->fabrics_local_port, sizeof(per_cfg.fabrics_local_port)-1);
        per_cfg.framebuff_cnt    = (uint32_t)(cfg.framebuff_cnt > 0 ? cfg.framebuff_cnt : 8);
        per_cfg.queue_depth      = 16;
        per_cfg.accept_incomplete = cfg.accept_incomplete;
        per_cfg.worker_cpu       = sc->worker_cpu;
        per_cfg.redundancy       = false;

        /* Frame queue */
        if (poc_queue_init(&ss->queue, per_cfg.queue_depth) != 0) {
            fprintf(stderr, "[SENDER-%d] Failed to init frame queue\n", sc->id);
            continue;
        }

        /* Load target info */
        char *ti_str = load_target_info(target_info_dir, sc->id);
        if (!ti_str) {
            fprintf(stderr, "[SENDER-%d] No target info, skipping\n", sc->id);
            continue;
        }
        strncpy(per_cfg.target_info_str, ti_str, sizeof(per_cfg.target_info_str)-1);
        free(ti_str);

        /* MXL Bridge init (creates FlowWriter, DMA maps grains) */
        if (poc_mxl_bridge_init(&ss->bridge, &per_cfg, mtl,
                                &ss->queue, &ss->stats, &g_running) != 0) {
            fprintf(stderr, "[SENDER-%d] Bridge init failed\n", sc->id);
            continue;
        }

        /* ST20 RX session (zero-copy ext_frame from grain buffers) */
        struct st20_rx_ops ops;
        memset(&ops, 0, sizeof(ops));

        char name[32];
        snprintf(name, sizeof(name), "rx16_%d", sc->id);
        ops.name = name;
        ops.priv = ss;
        ops.num_port = 1;

        memcpy(ops.ip_addr[MTL_SESSION_PORT_P], sc->multicast_ip, MTL_IP_ADDR_LEN);
        snprintf(ops.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s", cfg.mtl_port);
        ops.udp_port[MTL_SESSION_PORT_P] = sc->udp_port;

        /* SSM source filtering for camera sources */
        if (sc->camera_src_ip[0] || sc->camera_src_ip[1] ||
            sc->camera_src_ip[2] || sc->camera_src_ip[3]) {
            memcpy(ops.mcast_sip_addr[MTL_SESSION_PORT_P],
                   sc->camera_src_ip, MTL_IP_ADDR_LEN);
            char sip_str[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, sc->camera_src_ip, sip_str, sizeof(sip_str));
            printf("[SENDER-%d] SSM source filter: %s\n", sc->id, sip_str);
        }

        ops.type = ST20_TYPE_FRAME_LEVEL;
        ops.width = cfg.video_width;
        ops.height = cfg.video_height;
        ops.fps = fps_to_mtl(cfg.fps_num, cfg.fps_den);
        ops.interlaced = false;
        ops.fmt = ST20_FMT_YUV_422_10BIT;
        ops.payload_type = per_cfg.payload_type;

        ops.framebuff_cnt = (uint16_t)per_cfg.framebuff_cnt;
        if (ops.framebuff_cnt < 2) ops.framebuff_cnt = 4;

        ops.notify_frame_ready = rx_frame_ready_cb;
        ops.flags |= ST20_RX_FLAG_RECEIVE_INCOMPLETE_FRAME;
        ops.query_ext_frame = query_ext_frame_wrapper;

        ss->rx_handle = st20_rx_create(mtl, &ops);
        if (!ss->rx_handle) {
            fprintf(stderr, "[SENDER-%d] st20_rx_create failed\n", sc->id);
            poc_mxl_bridge_destroy(&ss->bridge);
            continue;
        }

        ss->bridge.rx_handle = ss->rx_handle;

        /* Connect fabrics */
        if (poc_mxl_bridge_connect(&ss->bridge, &per_cfg) != 0) {
            fprintf(stderr, "[SENDER-%d] Fabrics connect failed\n", sc->id);
            st20_rx_free(ss->rx_handle);
            poc_mxl_bridge_destroy(&ss->bridge);
            continue;
        }

        /* Start bridge worker thread */
        if (pthread_create(&ss->bridge_tid, NULL, bridge_thread_fn, ss) != 0) {
            fprintf(stderr, "[SENDER-%d] pthread_create failed\n", sc->id);
            st20_rx_free(ss->rx_handle);
            poc_mxl_bridge_destroy(&ss->bridge);
            continue;
        }
        ss->bridge_running = true;

        /* Pin bridge worker to CPU */
        if (sc->worker_cpu >= 0) {
            cpu_set_t cpuset;
            CPU_ZERO(&cpuset);
            CPU_SET(sc->worker_cpu, &cpuset);
            int ht = sc->worker_cpu + 72;
            if (ht < 144) CPU_SET(ht, &cpuset);
            pthread_setaffinity_np(ss->bridge_tid, sizeof(cpuset), &cpuset);
            printf("[SENDER-%d] Bridge pinned to CPU %d (+HT %d)\n",
                   sc->id, sc->worker_cpu, ht);
        }

        char dip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, sc->multicast_ip, dip, sizeof(dip));
        printf("[SENDER-%d] Stream %d ← %s:%u  bridge active\n",
               sc->id, sc->id, dip, sc->udp_port);

        active++;
    }

    if (active == 0) {
        fprintf(stderr, "[SENDER_16] No streams could be started\n");
        mtl_uninit(mtl);
        return 1;
    }

    printf("\n[SENDER_16] %d/%u streams active\n\n", active, num_streams);

    /* ── InfluxDB init ── */
    poc14_influxdb_init();

    /* ── Main loop: stats ── */
    time_t start = time(NULL);
    while (g_running) {
        sleep(2);

        time_t elapsed = time(NULL) - start;
        printf("[SENDER_16] %lds —", (long)elapsed);

        for (int i = 0; i < active; i++) {
            sender_stream_t *ss = &streams[i];
            uint64_t rx = atomic_load(&ss->stats.rx_frames);
            uint64_t br = atomic_load(&ss->stats.bridge_frames);
            uint64_t fab = atomic_load(&ss->stats.fabrics_transfers);
            printf(" s%d:rx=%lu/br=%lu/fab=%lu",
                   ss->stream_id, (unsigned long)rx,
                   (unsigned long)br, (unsigned long)fab);

            poc14_influxdb_push(&ss->stats, "sender", ss->stream_id);
        }
        printf("\n");

        if (cfg.duration_sec > 0 && elapsed >= cfg.duration_sec) {
            printf("[SENDER_16] Duration reached (%ds)\n", cfg.duration_sec);
            break;
        }
    }

    /* ── Cleanup ── */
    g_running = false;

    for (int i = 0; i < active; i++) {
        sender_stream_t *ss = &streams[i];
        if (ss->bridge_running) {
            pthread_join(ss->bridge_tid, NULL);
        }
        if (ss->rx_handle)
            st20_rx_free(ss->rx_handle);
        poc_mxl_bridge_destroy(&ss->bridge);
    }

    poc14_influxdb_cleanup();
    mtl_uninit(mtl);

    printf("[SENDER_16] Clean shutdown (%d streams)\n", active);
    return 0;
}
