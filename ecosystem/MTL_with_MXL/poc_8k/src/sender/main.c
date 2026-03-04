/* SPDX-License-Identifier: BSD-3-Clause
 * poc_8k — ST22 RX → MXL RDMA Sender
 *
 * Receives compressed JPEG XS codestream from ST2110-22 multicast via
 * st22_rx (frame-level, no decode), and forwards the raw compressed
 * bytes over RDMA using MXL Fabrics.
 *
 * Data flow:
 *   ST2110-22 multicast → st22_rx (frame-level) → compressed codestream
 *   → memcpy into MXL grain → mxlFabricsInitiatorTransferGrain → RDMA
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <getopt.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <time.h>
#include <errno.h>
#include <stdatomic.h>
#include <immintrin.h>

/* ── Non-temporal memcpy — bypasses writer's L2 cache ──
 * Bridge thread copies codestream into RDMA-registered grain buffers.
 * The NIC DMA engine reads these for RDMA transfer — the bridge thread
 * never reads them back.  NT stores put data directly in LLC where
 * the NIC's DMA can fetch it, avoiding L2 pollution on the bridge thread.
 */
static inline void nt_memcpy(void *dst, const void *src, size_t n)
{
    const __m128i *s = (const __m128i *)src;
    __m128i       *d = (__m128i *)dst;
    size_t chunks = n / sizeof(__m128i);

    for (size_t i = 0; i < chunks; i++)
        _mm_stream_si128(d + i, _mm_loadu_si128(s + i));

    size_t tail = n & (sizeof(__m128i) - 1);
    if (tail)
        memcpy((uint8_t *)dst + n - tail, (const uint8_t *)src + n - tail, tail);

    _mm_sfence();
}

/* MTL */
#include <mtl/mtl_api.h>
#include <mtl/st20_api.h>

/* MXL */
#include <mxl/mxl.h>
#include <mxl/flow.h>
#include <mxl/flowinfo.h>
#include <mxl/fabrics.h>

/* ── Constants ── */
#define MAX_CODESTREAM_SIZE  (20 * 1024 * 1024)  /* 20 MB max compressed frame */
#define MAX_GRAIN_SLOTS      16

/* ── Latency header prepended by compositor_tx ── */
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
    char     port[64];           /* VF BDF for ST22 RX */
    uint8_t  sip[4];             /* Source IP for VF */
    uint8_t  rx_multicast[4];    /* Multicast group for ST22 RX */
    uint16_t rx_udp_port;        /* UDP port for ST22 RX */
    uint8_t  payload_type;       /* RTP payload type */
    char     lcores[128];        /* DPDK lcore list */

    /* MXL / RDMA */
    char     mxl_domain[256];    /* MXL domain path */
    char     flow_json_path[512];/* MXL flow JSON path */
    char     provider[32];       /* Fabrics provider (verbs/tcp) */
    char     local_ip[64];       /* RDMA sender IP */
    char     local_port[16];     /* Fabrics port (0=ephemeral) */
    char     target_info_file[512]; /* File with receiver target info */

    /* Video params (for ST22 RX) */
    uint32_t width;
    uint32_t height;
    uint32_t fps_num;
    uint32_t fps_den;

    int      duration_sec;       /* 0 = infinite */
} sender_config_t;

/* ── Sender context ── */
typedef struct {
    /* MTL */
    mtl_handle       mtl;
    st22_rx_handle   st22_rx;

    /* MXL */
    mxlInstance           mxl_instance;
    mxlFlowWriter         writer;
    mxlFlowConfigInfo     config_info;
    mxlFabricsInstance    fab_instance;
    mxlFabricsInitiator  initiator;
    mxlFabricsRegions    regions;
    mxlFabricsTargetInfo target_info;

    /* Grain management */
    struct {
        uint8_t *addr;
        size_t   buf_len;
    } grain_slots[MAX_GRAIN_SLOTS];
    uint32_t grain_count;

    /* Frame ring buffer (ST22 RX → bridge thread) */
    struct {
        uint8_t *buf;         /* copy of codestream */
        size_t   size;        /* actual codestream size */
        atomic_bool ready;    /* true when data is available */
    } frame_ring[4];
    atomic_int ring_write;    /* index for ST22 callback */
    atomic_int ring_read;     /* index for bridge thread */

    /* Runtime */
    volatile bool running;
    sender_config_t cfg;

    /* Stats */
    atomic_uint_fast64_t rx_frames;
    atomic_uint_fast64_t rdma_frames;
    atomic_uint_fast64_t rx_drops;
    atomic_uint_fast64_t rdma_transfer_us;  /* cumulative RDMA transfer time */
    atomic_uint_fast64_t ingest_path_us;    /* shm_commit -> sender ST22 RX callback */
    atomic_uint_fast64_t shm_to_comptx_us;  /* shm_commit -> compositor_tx pop */
    atomic_uint_fast64_t comptx_stage_us;   /* compositor_tx pop -> tx_ready */
    atomic_uint_fast64_t st22_path_us;      /* tx_ready -> sender ST22 RX callback */
    atomic_uint_fast64_t hdr_frames;        /* frames with valid extended header */
} sender_t;

