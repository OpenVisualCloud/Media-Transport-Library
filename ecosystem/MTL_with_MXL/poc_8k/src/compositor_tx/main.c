/* SPDX-License-Identifier: BSD-3-Clause
 * poc_8k — Compositor TX (dedicated ST22 TX process)
 *
 * Reads JPEG-XS codestreams from a shared-memory ring buffer
 * (produced by the compositor) and transmits them as ST2110-22
 * via a dedicated MTL VF.
 *
 * Data flow:
 *   shm_ring (from compositor) → ST22 TX → multicast
 *
 * This runs as a separate process from the compositor so that
 * ST22 TX uses its own dedicated VF, avoiding TX/RX contention.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <getopt.h>
#include <arpa/inet.h>
#include <time.h>
#include <errno.h>
#include <stdatomic.h>

/* MTL */
#include <mtl/mtl_api.h>
#include <mtl/st20_api.h>
#include <mtl/st_pipeline_api.h>

/* Shared-memory ring buffer (IPC from compositor) */
#include "../common/shm_ring.h"

/* ── Constants ── */
#define OUT_W           7680
#define OUT_H           4320
#define MAX_CODESTREAM_SIZE  (20 * 1024 * 1024)  /* 20 MB */

/* ── Latency header (must match compositor/receiver/sender definition) ── */
typedef struct __attribute__((packed)) {
    uint64_t encode_start_ns;
    uint32_t frame_idx;
    uint32_t codestream_size;
    uint32_t encode_duration_us;
    uint64_t shm_commit_ns;
    uint64_t comptx_pop_ns;
    uint64_t comptx_tx_ready_ns;
} poc_8k_frame_header_t;
#define POC_8K_HEADER_SIZE  ((uint32_t)sizeof(poc_8k_frame_header_t))

/* ── Configuration ── */
typedef struct {
    char     tx_port[64];        /* VF BDF for ST22 TX */
    uint8_t  tx_sip[4];          /* Source IP for TX VF */
    uint8_t  tx_multicast[4];    /* Multicast destination */
    uint16_t tx_udp_port;
    uint8_t  payload_type;
    char     lcores[128];
    uint32_t fps_num;
    uint32_t fps_den;
    int      duration_sec;
    char     shm_name[128];      /* shared memory name */
} comptx_config_t;

/* ── Runtime context ── */
typedef struct {
    mtl_handle          mtl;
    st22_tx_handle      st22_tx;
    shm_ring_consumer_t shm_ring;

    /* ST22 TX framebuffers */
    int                 tx_fb_cnt;
    int                 tx_fb_write;
    atomic_int          tx_fb_ready;
    size_t              tx_codestream_sz;

    volatile bool       running;
    comptx_config_t     cfg;

    /* Stats */
    atomic_uint_fast64_t frames_sent;
    atomic_uint_fast64_t frames_read;
    atomic_uint_fast64_t frames_stale_dropped;
} comptx_t;

static comptx_t g_ctx;

static void sig_handler(int sig) { (void)sig; g_ctx.running = false; }

/* ═══════════════════════════════════════════════
 * ST22 TX callbacks
 * ═══════════════════════════════════════════════ */

static int st22_get_next_frame(void *priv, uint16_t *next_frame_idx,
                               struct st22_tx_frame_meta *meta)
{
    comptx_t *ctx = (comptx_t *)priv;
    int idx = atomic_load(&ctx->tx_fb_ready);
    if (idx < 0)
        return -1;

    *next_frame_idx = (uint16_t)idx;
    meta->codestream_size = ctx->tx_codestream_sz;
    atomic_store(&ctx->tx_fb_ready, -1);
    return 0;
}

static int st22_notify_frame_done(void *priv, uint16_t frame_idx,
                                  struct st22_tx_frame_meta *meta)
{
    comptx_t *ctx = (comptx_t *)priv;
    (void)frame_idx;
    (void)meta;
    atomic_fetch_add(&ctx->frames_sent, 1);
    return 0;
}