static sender_t g_sender;

/* ── Signal handler ── */
static void sig_handler(int sig) { (void)sig; g_sender.running = false; }

/* ── Helpers ── */
static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000;
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

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

static int parse_ip(const char *str, uint8_t out[4])
{
    unsigned a, b, c, d;
    if (sscanf(str, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return -1;
    out[0] = (uint8_t)a; out[1] = (uint8_t)b;
    out[2] = (uint8_t)c; out[3] = (uint8_t)d;
    return 0;
}

static char *load_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(len + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, len, f);
    buf[n] = '\0';
    fclose(f);
    if (out_len) *out_len = n;
    /* Strip trailing whitespace */
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r' || buf[n-1] == ' '))
        buf[--n] = '\0';
    return buf;
}

/* ═══════════════════════════════════════════════
 * ST22 RX Callbacks (frame-level, no decode)
 * ═══════════════════════════════════════════════ */

/* ST22 RX uses push model: notify_frame_ready provides `void *frame` directly.
 * App must call st22_rx_put_framebuff() to return the frame when done. */

static int st22_rx_frame_ready(void *priv, void *frame,
                               struct st22_rx_frame_meta *meta)
{
    sender_t *s = (sender_t *)priv;

    if (!frame || meta->frame_total_size == 0) {
        st22_rx_put_framebuff(s->st22_rx, frame);
        return -1;
    }

    int w = atomic_load(&s->ring_write);
    if (atomic_load(&s->frame_ring[w].ready)) {
        /* Ring full — drop frame */
        atomic_fetch_add(&s->rx_drops, 1);
        st22_rx_put_framebuff(s->st22_rx, frame);
        return 0;
    }

    size_t cs_size = meta->frame_total_size;
    if (cs_size > MAX_CODESTREAM_SIZE) cs_size = MAX_CODESTREAM_SIZE;

    if (cs_size >= POC_8K_HEADER_SIZE) {
        poc_8k_frame_header_t hdr;
        memcpy(&hdr, frame, sizeof(hdr));
        uint64_t now = now_ns();
        bool valid = (hdr.shm_commit_ns > 0 && hdr.comptx_pop_ns > 0 &&
                      hdr.comptx_tx_ready_ns > 0 &&
                      hdr.shm_commit_ns <= hdr.comptx_pop_ns &&
                      hdr.comptx_pop_ns <= hdr.comptx_tx_ready_ns &&
                      hdr.comptx_tx_ready_ns <= now);
        if (valid) {
            atomic_fetch_add(&s->ingest_path_us, (now - hdr.shm_commit_ns) / 1000ULL);
            atomic_fetch_add(&s->shm_to_comptx_us, (hdr.comptx_pop_ns - hdr.shm_commit_ns) / 1000ULL);
            atomic_fetch_add(&s->comptx_stage_us, (hdr.comptx_tx_ready_ns - hdr.comptx_pop_ns) / 1000ULL);
            atomic_fetch_add(&s->st22_path_us, (now - hdr.comptx_tx_ready_ns) / 1000ULL);
            atomic_fetch_add(&s->hdr_frames, 1);
        }
    }

    memcpy(s->frame_ring[w].buf, frame, cs_size);
    s->frame_ring[w].size = cs_size;
    atomic_store(&s->frame_ring[w].ready, true);
    atomic_store(&s->ring_write, (w + 1) % 4);

    atomic_fetch_add(&s->rx_frames, 1);

    /* Return frame to MTL */
    st22_rx_put_framebuff(s->st22_rx, frame);
    return 0;
}

/* ═══════════════════════════════════════════════
 * MXL Bridge: init, connect, transfer
 * ═══════════════════════════════════════════════ */

static int mxl_init(sender_t *s)
{
    sender_config_t *cfg = &s->cfg;
    mxlStatus st;

    /* Load flow JSON */
    char *flow_json = load_file(cfg->flow_json_path, NULL);
    if (!flow_json) {
        fprintf(stderr, "[MXL] Failed to load flow JSON: %s\n", cfg->flow_json_path);
        return -1;
    }

    /* Create MXL instance */
    s->mxl_instance = mxlCreateInstance(cfg->mxl_domain, "");
    if (!s->mxl_instance) {
        fprintf(stderr, "[MXL] mxlCreateInstance failed\n");
        free(flow_json);
        return -1;
    }

    /* Create FlowWriter */
    bool created = false;
    st = mxlCreateFlowWriter(s->mxl_instance, flow_json, NULL,
                              &s->writer, &s->config_info, &created);
    free(flow_json);
    if (st != MXL_STATUS_OK) {
        fprintf(stderr, "[MXL] mxlCreateFlowWriter failed: %d\n", st);
        return -1;
    }

    s->grain_count = s->config_info.discrete.grainCount;
    printf("[MXL] FlowWriter ready (grainCount=%u, sliceSize[0]=%u)\n",
           s->grain_count, s->config_info.discrete.sliceSizes[0]);

    /* Pre-compute grain addresses */
    for (uint32_t i = 0; i < s->grain_count && i < MAX_GRAIN_SLOTS; i++) {
        mxlGrainInfo ginfo;
        uint8_t *payload = NULL;

        st = mxlFlowWriterOpenGrain(s->writer, i, &ginfo, &payload);
        if (st != MXL_STATUS_OK) {
            fprintf(stderr, "[MXL] OpenGrain(%u) failed: %d\n", i, st);
            return -1;
        }

        s->grain_slots[i].addr    = payload;
        s->grain_slots[i].buf_len = ginfo.grainSize;

        /* Empty commit */
        ginfo.validSlices = 0;
        mxlFlowWriterCommitGrain(s->writer, &ginfo);
    }

    /* Create Fabrics instance */
    st = mxlFabricsCreateInstance(s->mxl_instance, &s->fab_instance);
    if (st != MXL_STATUS_OK) {
        fprintf(stderr, "[MXL] mxlFabricsCreateInstance failed: %d\n", st);
        return -1;
    }

    /* Create Initiator */
    st = mxlFabricsCreateInitiator(s->fab_instance, &s->initiator);
    if (st != MXL_STATUS_OK) {
        fprintf(stderr, "[MXL] mxlFabricsCreateInitiator failed: %d\n", st);
        return -1;
    }

    /* Get regions */
    st = mxlFabricsRegionsForFlowWriter(s->writer, &s->regions);
    if (st != MXL_STATUS_OK) {
        fprintf(stderr, "[MXL] mxlFabricsRegionsForFlowWriter failed: %d\n", st);
        return -1;
    }

    /* Resolve provider */
    mxlFabricsProvider provider = MXL_FABRICS_PROVIDER_TCP;
    if (cfg->provider[0]) {
        mxlFabricsProviderFromString(cfg->provider, &provider);
    }

    /* Setup initiator */
    mxlFabricsInitiatorConfig init_cfg = {
        .endpointAddress = {
            .node    = cfg->local_ip,
            .service = cfg->local_port,
        },
        .provider      = provider,
        .regions       = s->regions,
        .deviceSupport = false,
    };

    st = mxlFabricsInitiatorSetup(s->initiator, &init_cfg);
    if (st != MXL_STATUS_OK) {
        fprintf(stderr, "[MXL] mxlFabricsInitiatorSetup failed: %d\n", st);
        return -1;
    }

    printf("[MXL] Initiator setup complete (provider=%s, local=%s:%s)\n",
           cfg->provider, cfg->local_ip, cfg->local_port);
    return 0;
}

static int mxl_connect(sender_t *s)
{
    mxlStatus st;

    /* Load target info */
    char *ti_str = load_file(s->cfg.target_info_file, NULL);
    if (!ti_str) {
        fprintf(stderr, "[MXL] Cannot read target info: %s\n",
                s->cfg.target_info_file);
        return -1;
    }

    st = mxlFabricsTargetInfoFromString(ti_str, &s->target_info);
    free(ti_str);
    if (st != MXL_STATUS_OK) {
        fprintf(stderr, "[MXL] mxlFabricsTargetInfoFromString failed: %d\n", st);
        return -1;
    }

    st = mxlFabricsInitiatorAddTarget(s->initiator, s->target_info);
    if (st != MXL_STATUS_OK) {
        fprintf(stderr, "[MXL] mxlFabricsInitiatorAddTarget failed: %d\n", st);
        return -1;
    }

    /* Connect */
    printf("[SENDER] Connecting to receiver...\n");
    uint64_t deadline = now_ms() + 15000;
    while (now_ms() < deadline) {
        st = mxlFabricsInitiatorMakeProgressNonBlocking(s->initiator);
        if (st == MXL_STATUS_OK) {
            printf("[SENDER] Connected to receiver!\n");
            return 0;
        }
        if (st != MXL_ERR_NOT_READY) {
            fprintf(stderr, "[SENDER] MakeProgress error: %d\n", st);
            return -1;
        }
        usleep(1000);
    }

    fprintf(stderr, "[SENDER] Timed out connecting to receiver\n");
    return -1;
}

static void mxl_cleanup(sender_t *s)
{
    if (s->initiator) {
        mxlFabricsDestroyInitiator(s->fab_instance, s->initiator);
        s->initiator = NULL;
    }
    if (s->fab_instance) {
        mxlFabricsDestroyInstance(s->fab_instance);
        s->fab_instance = NULL;
    }
    if (s->writer) {
        mxlReleaseFlowWriter(s->mxl_instance, s->writer);
        s->writer = NULL;
    }
    if (s->mxl_instance) {
        mxlDestroyInstance(s->mxl_instance);
        s->mxl_instance = NULL;
    }
}

/* ═══════════════════════════════════════════════
 * Bridge Thread: dequeue compressed frames, push via RDMA
 * ═══════════════════════════════════════════════ */

static void *bridge_thread(void *arg)
{
    sender_t *s = (sender_t *)arg;
    uint32_t grain_idx = 0;  /* monotonic grain index */
    int pending = 0;
    uint64_t t_rdma_start = 0;  /* RDMA transfer start timestamp (us) */

    printf("[BRIDGE] Worker thread started\n");

    while (s->running) {
        int r = atomic_load(&s->ring_read);

        /* Check if a frame is available */
        if (!atomic_load(&s->frame_ring[r].ready)) {
            /* Poll RDMA progress while waiting */
            if (pending > 0) {
                mxlStatus pst = mxlFabricsInitiatorMakeProgressNonBlocking(s->initiator);
                if (pst == MXL_STATUS_OK) {
                    atomic_fetch_add(&s->rdma_transfer_us, now_us() - t_rdma_start);
                    pending = 0;
                }
            }
            usleep(100);
            continue;
        }

        /* Wait for any pending RDMA transfer */
        if (pending > 0) {
            int spins = 0;
            while (s->running) {
                mxlStatus pst = mxlFabricsInitiatorMakeProgressNonBlocking(s->initiator);
                if (pst == MXL_STATUS_OK) {
                    atomic_fetch_add(&s->rdma_transfer_us, now_us() - t_rdma_start);
                    pending = 0;
                    break;
                }
                if (pst != MXL_ERR_NOT_READY) {
                    fprintf(stderr, "[BRIDGE] MakeProgress error: %d\n", pst);
                    s->running = false;
                    return NULL;
                }
                if (++spins > 10000) usleep(10);
            }
        }

        /* Copy codestream into MXL grain */
        size_t cs_size = s->frame_ring[r].size;

        mxlGrainInfo ginfo;
        uint8_t *payload = NULL;
        mxlStatus st = mxlFlowWriterOpenGrain(s->writer, grain_idx, &ginfo, &payload);
        if (st != MXL_STATUS_OK) {
            fprintf(stderr, "[BRIDGE] OpenGrain(%u) failed: %d\n", grain_idx, st);
            goto skip;
        }

        /* Copy compressed data into grain */
        if (cs_size > ginfo.grainSize) {
            fprintf(stderr, "[BRIDGE] Codestream %zu > grain %u, truncating\n",
                    cs_size, ginfo.grainSize);
            cs_size = ginfo.grainSize;
        }
        /* NT stores: bridge thread → NIC DMA (cross-core/device) */
        nt_memcpy(payload, s->frame_ring[r].buf, cs_size);

        /* Commit grain with ALL slices (not just 1 — grain has many 5120-byte slices) */
        ginfo.validSlices = ginfo.totalSlices;
        st = mxlFlowWriterCommitGrain(s->writer, &ginfo);
        if (st != MXL_STATUS_OK) {
            fprintf(stderr, "[BRIDGE] CommitGrain failed: %d\n", st);
            goto skip;
        }

        /* Trigger RDMA transfer — must transfer ALL slices, not just 1 */
        t_rdma_start = now_us();
        st = mxlFabricsInitiatorTransferGrain(s->initiator, grain_idx, 0,
                                               ginfo.totalSlices);
        if (st != MXL_STATUS_OK) {
            fprintf(stderr, "[BRIDGE] TransferGrain(%u) failed: %d\n", grain_idx, st);
            goto skip;
        }

        pending = 1;
        atomic_fetch_add(&s->rdma_frames, 1);

        /* Guard against ImmData uint16_t wrap — same as poc bridge */
        grain_idx++;
        if (grain_idx >= 65534) {
            grain_idx = s->grain_count;
        }

skip:
        /* Release ring slot */
        atomic_store(&s->frame_ring[r].ready, false);
        atomic_store(&s->ring_read, (r + 1) % 4);
    }

    /* Drain any final pending transfer */
    if (pending > 0) {
        for (int i = 0; i < 1000; i++) {
            if (mxlFabricsInitiatorMakeProgressNonBlocking(s->initiator) == MXL_STATUS_OK)
                break;
            usleep(1000);
        }
    }

    printf("[BRIDGE] Worker thread stopped\n");
    return NULL;
}

/* ═══════════════════════════════════════════════
 * CLI Argument Parsing
 * ═══════════════════════════════════════════════ */

static void set_defaults(sender_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));

    cfg->port[0] = '\0';
    memset(cfg->sip, 0, sizeof(cfg->sip));
    memset(cfg->rx_multicast, 0, sizeof(cfg->rx_multicast));
    cfg->rx_udp_port  = 30000;
    cfg->payload_type = 113;
    strncpy(cfg->lcores, "38,39", sizeof(cfg->lcores));

    strncpy(cfg->mxl_domain, "/dev/shm/mxl_8k_tx", sizeof(cfg->mxl_domain));
    strncpy(cfg->provider, "verbs", sizeof(cfg->provider));
    cfg->local_ip[0] = '\0';
    strncpy(cfg->local_port, "0", sizeof(cfg->local_port));

    cfg->width    = 7680;
    cfg->height   = 4320;
    cfg->fps_num  = 30000;
    cfg->fps_den  = 1001;
}

static void print_usage(const char *prog)
{
    printf("Usage: %s [options]\n"
           "  --port <BDF>          VF for ST22 RX (required, e.g. 0000:xx:02.0)\n"
           "  --sip <IP>            Source IP for VF (required, e.g. 192.168.1.x)\n"
           "  --rx-mcast <IP>       Multicast for ST22 RX (required, e.g. 239.0.4.1)\n"
           "  --rx-udp-port <N>     UDP port for ST22 RX (default: 30000)\n"
           "  --payload-type <N>    RTP payload type (default: 113)\n"
           "  --lcores <list>       DPDK lcore list (default: 38,39)\n"
           "  --mxl-domain <path>   MXL domain path\n"
           "  --flow-json <path>    MXL flow JSON path\n"
           "  --provider <name>     Fabrics provider (verbs/tcp)\n"
           "  --local-ip <IP>       RDMA sender IP\n"
           "  --local-port <N>      Fabrics port\n"
           "  --target-info-file <path>  Receiver target info file\n"
           "  --width <N>           Video width (default: 7680)\n"
           "  --height <N>          Video height (default: 4320)\n"
           "  --fps-num <N>         FPS numerator (default: 30000)\n"
           "  --fps-den <N>         FPS denominator (default: 1001)\n"
           "  --duration <sec>      Duration (0=infinite)\n"
           "  --help\n", prog);
}