/* ── FPS helper ── */
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

/* ── IP parse ── */
static int parse_ip(const char *str, uint8_t out[4])
{
    unsigned a, b, c, d;
    if (sscanf(str, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return -1;
    out[0] = (uint8_t)a; out[1] = (uint8_t)b;
    out[2] = (uint8_t)c; out[3] = (uint8_t)d;
    return 0;
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ═══════════════════════════════════════════════
 * CLI
 * ═══════════════════════════════════════════════ */

static void print_usage(const char *prog)
{
    printf("Usage: %s [options]\n"
           "  --tx-port <BDF>       VF for ST22 TX (required, e.g. 0000:xx:02.0)\n"
           "  --tx-sip <IP>         Source IP (required, e.g. 192.168.1.84)\n"
           "  --tx-mcast <IP>       Multicast destination (required, e.g. 239.0.4.1)\n"
           "  --tx-udp-port <N>     UDP port (default: 30000)\n"
           "  --payload-type <N>    RTP payload type (default: 113)\n"
           "  --lcores <list>       DPDK lcores (default: 40,41)\n"
           "  --fps-num <N>         FPS numerator (default: 30000)\n"
           "  --fps-den <N>         FPS denominator (default: 1001)\n"
           "  --shm-name <name>     Shared memory name (default: /poc_8k_ring)\n"
           "  --duration <sec>      Run duration, 0=infinite (default: 0)\n"
           "  --help\n", prog);
}

static void set_defaults(comptx_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->tx_port[0] = '\0';
    memset(cfg->tx_sip, 0, sizeof(cfg->tx_sip));
    memset(cfg->tx_multicast, 0, sizeof(cfg->tx_multicast));
    cfg->tx_udp_port   = 30000;
    cfg->payload_type  = 113;
    strncpy(cfg->lcores, "40,41", sizeof(cfg->lcores));
    cfg->fps_num       = 30000;
    cfg->fps_den       = 1001;
    cfg->duration_sec  = 0;
    strncpy(cfg->shm_name, "/poc_8k_ring", sizeof(cfg->shm_name));
}

static int parse_args(int argc, char **argv, comptx_config_t *cfg)
{
    static struct option long_opts[] = {
        {"tx-port",        required_argument, NULL, 1001},
        {"tx-sip",         required_argument, NULL, 1002},
        {"tx-mcast",       required_argument, NULL, 1003},
        {"tx-udp-port",    required_argument, NULL, 1004},
        {"payload-type",   required_argument, NULL, 1005},
        {"lcores",         required_argument, NULL, 1006},
        {"fps-num",        required_argument, NULL, 1007},
        {"fps-den",        required_argument, NULL, 1008},
        {"shm-name",       required_argument, NULL, 1009},
        {"duration",       required_argument, NULL, 1010},
        {"help",           no_argument,       NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "h", long_opts, NULL)) != -1) {
        switch (opt) {
        case 1001: strncpy(cfg->tx_port, optarg, sizeof(cfg->tx_port)-1); break;
        case 1002: parse_ip(optarg, cfg->tx_sip); break;
        case 1003: parse_ip(optarg, cfg->tx_multicast); break;
        case 1004: cfg->tx_udp_port   = (uint16_t)atoi(optarg); break;
        case 1005: cfg->payload_type  = (uint8_t)atoi(optarg); break;
        case 1006: strncpy(cfg->lcores, optarg, sizeof(cfg->lcores)-1); break;
        case 1007: cfg->fps_num = (uint32_t)atoi(optarg); break;
        case 1008: cfg->fps_den = (uint32_t)atoi(optarg); break;
        case 1009: strncpy(cfg->shm_name, optarg, sizeof(cfg->shm_name)-1); break;
        case 1010: cfg->duration_sec = atoi(optarg); break;
        case 'h': print_usage(argv[0]); exit(0);
        default:  print_usage(argv[0]); return -1;
        }
    }
    return 0;
}

/* ═══════════════════════════════════════════════
 * Main
 * ═══════════════════════════════════════════════ */

int main(int argc, char **argv)
{
    comptx_t *ctx = &g_ctx;
    memset(ctx, 0, sizeof(*ctx));
    ctx->running = true;
    atomic_store(&ctx->tx_fb_ready, -1);

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    /* ── 1. Parse config ── */
    set_defaults(&ctx->cfg);
    if (parse_args(argc, argv, &ctx->cfg) != 0)
        return 1;

    comptx_config_t *cfg = &ctx->cfg;

    /* Validate required fields */
    if (!cfg->tx_port[0]) {
        fprintf(stderr, "Error: --tx-port is required (e.g. 0000:xx:02.0)\n");
        return 1;
    }
    if (cfg->tx_sip[0] == 0 && cfg->tx_sip[1] == 0 &&
        cfg->tx_sip[2] == 0 && cfg->tx_sip[3] == 0) {
        fprintf(stderr, "Error: --tx-sip is required (e.g. 192.168.1.84)\n");
        return 1;
    }
    if (cfg->tx_multicast[0] == 0 && cfg->tx_multicast[1] == 0 &&
        cfg->tx_multicast[2] == 0 && cfg->tx_multicast[3] == 0) {
        fprintf(stderr, "Error: --tx-mcast is required (e.g. 239.0.4.1)\n");
        return 1;
    }

    printf("\n══════════════════════════════════════════════════════\n");
    printf("  poc_8k Compositor TX — ST22 TX on dedicated VF\n");
    printf("  TX port: %s (SIP: %u.%u.%u.%u) → %u.%u.%u.%u:%u\n",
           cfg->tx_port, cfg->tx_sip[0], cfg->tx_sip[1],
           cfg->tx_sip[2], cfg->tx_sip[3],
           cfg->tx_multicast[0], cfg->tx_multicast[1],
           cfg->tx_multicast[2], cfg->tx_multicast[3],
           cfg->tx_udp_port);
    printf("  Lcores: %s\n", cfg->lcores);
    printf("  SHM ring: %s\n", cfg->shm_name);
    printf("══════════════════════════════════════════════════════\n\n");

    /* ── 2. Open shared-memory ring buffer ── */
    printf("[COMPTX] Waiting for compositor to create shared memory ring...\n");
    int shm_retries = 0;
    while (shm_ring_consumer_open(&ctx->shm_ring, cfg->shm_name) != 0) {
        if (++shm_retries > 60) {
            fprintf(stderr, "[COMPTX] Timeout waiting for %s\n", cfg->shm_name);
            return 1;
        }
        sleep(1);
    }

    /* ── 3. Init MTL (single VF, TX only) ── */
    struct mtl_init_params p;
    memset(&p, 0, sizeof(p));

    p.num_ports = 1;
    snprintf(p.port[MTL_PORT_P], MTL_PORT_MAX_LEN, "%s", cfg->tx_port);
    memcpy(p.sip_addr[MTL_PORT_P], cfg->tx_sip, MTL_IP_ADDR_LEN);

    p.flags = MTL_FLAG_DEV_AUTO_START_STOP;
    p.log_level = MTL_LOG_LEVEL_WARNING;
    p.data_quota_mbs_per_sch = 40000;  /* all sessions on 1 scheduler */

    mtl_para_rx_queues_cnt_set(&p, MTL_PORT_P, 0);
    mtl_para_tx_queues_cnt_set(&p, MTL_PORT_P, 1);

    if (cfg->lcores[0]) {
        p.lcores = (char *)cfg->lcores;
        printf("[MTL] Lcores: %s\n", cfg->lcores);
    }

    ctx->mtl = mtl_init(&p);
    if (!ctx->mtl) {
        fprintf(stderr, "[COMPTX] mtl_init failed\n");
        shm_ring_consumer_destroy(&ctx->shm_ring);
        return 1;
    }
    printf("[MTL] Initialized (port P: %s — TX only)\n", cfg->tx_port);

    /* ── 4. Create ST22 TX session ── */
    ctx->tx_fb_cnt = 3;
    ctx->tx_fb_write = 0;

    struct st22_tx_ops tx_ops;
    memset(&tx_ops, 0, sizeof(tx_ops));

    tx_ops.name = "comptx_st22";
    tx_ops.priv = ctx;
    tx_ops.num_port = 1;

    memcpy(tx_ops.dip_addr[MTL_SESSION_PORT_P],
           cfg->tx_multicast, MTL_IP_ADDR_LEN);
    snprintf(tx_ops.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN,
             "%s", cfg->tx_port);
    tx_ops.udp_port[MTL_SESSION_PORT_P] = cfg->tx_udp_port;

    tx_ops.type = ST22_TYPE_FRAME_LEVEL;
    tx_ops.pack_type = ST22_PACK_CODESTREAM;
    tx_ops.width = OUT_W;
    tx_ops.height = OUT_H;
    tx_ops.fps = fps_to_mtl(cfg->fps_num, cfg->fps_den);
    tx_ops.interlaced = false;
    tx_ops.fmt = ST20_FMT_YUV_422_10BIT;
    tx_ops.payload_type = cfg->payload_type;

    tx_ops.framebuff_cnt = (uint16_t)ctx->tx_fb_cnt;
    tx_ops.framebuff_max_size = MAX_CODESTREAM_SIZE;

    tx_ops.get_next_frame = st22_get_next_frame;
    tx_ops.notify_frame_done = st22_notify_frame_done;

    ctx->st22_tx = st22_tx_create(ctx->mtl, &tx_ops);
    if (!ctx->st22_tx) {
        fprintf(stderr, "[COMPTX] st22_tx_create failed\n");
        mtl_uninit(ctx->mtl);
        shm_ring_consumer_destroy(&ctx->shm_ring);
        return 1;
    }
    printf("[COMPTX] ST22 TX created: %dx%d@%u/%u → %u.%u.%u.%u:%u\n",
           OUT_W, OUT_H, cfg->fps_num, cfg->fps_den,
           cfg->tx_multicast[0], cfg->tx_multicast[1],
           cfg->tx_multicast[2], cfg->tx_multicast[3],
           cfg->tx_udp_port);

    /* ── 5. Main loop: read from shm ring → copy into ST22 TX fb ── */
    printf("\n[COMPTX] Running. Press Ctrl+C to stop.\n\n");
    time_t start_time = time(NULL);
    time_t last_stats = start_time;

    while (ctx->running) {
        /* Try to get next codestream from ring */
        int slot = shm_ring_consumer_try_peek(&ctx->shm_ring);
        if (slot < 0) {
            /* Check if compositor is done */
            if (shm_ring_consumer_done(&ctx->shm_ring)) {
                printf("[COMPTX] Compositor signaled done, exiting\n");
                break;
            }
            usleep(100);
            continue;
        }

        /* Freshness policy: if multiple slots are queued, drop stale ones
         * and keep only the newest frame. This bounds transit latency. */
        while (ctx->running) {
            uint64_t tail = atomic_load_explicit(&ctx->shm_ring.hdr->tail, memory_order_relaxed);
            uint64_t head = atomic_load_explicit(&ctx->shm_ring.hdr->head, memory_order_acquire);
            uint64_t backlog = (head > tail) ? (head - tail) : 0;
            if (backlog <= 1)
                break;
            shm_ring_consumer_release(&ctx->shm_ring);
            atomic_fetch_add(&ctx->frames_stale_dropped, 1);
            slot = shm_ring_consumer_try_peek(&ctx->shm_ring);
            if (slot < 0)
                break;
        }
        if (slot < 0)
            continue;

        /* Read slot metadata + payload */
        uint64_t t_pop_ns = now_ns();
        shm_slot_header_t *sh = shm_ring_slot_hdr(ctx->shm_ring.base, (uint32_t)slot);
        uint8_t *payload = shm_ring_slot_payload(ctx->shm_ring.base, (uint32_t)slot);
        uint32_t payload_size = sh->payload_size;
        sh->comptx_pop_ns = t_pop_ns;

        atomic_fetch_add(&ctx->frames_read, 1);

        /* Wait for ST22 TX framebuffer to be available */
        int spin = 0;
        while (atomic_load(&ctx->tx_fb_ready) >= 0 && ctx->running) {
            if (++spin > 10000) {
                usleep(100);
            } else {
                __asm__ volatile("pause");
            }
        }

        if (!ctx->running) {
            shm_ring_consumer_release(&ctx->shm_ring);
            break;
        }

        /* Copy codestream (with latency header) into ST22 TX fb */
        int fb_idx = ctx->tx_fb_write;
        uint8_t *fb_addr = (uint8_t *)st22_tx_get_fb_addr(ctx->st22_tx, (uint16_t)fb_idx);
        if (fb_addr) {
            /* Prepend latency header */
            poc_8k_frame_header_t hdr = {
                .encode_start_ns    = sh->encode_start_ns,
                .frame_idx          = sh->frame_idx,
                .codestream_size    = payload_size,
                .encode_duration_us = sh->encode_duration_us,
                .shm_commit_ns      = sh->shm_commit_ns,
                .comptx_pop_ns      = sh->comptx_pop_ns,
                .comptx_tx_ready_ns = now_ns(),
            };
            memcpy(fb_addr, &hdr, POC_8K_HEADER_SIZE);
            memcpy(fb_addr + POC_8K_HEADER_SIZE, payload, payload_size);
            ctx->tx_codestream_sz = POC_8K_HEADER_SIZE + payload_size;
            atomic_store(&ctx->tx_fb_ready, fb_idx);
            sh->comptx_tx_ready_ns = hdr.comptx_tx_ready_ns;
            ctx->tx_fb_write = (fb_idx + 1) % ctx->tx_fb_cnt;
        }

        /* Release shm slot back to producer */
        shm_ring_consumer_release(&ctx->shm_ring);

        /* Periodic stats */
        time_t now = time(NULL);
        if (now - last_stats >= 2) {
            uint64_t read_cnt = atomic_exchange(&ctx->frames_read, 0);
            uint64_t sent_cnt = atomic_exchange(&ctx->frames_sent, 0);
             uint64_t stale_drop_cnt = atomic_exchange(&ctx->frames_stale_dropped, 0);
            uint64_t interval = (uint64_t)(now - last_stats);
            double fps = (double)sent_cnt / (double)interval;

             printf("[COMPTX] %lds: read=%lu sent=%lu stale_drop=%lu fps=%.1f\n",
                   (long)(now - start_time),
                 (unsigned long)read_cnt, (unsigned long)sent_cnt,
                 (unsigned long)stale_drop_cnt, fps);
            last_stats = now;
        }

        /* Duration check */
        if (cfg->duration_sec > 0 &&
            (int)(now - start_time) >= cfg->duration_sec) {
            printf("[COMPTX] Duration reached (%ds)\n", cfg->duration_sec);
            break;
        }
    }

    /* ── Cleanup ── */
    printf("\n[COMPTX] Shutting down...\n");
    ctx->running = false;

    if (ctx->st22_tx)
        st22_tx_free(ctx->st22_tx);

    if (ctx->mtl)
        mtl_uninit(ctx->mtl);

    shm_ring_consumer_destroy(&ctx->shm_ring);

    printf("[COMPTX] Clean shutdown.\n");
    return 0;
}