static int parse_args(int argc, char **argv, sender_config_t *cfg)
{
    static struct option long_opts[] = {
        {"port",              required_argument, NULL, 1001},
        {"sip",               required_argument, NULL, 1002},
        {"rx-mcast",          required_argument, NULL, 1003},
        {"rx-udp-port",       required_argument, NULL, 1004},
        {"payload-type",      required_argument, NULL, 1005},
        {"lcores",            required_argument, NULL, 1006},
        {"mxl-domain",        required_argument, NULL, 1007},
        {"flow-json",         required_argument, NULL, 1008},
        {"provider",          required_argument, NULL, 1009},
        {"local-ip",          required_argument, NULL, 1010},
        {"local-port",        required_argument, NULL, 1011},
        {"target-info-file",  required_argument, NULL, 1012},
        {"width",             required_argument, NULL, 1013},
        {"height",            required_argument, NULL, 1014},
        {"fps-num",           required_argument, NULL, 1015},
        {"fps-den",           required_argument, NULL, 1016},
        {"duration",          required_argument, NULL, 1017},
        {"help",              no_argument,       NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "h", long_opts, NULL)) != -1) {
        switch (opt) {
        case 1001: strncpy(cfg->port, optarg, sizeof(cfg->port)-1); break;
        case 1002: parse_ip(optarg, cfg->sip); break;
        case 1003: parse_ip(optarg, cfg->rx_multicast); break;
        case 1004: cfg->rx_udp_port = (uint16_t)atoi(optarg); break;
        case 1005: cfg->payload_type = (uint8_t)atoi(optarg); break;
        case 1006: strncpy(cfg->lcores, optarg, sizeof(cfg->lcores)-1); break;
        case 1007: strncpy(cfg->mxl_domain, optarg, sizeof(cfg->mxl_domain)-1); break;
        case 1008: strncpy(cfg->flow_json_path, optarg, sizeof(cfg->flow_json_path)-1); break;
        case 1009: strncpy(cfg->provider, optarg, sizeof(cfg->provider)-1); break;
        case 1010: strncpy(cfg->local_ip, optarg, sizeof(cfg->local_ip)-1); break;
        case 1011: strncpy(cfg->local_port, optarg, sizeof(cfg->local_port)-1); break;
        case 1012: strncpy(cfg->target_info_file, optarg, sizeof(cfg->target_info_file)-1); break;
        case 1013: cfg->width = (uint32_t)atoi(optarg); break;
        case 1014: cfg->height = (uint32_t)atoi(optarg); break;
        case 1015: cfg->fps_num = (uint32_t)atoi(optarg); break;
        case 1016: cfg->fps_den = (uint32_t)atoi(optarg); break;
        case 1017: cfg->duration_sec = atoi(optarg); break;
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
    sender_t *s = &g_sender;
    memset(s, 0, sizeof(*s));
    s->running = true;

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    /* Parse config */
    set_defaults(&s->cfg);
    if (parse_args(argc, argv, &s->cfg) != 0)
        return 1;

    sender_config_t *cfg = &s->cfg;

    /* Validation */
    if (!cfg->port[0]) {
        fprintf(stderr, "Error: --port is required (e.g. 0000:xx:02.0)\n");
        return 1;
    }
    if (cfg->sip[0] == 0 && cfg->sip[1] == 0 &&
        cfg->sip[2] == 0 && cfg->sip[3] == 0) {
        fprintf(stderr, "Error: --sip is required (e.g. 192.168.1.x)\n");
        return 1;
    }
    if (cfg->rx_multicast[0] == 0 && cfg->rx_multicast[1] == 0 &&
        cfg->rx_multicast[2] == 0 && cfg->rx_multicast[3] == 0) {
        fprintf(stderr, "Error: --rx-mcast is required (e.g. 239.0.4.1)\n");
        return 1;
    }
    if (!cfg->local_ip[0]) {
        fprintf(stderr, "Error: --local-ip is required (e.g. 192.168.2.x)\n");
        return 1;
    }
    if (!cfg->flow_json_path[0]) {
        fprintf(stderr, "Error: --flow-json is required\n");
        return 1;
    }
    if (!cfg->target_info_file[0]) {
        fprintf(stderr, "Error: --target-info-file is required\n");
        return 1;
    }

    printf("\n══════════════════════════════════════════════════════\n");
    printf("  poc_8k Sender — ST22 RX → MXL RDMA TX\n");
    printf("══════════════════════════════════════════════════════\n");
    printf("  Port: %s (SIP: %u.%u.%u.%u)\n",
           cfg->port, cfg->sip[0], cfg->sip[1], cfg->sip[2], cfg->sip[3]);
    printf("  RX: %u.%u.%u.%u:%u (ST2110-22)\n",
           cfg->rx_multicast[0], cfg->rx_multicast[1],
           cfg->rx_multicast[2], cfg->rx_multicast[3],
           cfg->rx_udp_port);
    printf("  RDMA: %s → receiver\n", cfg->local_ip);
    printf("  Video: %ux%u @ %u/%u fps\n",
           cfg->width, cfg->height, cfg->fps_num, cfg->fps_den);
    printf("  Lcores: %s\n", cfg->lcores);
    printf("══════════════════════════════════════════════════════\n\n");

    /* Allocate frame ring buffers */
    for (int i = 0; i < 4; i++) {
        s->frame_ring[i].buf = malloc(MAX_CODESTREAM_SIZE);
        if (!s->frame_ring[i].buf) {
            fprintf(stderr, "Failed to allocate frame ring buffer\n");
            return 1;
        }
        atomic_init(&s->frame_ring[i].ready, false);
    }
    atomic_init(&s->ring_write, 0);
    atomic_init(&s->ring_read, 0);

    /* ── 1. Init MXL (before MTL, since we don't need DMA mapping) ── */
    if (mxl_init(s) != 0) {
        fprintf(stderr, "[SENDER] MXL init failed\n");
        goto cleanup;
    }

    /* ── 2. Init MTL ── */
    struct mtl_init_params p;
    memset(&p, 0, sizeof(p));

    p.num_ports = 1;
    snprintf(p.port[MTL_PORT_P], MTL_PORT_MAX_LEN, "%s", cfg->port);
    memcpy(p.sip_addr[MTL_PORT_P], cfg->sip, MTL_IP_ADDR_LEN);

    p.flags = MTL_FLAG_DEV_AUTO_START_STOP;
    p.log_level = MTL_LOG_LEVEL_WARNING;
    p.data_quota_mbs_per_sch = 40000;  /* all sessions on 1 scheduler */
    p.nb_rx_desc = 4096;

    mtl_para_rx_queues_cnt_set(&p, MTL_PORT_P, 1);
    mtl_para_tx_queues_cnt_set(&p, MTL_PORT_P, 0);

    if (cfg->lcores[0])
        p.lcores = (char *)cfg->lcores;

    s->mtl = mtl_init(&p);
    if (!s->mtl) {
        fprintf(stderr, "[SENDER] mtl_init failed\n");
        goto cleanup;
    }
    printf("[SENDER] MTL initialized (port=%s)\n", cfg->port);

    /* ── 3. Create ST22 RX (frame-level, no decode) ── */
    struct st22_rx_ops rx_ops;
    memset(&rx_ops, 0, sizeof(rx_ops));

    rx_ops.name = "8k_st22_rx";
    rx_ops.priv = s;
    rx_ops.num_port = 1;

    memcpy(rx_ops.ip_addr[MTL_SESSION_PORT_P], cfg->rx_multicast, MTL_IP_ADDR_LEN);
    snprintf(rx_ops.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s", cfg->port);
    rx_ops.udp_port[MTL_SESSION_PORT_P] = cfg->rx_udp_port;

    rx_ops.type = ST22_TYPE_FRAME_LEVEL;
    rx_ops.pack_type = ST22_PACK_CODESTREAM;
    rx_ops.width = cfg->width;
    rx_ops.height = cfg->height;
    rx_ops.fps = fps_to_mtl(cfg->fps_num, cfg->fps_den);
    rx_ops.interlaced = false;
    rx_ops.fmt = ST20_FMT_YUV_422_10BIT;
    rx_ops.payload_type = cfg->payload_type;

    rx_ops.framebuff_cnt = 4;
    rx_ops.framebuff_max_size = MAX_CODESTREAM_SIZE;

    rx_ops.notify_frame_ready = st22_rx_frame_ready;

    s->st22_rx = st22_rx_create(s->mtl, &rx_ops);
    if (!s->st22_rx) {
        fprintf(stderr, "[SENDER] st22_rx_create failed\n");
        goto cleanup;
    }
    printf("[SENDER] ST22 RX created → %u.%u.%u.%u:%u\n",
           cfg->rx_multicast[0], cfg->rx_multicast[1],
           cfg->rx_multicast[2], cfg->rx_multicast[3],
           cfg->rx_udp_port);

    /* ── 4. Connect MXL to receiver ── */
    if (mxl_connect(s) != 0) {
        fprintf(stderr, "[SENDER] MXL connect failed\n");
        goto cleanup;
    }

    /* ── 5. Start bridge thread ── */
    pthread_t bridge_tid;
    if (pthread_create(&bridge_tid, NULL, bridge_thread, s) != 0) {
        perror("pthread_create bridge");
        goto cleanup;
    }

    printf("\n[SENDER] Running. Press Ctrl+C to stop.\n\n");

    /* ── 6. Main loop: stats ── */
    time_t start = time(NULL);
    uint64_t prev_rdma = 0, prev_rdma_us = 0;
    time_t prev_elapsed = 0;
    uint64_t prev_hdr_frames = 0;
    uint64_t prev_ingest_us = 0, prev_shm_to_comptx_us = 0;
    uint64_t prev_comptx_stage_us = 0, prev_st22_path_us = 0;
    double win_rdma_avg_ms = 0.0;
    double win_ingest_avg_ms = 0.0;
    double win_shm_to_comptx_ms = 0.0;
    double win_comptx_stage_ms = 0.0;
    double win_st22_path_ms = 0.0;
    const char *stats_path = "/dev/shm/poc_8k_sender_stats.json";
    const char *stats_tmp  = "/dev/shm/poc_8k_sender_stats.json.tmp";
    while (s->running) {
        sleep(2);

        time_t elapsed = time(NULL) - start;
        uint64_t rx  = atomic_load(&s->rx_frames);
        uint64_t rdma = atomic_load(&s->rdma_frames);
        uint64_t drops = atomic_load(&s->rx_drops);
        uint64_t rdma_us = atomic_load(&s->rdma_transfer_us);
        uint64_t hdr_frames = atomic_load(&s->hdr_frames);
        uint64_t ingest_us = atomic_load(&s->ingest_path_us);
        uint64_t shm_to_comptx_us = atomic_load(&s->shm_to_comptx_us);
        uint64_t comptx_stage_us = atomic_load(&s->comptx_stage_us);
        uint64_t st22_path_us = atomic_load(&s->st22_path_us);

        /* Windowed RDMA latency (delta since last stats print) */
        uint64_t d_rdma = rdma - prev_rdma;
        uint64_t d_us   = rdma_us - prev_rdma_us;
        if (d_rdma > 0)
            win_rdma_avg_ms = (double)d_us / d_rdma / 1000.0;
        prev_rdma = rdma;
        prev_rdma_us = rdma_us;

        uint64_t d_hdr = hdr_frames - prev_hdr_frames;
        if (d_hdr > 0) {
            win_ingest_avg_ms = (double)(ingest_us - prev_ingest_us) / d_hdr / 1000.0;
            win_shm_to_comptx_ms = (double)(shm_to_comptx_us - prev_shm_to_comptx_us) / d_hdr / 1000.0;
            win_comptx_stage_ms = (double)(comptx_stage_us - prev_comptx_stage_us) / d_hdr / 1000.0;
            win_st22_path_ms = (double)(st22_path_us - prev_st22_path_us) / d_hdr / 1000.0;
        }
        prev_hdr_frames = hdr_frames;
        prev_ingest_us = ingest_us;
        prev_shm_to_comptx_us = shm_to_comptx_us;
        prev_comptx_stage_us = comptx_stage_us;
        prev_st22_path_us = st22_path_us;

        time_t d_elapsed = elapsed - prev_elapsed;
        double fps_delta = (d_elapsed > 0) ? (double)d_rdma / (double)d_elapsed : 0.0;
        double fps_cum = (elapsed > 0) ? (double)rdma / (double)elapsed : 0.0;
        prev_elapsed = elapsed;

        double cum_rdma_avg = rdma > 0 ? (double)rdma_us / rdma / 1000.0 : 0.0;

        printf("[SENDER] %lds: rx=%lu rdma=%lu drops=%lu fps_delta=%.1f fps_cum=%.1f rdma_avg=%.1fms "
               "ingest=%.1fms (shm->comptx=%.1f comptx=%.1f st22=%.1f)\n",
               (long)elapsed, (unsigned long)rx, (unsigned long)rdma,
               (unsigned long)drops,
               fps_delta,
               fps_cum,
               cum_rdma_avg,
               win_ingest_avg_ms,
               win_shm_to_comptx_ms,
               win_comptx_stage_ms,
               win_st22_path_ms);

        /* Write stats file for dashboard consumption (atomic rename) */
        FILE *sf = fopen(stats_tmp, "w");
        if (sf) {
            fprintf(sf, "{\"rdma_avg_ms\":%.2f,\"ingest_avg_ms\":%.2f,"
                        "\"shm_to_comptx_ms\":%.2f,\"comptx_stage_ms\":%.2f,"
                        "\"st22_path_ms\":%.2f,\"rx_frames\":%lu,"
                        "\"rdma_frames\":%lu,\"hdr_frames\":%lu,"
                        "\"drops\":%lu,\"fps\":%.1f,\"fps_cum\":%.1f}\n",
                    win_rdma_avg_ms, win_ingest_avg_ms,
                    win_shm_to_comptx_ms,
                    win_comptx_stage_ms, win_st22_path_ms,
                    (unsigned long)rx,
                    (unsigned long)rdma, (unsigned long)hdr_frames,
                    (unsigned long)drops,
                    fps_delta,
                    fps_cum);
            fclose(sf);
            rename(stats_tmp, stats_path);
        }

        if (cfg->duration_sec > 0 && (int)elapsed >= cfg->duration_sec) {
            printf("[SENDER] Duration reached (%ds)\n", cfg->duration_sec);
            break;
        }
    }

    /* ── Cleanup ── */
    s->running = false;
    pthread_join(bridge_tid, NULL);

cleanup:
    if (s->st22_rx)
        st22_rx_free(s->st22_rx);
    if (s->mtl)
        mtl_uninit(s->mtl);
    mxl_cleanup(s);
    for (int i = 0; i < 4; i++)
        free(s->frame_ring[i].buf);

    printf("[SENDER] Clean shutdown.\n");
    return 0;
}
