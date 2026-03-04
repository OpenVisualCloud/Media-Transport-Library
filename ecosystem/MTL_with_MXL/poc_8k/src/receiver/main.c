/* SPDX-License-Identifier: BSD-3-Clause
 * poc_8k — MXL RDMA Receiver with JPEG XS Decode
 *
 * Receives compressed JPEG XS codestream via MXL RDMA,
 * decodes with SVT-JPEG-XS to YUV, generates JPEG thumbnails,
 * and serves them via a built-in HTTP server.
 *
 * Data flow:
 *   MXL RDMA RX → grain (compressed JPEG XS codestream)
 *   → SVT-JPEG-XS decode → YUV422P10LE 8K
 *   → downscale to 960×540 RGB → JPEG → /dev/shm/poc_8k_thumbs/thumb.jpg
 *   → HTTP /snapshot (JPEG), /stream (MJPEG)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <getopt.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <errno.h>
#include <stdatomic.h>
#include <sched.h>

/* MXL */
#include <mxl/mxl.h>
#include <mxl/flow.h>
#include <mxl/flowinfo.h>
#include <mxl/fabrics.h>

/* SVT-JPEG-XS Decoder */
#include <svt-jpegxs/SvtJpegxs.h>
#include <svt-jpegxs/SvtJpegxsDec.h>

/* libjpeg */
#include <jpeglib.h>

/* ── Constants ── */
#define MAX_CODESTREAM_SIZE  (20 * 1024 * 1024)   /* 20 MB */
#define DECODE_QUEUE_DEPTH   4  /* async decode pipeline depth */

/* ── Latency tracking header — prepended to each codestream by compositor ──
 * Same struct as in compositor/main.c. All poc_8k processes share CLOCK_MONOTONIC. */
typedef struct __attribute__((packed)) {
    uint64_t encode_start_ns;   /* CLOCK_MONOTONIC ns at compositor pre-encode */
    uint32_t frame_idx;         /* monotonic compositor frame counter */
    uint32_t codestream_size;   /* actual JPEG XS codestream bytes (excl. header) */
    uint32_t encode_duration_us;/* encode time in microseconds */
    uint64_t shm_commit_ns;
    uint64_t comptx_pop_ns;
    uint64_t comptx_tx_ready_ns;
} poc_8k_frame_header_t;
#define POC_8K_HEADER_SIZE  ((uint32_t)sizeof(poc_8k_frame_header_t))

/* 8K output */
#define SRC_W  7680
#define SRC_H  4320

/* Thumbnail */
#define THUMB_W       960
#define THUMB_H       540
#define THUMB_QUALITY 75
#define THUMB_RGB_SIZE ((size_t)THUMB_W * THUMB_H * 3)

/* ── Configuration ── */
typedef struct {
    char     mxl_domain[256];
    char     flow_json_path[512];
    char     provider[32];
    char     bind_ip[64];
    char     bind_port[16];

    int      decoder_threads;
    char     thumb_dir[256];
    int      http_port;
    int      duration_sec;
} receiver_config_t;

/* ── Thumbnail triple-buffer ── */
typedef struct {
    uint8_t *hot;        /* writer fills this */
    uint8_t *ready;      /* swapped after fill, encoder reads */
    uint8_t *encode_buf; /* encoder working buffer */
    pthread_mutex_t  lock;
    pthread_cond_t   cond;
    volatile bool    new_frame;
    volatile bool    running;
    pthread_t        thread;
    char             dir[256];
} thumb_ctx_t;

/* ── Async decode queue ── */
typedef struct {
    uint8_t              *data;     /* copied codestream */
    size_t                size;     /* codestream bytes  */
    poc_8k_frame_header_t hdr;      /* latency header    */
} decode_slot_t;

typedef struct {
    decode_slot_t   slots[DECODE_QUEUE_DEPTH];
    int             head;       /* next write position */
    int             tail;       /* next read position  */
    int             count;      /* filled slots        */
    pthread_mutex_t lock;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
    volatile bool   running;
    pthread_t       thread;
} decode_queue_t;

/* ── Receiver context ── */
typedef struct {
    /* MXL */
    mxlInstance          mxl_instance;
    mxlFlowWriter        writer;
    mxlFlowConfigInfo    config_info;
    mxlFlowReader        reader;
    mxlFabricsInstance   fab_instance;
    mxlFabricsTarget     target;
    mxlFabricsTargetInfo target_info;
    mxlFabricsRegions    regions;

    /* Decoder */
    svt_jpeg_xs_decoder_api_t dec_api;
    bool decoder_initialized;

    /* Decoded frame buffer (YUV422P10LE, 8K) */
    uint16_t *decoded_y;
    uint16_t *decoded_u;
    uint16_t *decoded_v;
    size_t    decoded_y_size;
    size_t    decoded_uv_size;

    /* Thumbnail */
    thumb_ctx_t thumb;

    /* Async decode */
    decode_queue_t dq;

    /* HTTP */
    int http_fd;

    /* Runtime */
    volatile bool running;
    receiver_config_t cfg;

    /* Stats */
    atomic_uint_fast64_t rdma_frames;
    atomic_uint_fast64_t decoded_frames;
    atomic_uint_fast64_t decode_errors;
    atomic_uint_fast64_t decode_time_us;
    atomic_uint_fast64_t encode_time_us;   /* cumulative encode latency (from header) */
    atomic_uint_fast64_t e2e_latency_us;   /* cumulative end-to-end latency */

    /* Windowed stats (updated every 2s) */
    struct {
        double avg_decode_ms;
        double avg_encode_ms;
        double avg_rdma_ms;     /* residual: e2e - enc - dec (legacy/fallback) */
        double avg_e2e_ms;
        double real_rdma_ms;   /* actual RDMA transfer from sender stats file */
        double transit_ms;     /* sender ingest path: shm_commit -> sender RX */
        double shm_to_comptx_ms;
        double comptx_stage_ms;
        double st22_path_ms;
        double sender_fps_delta;
        double sender_fps_cum;
        double fps;
        /* snapshot of cumulative counters at last window boundary */
        uint64_t prev_decoded;
        uint64_t prev_decode_us;
        uint64_t prev_encode_us;
        uint64_t prev_e2e_us;
        uint64_t prev_time_us;
    } win;
} receiver_t;

static receiver_t g_recv;

/* ── Signal handler ── */
static void sig_handler(int sig) { (void)sig; g_recv.running = false; }

/* Ignore SIGPIPE globally (MJPEG stream writes to disconnected clients) */
static void ignore_sigpipe(void) { signal(SIGPIPE, SIG_IGN); }

/* ── Helpers ── */
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

static bool json_get_double(const char *json, const char *key, double *out)
{
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(json, pat);
    if (!p) return false;
    *out = atof(p + strlen(pat));
    return true;
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
    return buf;
}

/* ═══════════════════════════════════════════════
 * Thumbnail: YUV422P10LE → downscale → RGB → JPEG
 * ═══════════════════════════════════════════════ */

static void yuv422p10le_to_rgb_thumb(const uint16_t *y_plane,
                                     const uint16_t *u_plane,
                                     const uint16_t *v_plane,
                                     int src_w, int src_h,
                                     uint8_t *rgb_out,
                                     int dst_w, int dst_h)
{
    /* Nearest-neighbour downscale + YUV→RGB conversion */
    float x_ratio = (float)src_w / dst_w;
    float y_ratio = (float)src_h / dst_h;

    for (int dy = 0; dy < dst_h; dy++) {
        int sy = (int)(dy * y_ratio);
        if (sy >= src_h) sy = src_h - 1;

        const uint16_t *y_row = y_plane + sy * src_w;
        const uint16_t *u_row = u_plane + sy * (src_w / 2);
        const uint16_t *v_row = v_plane + sy * (src_w / 2);

        for (int dx = 0; dx < dst_w; dx++) {
            int sx = (int)(dx * x_ratio);
            if (sx >= src_w) sx = src_w - 1;

            /* 10-bit→8-bit conversion */
            int y_val = y_row[sx] >> 2;
            int u_val = u_row[sx / 2] >> 2;
            int v_val = v_row[sx / 2] >> 2;

            /* BT.709 YUV→RGB */
            int c = y_val - 16;
            int d = u_val - 128;
            int e = v_val - 128;

            int r = (298 * c + 409 * e + 128) >> 8;
            int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
            int b = (298 * c + 516 * d + 128) >> 8;

            uint8_t *p = rgb_out + (dy * dst_w + dx) * 3;
            p[0] = (uint8_t)(r < 0 ? 0 : (r > 255 ? 255 : r));
            p[1] = (uint8_t)(g < 0 ? 0 : (g > 255 ? 255 : g));
            p[2] = (uint8_t)(b < 0 ? 0 : (b > 255 ? 255 : b));
        }
    }
}

static int write_jpeg(const char *path, const uint8_t *rgb,
                      int width, int height, int quality)
{
    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

    FILE *f = fopen(tmp_path, "wb");
    if (!f) return -1;

    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);
    jpeg_stdio_dest(&cinfo, f);

    cinfo.image_width = width;
    cinfo.image_height = height;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, TRUE);
    jpeg_start_compress(&cinfo, TRUE);

    while (cinfo.next_scanline < cinfo.image_height) {
        const uint8_t *row = rgb + cinfo.next_scanline * width * 3;
        JSAMPROW row_ptr = (JSAMPROW)row;
        jpeg_write_scanlines(&cinfo, &row_ptr, 1);
    }

    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    fclose(f);

    rename(tmp_path, path);
    return 0;
}

/* Thumbnail background encoder thread */
static void *thumb_encode_thread(void *arg)
{
    thumb_ctx_t *tc = (thumb_ctx_t *)arg;
    char path[512];
    snprintf(path, sizeof(path), "%s/thumb.jpg", tc->dir);

    printf("[THUMB] Encoder thread started → %s\n", path);

    while (tc->running) {
        pthread_mutex_lock(&tc->lock);
        while (!tc->new_frame && tc->running) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 100000000;  /* 100 ms timeout */
            if (ts.tv_nsec >= 1000000000) {
                ts.tv_sec += 1;
                ts.tv_nsec -= 1000000000;
            }
            pthread_cond_timedwait(&tc->cond, &tc->lock, &ts);
        }
        if (!tc->running) {
            pthread_mutex_unlock(&tc->lock);
            break;
        }
        tc->new_frame = false;

        /* Swap ready → encode_buf */
        uint8_t *tmp = tc->encode_buf;
        tc->encode_buf = tc->ready;
        tc->ready = tmp;
        pthread_mutex_unlock(&tc->lock);

        /* JPEG encode + write */
        write_jpeg(path, tc->encode_buf, THUMB_W, THUMB_H, THUMB_QUALITY);
    }

    printf("[THUMB] Encoder thread stopped\n");
    return NULL;
}

static int thumb_init(thumb_ctx_t *tc, const char *dir)
{
    memset(tc, 0, sizeof(*tc));
    strncpy(tc->dir, dir, sizeof(tc->dir) - 1);

    mkdir(dir, 0755);

    tc->hot        = calloc(1, THUMB_RGB_SIZE);
    tc->ready      = calloc(1, THUMB_RGB_SIZE);
    tc->encode_buf = calloc(1, THUMB_RGB_SIZE);
    if (!tc->hot || !tc->ready || !tc->encode_buf)
        return -1;

    pthread_mutex_init(&tc->lock, NULL);
    pthread_cond_init(&tc->cond, NULL);
    tc->running = true;
    tc->new_frame = false;

    if (pthread_create(&tc->thread, NULL, thumb_encode_thread, tc) != 0)
        return -1;

    return 0;
}

static void thumb_submit(thumb_ctx_t *tc, const uint16_t *y_plane,
                         const uint16_t *u_plane, const uint16_t *v_plane,
                         int src_w, int src_h)
{
    /* Downscale + convert into hot buffer */
    yuv422p10le_to_rgb_thumb(y_plane, u_plane, v_plane,
                             src_w, src_h,
                             tc->hot, THUMB_W, THUMB_H);

    /* Swap hot → ready, signal encoder */
    pthread_mutex_lock(&tc->lock);
    uint8_t *tmp = tc->hot;
    tc->hot = tc->ready;
    tc->ready = tmp;
    tc->new_frame = true;
    pthread_cond_signal(&tc->cond);
    pthread_mutex_unlock(&tc->lock);
}

static void thumb_cleanup(thumb_ctx_t *tc)
{
    tc->running = false;
    pthread_cond_signal(&tc->cond);
    pthread_join(tc->thread, NULL);
    pthread_mutex_destroy(&tc->lock);
    pthread_cond_destroy(&tc->cond);
    free(tc->hot);
    free(tc->ready);
    free(tc->encode_buf);
}

/* ═══════════════════════════════════════════════
 * MXL Sink + Fabrics Target (receiver side)
 * ═══════════════════════════════════════════════ */

static int mxl_init(receiver_t *r)
{
    receiver_config_t *cfg = &r->cfg;
    mxlStatus st;

    /* Load flow JSON */
    char *flow_json = load_file(cfg->flow_json_path, NULL);
    if (!flow_json) {
        fprintf(stderr, "[MXL] Failed to load flow JSON: %s\n", cfg->flow_json_path);
        return -1;
    }

    /* Create MXL instance */
    r->mxl_instance = mxlCreateInstance(cfg->mxl_domain, "");
    if (!r->mxl_instance) {
        fprintf(stderr, "[MXL] mxlCreateInstance failed\n");
        free(flow_json);
        return -1;
    }

    /* Create FlowWriter (allocates grain ring buffer) */
    bool created = false;
    st = mxlCreateFlowWriter(r->mxl_instance, flow_json, NULL,
                              &r->writer, &r->config_info, &created);
    free(flow_json);
    if (st != MXL_STATUS_OK) {
        fprintf(stderr, "[MXL] mxlCreateFlowWriter failed: %d\n", st);
        return -1;
    }

    printf("[MXL] FlowWriter ready (grainCount=%u, sliceSize[0]=%u)\n",
           r->config_info.discrete.grainCount,
           r->config_info.discrete.sliceSizes[0]);

    /* Create FlowReader for local consumption */
    {
        uint8_t *id = r->config_info.common.id;
        char flow_id_str[64];
        snprintf(flow_id_str, sizeof(flow_id_str),
                 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                 id[0],id[1],id[2],id[3], id[4],id[5], id[6],id[7],
                 id[8],id[9], id[10],id[11],id[12],id[13],id[14],id[15]);
        st = mxlCreateFlowReader(r->mxl_instance, flow_id_str, "", &r->reader);
        if (st != MXL_STATUS_OK) {
            fprintf(stderr, "[MXL] mxlCreateFlowReader failed: %d (continuing)\n", st);
            r->reader = NULL;
        }
    }

    /* Fabrics instance */
    st = mxlFabricsCreateInstance(r->mxl_instance, &r->fab_instance);
    if (st != MXL_STATUS_OK) {
        fprintf(stderr, "[MXL] mxlFabricsCreateInstance failed: %d\n", st);
        return -1;
    }

    /* Create target */
    st = mxlFabricsCreateTarget(r->fab_instance, &r->target);
    if (st != MXL_STATUS_OK) {
        fprintf(stderr, "[MXL] mxlFabricsCreateTarget failed: %d\n", st);
        return -1;
    }

    /* Get memory regions */
    st = mxlFabricsRegionsForFlowWriter(r->writer, &r->regions);
    if (st != MXL_STATUS_OK) {
        fprintf(stderr, "[MXL] mxlFabricsRegionsForFlowWriter failed: %d\n", st);
        return -1;
    }

    /* Resolve provider */
    mxlFabricsProvider provider = MXL_FABRICS_PROVIDER_TCP;
    if (cfg->provider[0]) {
        mxlFabricsProviderFromString(cfg->provider, &provider);
    }

    /* Setup target */
    mxlFabricsTargetConfig target_cfg = {
        .endpointAddress = {
            .node    = cfg->bind_ip,
            .service = cfg->bind_port,
        },
        .provider      = provider,
        .regions       = r->regions,
        .deviceSupport = false,
    };

    st = mxlFabricsTargetSetup(r->target, &target_cfg, &r->target_info);
    if (st != MXL_STATUS_OK) {
        /* RDMA port may still be held by kernel after previous process was killed.
         * Retry a few times with increasing delay. */
        for (int retry = 1; retry <= 10 && st != MXL_STATUS_OK; retry++) {
            fprintf(stderr, "[MXL] mxlFabricsTargetSetup failed (attempt %d/10), "
                    "retrying in %ds...\n", retry, retry);
            sleep(retry);
            st = mxlFabricsTargetSetup(r->target, &target_cfg, &r->target_info);
        }
        if (st != MXL_STATUS_OK) {
            fprintf(stderr, "[MXL] mxlFabricsTargetSetup failed after 10 retries: %d\n", st);
            return -1;
        }
    }

    /* Serialize target info and write to file + stdout */
    size_t ti_len = 0;
    st = mxlFabricsTargetInfoToString(r->target_info, NULL, &ti_len);
    if (st != MXL_STATUS_OK && ti_len == 0) {
        fprintf(stderr, "[MXL] TargetInfoToString size query failed: %d\n", st);
        return -1;
    }

    char *ti_str = malloc(ti_len + 1);
    if (!ti_str) return -1;

    st = mxlFabricsTargetInfoToString(r->target_info, ti_str, &ti_len);
    if (st != MXL_STATUS_OK) {
        fprintf(stderr, "[MXL] TargetInfoToString failed: %d\n", st);
        free(ti_str);
        return -1;
    }
    ti_str[ti_len] = '\0';

    printf("\n[TARGET] ═══════════════ TARGET INFO ═══════════════\n");
    printf("%s\n", ti_str);
    printf("[TARGET] ════════════════════════════════════════════\n\n");

    /* Write to file for sender to read */
    char ti_path[512];
    snprintf(ti_path, sizeof(ti_path), "/dev/shm/poc_8k_target_info.json");
    FILE *fp = fopen(ti_path, "w");
    if (fp) {
        fprintf(fp, "%s\n", ti_str);
        fclose(fp);
        printf("[TARGET] Written to %s\n", ti_path);
    }

    free(ti_str);

    printf("[MXL] Target setup complete (provider=%s, bind=%s:%s)\n",
           cfg->provider, cfg->bind_ip, cfg->bind_port);
    return 0;
}

static void mxl_cleanup(receiver_t *r)
{
    if (r->target_info) {
        mxlFabricsFreeTargetInfo(r->target_info);
        r->target_info = NULL;
    }
    if (r->target) {
        mxlFabricsDestroyTarget(r->fab_instance, r->target);
        r->target = NULL;
    }
    if (r->regions) {
        mxlFabricsRegionsFree(r->regions);
        r->regions = NULL;
    }
    if (r->fab_instance) {
        mxlFabricsDestroyInstance(r->fab_instance);
        r->fab_instance = NULL;
    }
    if (r->reader) {
        mxlReleaseFlowReader(r->mxl_instance, r->reader);
        r->reader = NULL;
    }
    if (r->writer) {
        mxlReleaseFlowWriter(r->mxl_instance, r->writer);
        r->writer = NULL;
    }
    if (r->mxl_instance) {
        mxlDestroyInstance(r->mxl_instance);
        r->mxl_instance = NULL;
    }
}

/* ═══════════════════════════════════════════════
 * SVT-JPEG-XS Decoder
 * ═══════════════════════════════════════════════ */

static int decoder_init_from_codestream(receiver_t *r, const uint8_t *bitstream,
                                        size_t size)
{
    svt_jpeg_xs_decoder_api_t *dec = &r->dec_api;
    memset(dec, 0, sizeof(*dec));

    dec->threads_num = (uint32_t)r->cfg.decoder_threads;
    dec->verbose = 0;
    dec->packetization_mode = 0;  /* frame-based */

    svt_jpeg_xs_image_config_t img_cfg;
    memset(&img_cfg, 0, sizeof(img_cfg));

    /* Pin decoder threads to NUMA 1 cores starting at 56 by restricting
     * affinity before SVT init (threads inherit the creating thread's cpuset). */
    int n_dec = r->cfg.decoder_threads;
    if (n_dec > 56) n_dec = 56;  /* cap at NUMA1 physical core count */
    cpu_set_t old_set, pin_set;
    CPU_ZERO(&pin_set);
    for (int c = 56; c < 56 + n_dec; c++)
        CPU_SET(c, &pin_set);
    sched_getaffinity(0, sizeof(old_set), &old_set);
    sched_setaffinity(0, sizeof(pin_set), &pin_set);
    printf("[DEC] Decoder threads pinned to NUMA1 cores 56-%d (%d threads)\n",
           56 + n_dec - 1, n_dec);

    SvtJxsErrorType_t err = svt_jpeg_xs_decoder_init(
        SVT_JPEGXS_API_VER_MAJOR, SVT_JPEGXS_API_VER_MINOR,
        dec, bitstream, size, &img_cfg);

    /* Restore full affinity for the main consumer thread */
    sched_setaffinity(0, sizeof(old_set), &old_set);

    if (err != SvtJxsErrorNone) {
        fprintf(stderr, "[DEC] decoder_init failed: %d\n", err);
        return -1;
    }

    printf("[DEC] Initialized: %ux%u %dbit %dcomp, threads=%d\n",
           img_cfg.width, img_cfg.height, img_cfg.bit_depth,
           img_cfg.components_num, r->cfg.decoder_threads);

    /* Allocate decoded frame buffers */
    r->decoded_y_size  = (size_t)img_cfg.width * img_cfg.height * sizeof(uint16_t);
    r->decoded_uv_size = (size_t)(img_cfg.width / 2) * img_cfg.height * sizeof(uint16_t);

    r->decoded_y = (uint16_t *)calloc(1, r->decoded_y_size);
    r->decoded_u = (uint16_t *)calloc(1, r->decoded_uv_size);
    r->decoded_v = (uint16_t *)calloc(1, r->decoded_uv_size);
    if (!r->decoded_y || !r->decoded_u || !r->decoded_v) {
        fprintf(stderr, "[DEC] Failed to allocate decoded frame buffers\n");
        return -1;
    }

    r->decoder_initialized = true;
    return 0;
}

static int decoder_decode(receiver_t *r, const uint8_t *bitstream, size_t size)
{
    static int log_count = 0;

    /* Determine exact codestream frame size (SOC through EOC).
     * SVT-JPEG-XS requires used_size to be EXACTLY the codestream size.
     * The encoder may pad output beyond the EOC marker. */
    uint32_t frame_size = 0;
    SvtJxsErrorType_t fserr = svt_jpeg_xs_decoder_get_single_frame_size(
        (uint8_t *)bitstream, (uint32_t)size, NULL, &frame_size, 1);
    if (fserr != SvtJxsErrorNone || frame_size == 0 || frame_size > size) {
        if (log_count < 3) {
            fprintf(stderr, "[DEC] get_single_frame_size failed: err=%d frame_size=%u buf_size=%zu\n",
                    fserr, frame_size, size);
            fprintf(stderr, "[DEC] first bytes: %02x %02x %02x %02x %02x %02x %02x %02x\n",
                    size > 0 ? bitstream[0] : 0, size > 1 ? bitstream[1] : 0,
                    size > 2 ? bitstream[2] : 0, size > 3 ? bitstream[3] : 0,
                    size > 4 ? bitstream[4] : 0, size > 5 ? bitstream[5] : 0,
                    size > 6 ? bitstream[6] : 0, size > 7 ? bitstream[7] : 0);
            log_count++;
        }
        return -1;
    }

    /* Log first few frames for debugging */
    if (log_count < 3) {
        fprintf(stderr, "[DEC] Frame #%d: buf=%zu exact=%u\n",
                log_count, size, frame_size);
    }

    svt_jpeg_xs_frame_t frame;
    memset(&frame, 0, sizeof(frame));

    /* Bitstream input — used_size must be EXACT codestream size */
    frame.bitstream.buffer          = (uint8_t *)bitstream;
    frame.bitstream.allocation_size = (uint32_t)size;
    frame.bitstream.used_size       = frame_size;

    /* Image output buffers */
    frame.image.data_yuv[0] = r->decoded_y;
    frame.image.data_yuv[1] = r->decoded_u;
    frame.image.data_yuv[2] = r->decoded_v;

    frame.image.stride[0] = SRC_W;
    frame.image.stride[1] = SRC_W / 2;
    frame.image.stride[2] = SRC_W / 2;

    frame.image.alloc_size[0] = (uint32_t)r->decoded_y_size;
    frame.image.alloc_size[1] = (uint32_t)r->decoded_uv_size;
    frame.image.alloc_size[2] = (uint32_t)r->decoded_uv_size;

    /* Send frame to decoder (blocking) */
    SvtJxsErrorType_t err = svt_jpeg_xs_decoder_send_frame(&r->dec_api, &frame, 1);
    if (err != SvtJxsErrorNone) {
        fprintf(stderr, "[DEC] send_frame failed: %d\n", err);
        return -1;
    }

    /* Get decoded frame (blocking) */
    svt_jpeg_xs_frame_t out_frame;
    memset(&out_frame, 0, sizeof(out_frame));

    err = svt_jpeg_xs_decoder_get_frame(&r->dec_api, &out_frame, 1);
    if (err != SvtJxsErrorNone) {
        if (log_count < 3) {
            fprintf(stderr, "[DEC] get_frame failed: %d (0x%08x), frame size=%zu\n",
                    err, (unsigned int)err, size);
        }
        log_count++;
        return -1;
    }

    log_count++;
    return 0;
}

static void decoder_close(receiver_t *r)
{
    if (r->decoder_initialized) {
        svt_jpeg_xs_decoder_close(&r->dec_api);
        r->decoder_initialized = false;
    }
    free(r->decoded_y);  r->decoded_y = NULL;
    free(r->decoded_u);  r->decoded_u = NULL;
    free(r->decoded_v);  r->decoded_v = NULL;
}

/* ═══════════════════════════════════════════════
 * Async Decode Queue + Worker Thread
 *
 * The consumer loop copies the codestream out of the MXL grain
 * and releases the grain immediately, then pushes the copy into
 * a bounded ring.  A dedicated decode worker thread pulls entries,
 * runs the (blocking) SVT decoder, generates thumbnails, and
 * updates stats.  This eliminates RDMA back-pressure caused by
 * holding grains during the ~33 ms decode.
 * ═══════════════════════════════════════════════ */

static int dq_init(decode_queue_t *dq)
{
    memset(dq, 0, sizeof(*dq));
    for (int i = 0; i < DECODE_QUEUE_DEPTH; i++) {
        dq->slots[i].data = malloc(MAX_CODESTREAM_SIZE);
        if (!dq->slots[i].data) return -1;
    }
    pthread_mutex_init(&dq->lock, NULL);
    pthread_cond_init(&dq->not_empty, NULL);
    pthread_cond_init(&dq->not_full, NULL);
    dq->running = true;
    return 0;
}

/* Push a codestream copy into the queue.  Returns 0 on success,
 * -1 if the queue is full (frame dropped). */
static int dq_push(decode_queue_t *dq, const uint8_t *cs, size_t cs_size,
                    const poc_8k_frame_header_t *hdr)
{
    pthread_mutex_lock(&dq->lock);

    /* Wait up to 5 ms for space — avoids unbounded stall if decoder
       can't keep up, but gives a short grace window. */
    while (dq->count == DECODE_QUEUE_DEPTH && dq->running) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 5000000;  /* 5 ms */
        if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
        pthread_cond_timedwait(&dq->not_full, &dq->lock, &ts);
        if (dq->count == DECODE_QUEUE_DEPTH) {
            /* Still full — drop frame */
            pthread_mutex_unlock(&dq->lock);
            return -1;
        }
    }
    if (!dq->running) { pthread_mutex_unlock(&dq->lock); return -1; }

    decode_slot_t *slot = &dq->slots[dq->head];
    memcpy(slot->data, cs, cs_size);
    slot->size = cs_size;
    slot->hdr  = *hdr;
    dq->head = (dq->head + 1) % DECODE_QUEUE_DEPTH;
    dq->count++;

    pthread_cond_signal(&dq->not_empty);
    pthread_mutex_unlock(&dq->lock);
    return 0;
}

/* Pop a codestream from the queue (blocking).  Returns slot pointer
 * with data/size/hdr valid, or NULL on shutdown. */
static decode_slot_t *dq_pop(decode_queue_t *dq)
{
    pthread_mutex_lock(&dq->lock);
    while (dq->count == 0 && dq->running) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 100000000;  /* 100 ms timeout for shutdown check */
        if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
        pthread_cond_timedwait(&dq->not_empty, &dq->lock, &ts);
    }
    if (dq->count == 0) { pthread_mutex_unlock(&dq->lock); return NULL; }

    decode_slot_t *slot = &dq->slots[dq->tail];
    dq->tail = (dq->tail + 1) % DECODE_QUEUE_DEPTH;
    dq->count--;

    pthread_cond_signal(&dq->not_full);
    pthread_mutex_unlock(&dq->lock);
    return slot;
}

static void dq_stop(decode_queue_t *dq)
{
    pthread_mutex_lock(&dq->lock);
    dq->running = false;
    pthread_cond_broadcast(&dq->not_empty);
    pthread_cond_broadcast(&dq->not_full);
    pthread_mutex_unlock(&dq->lock);
}

static void dq_cleanup(decode_queue_t *dq)
{
    for (int i = 0; i < DECODE_QUEUE_DEPTH; i++)
        free(dq->slots[i].data);
    pthread_mutex_destroy(&dq->lock);
    pthread_cond_destroy(&dq->not_empty);
    pthread_cond_destroy(&dq->not_full);
}

/* Decode worker thread — runs on the same NUMA-1 cores as decoder threads */
static void *decode_worker_thread(void *arg)
{
    receiver_t *r = (receiver_t *)arg;
    decode_queue_t *dq = &r->dq;

    printf("[DECODE-WORKER] Started\n");

    while (dq->running) {
        decode_slot_t *slot = dq_pop(dq);
        if (!slot) break;

        /* Initialize decoder on first codestream */
        if (!r->decoder_initialized) {
            printf("[DECODE-WORKER] First frame (%zu bytes, idx=%u), init decoder...\n",
                   slot->size, slot->hdr.frame_idx);
            if (decoder_init_from_codestream(r, slot->data, slot->size) != 0) {
                fprintf(stderr, "[DECODE-WORKER] Decoder init failed\n");
                atomic_fetch_add(&r->decode_errors, 1);
                continue;
            }
        }

        /* Decode (blocking — SVT uses its 16 internal threads) */
        uint64_t t_dec = now_us();
        int rc = decoder_decode(r, slot->data, slot->size);

        if (rc == 0) {
            uint64_t dec_elapsed = now_us() - t_dec;
            atomic_fetch_add(&r->decoded_frames, 1);
            atomic_fetch_add(&r->decode_time_us, dec_elapsed);

            /* End-to-end latency */
            if (slot->hdr.encode_start_ns > 0) {
                uint64_t e2e_ns = now_ns() - slot->hdr.encode_start_ns;
                atomic_fetch_add(&r->e2e_latency_us, e2e_ns / 1000);
            }
            /* Encode latency from header */
            if (slot->hdr.encode_duration_us > 0) {
                atomic_fetch_add(&r->encode_time_us,
                                 (uint64_t)slot->hdr.encode_duration_us);
            }

            /* Generate thumbnail (downscale is CPU-bound but <2 ms at 8K→960×540) */
            thumb_submit(&r->thumb,
                         r->decoded_y, r->decoded_u, r->decoded_v,
                         SRC_W, SRC_H);
        } else {
            atomic_fetch_add(&r->decode_errors, 1);
        }
    }

    printf("[DECODE-WORKER] Stopped\n");
    return NULL;
}

/* ═══════════════════════════════════════════════
 * HTTP Server (simple single-threaded for snapshot/stream)
 * ═══════════════════════════════════════════════ */

static int http_start(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(port),
        .sin_addr.s_addr = INADDR_ANY,
    };

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    listen(fd, 5);
    return fd;
}

static void http_serve_snapshot(int client_fd, const char *thumb_path)
{
    FILE *f = fopen(thumb_path, "rb");
    if (!f) {
        dprintf(client_fd,
                "HTTP/1.1 404 Not Found\r\n"
                "Content-Length: 0\r\n\r\n");
        return;
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buf = malloc(sz);
    if (!buf) { fclose(f); return; }
    fread(buf, 1, sz, f);
    fclose(f);

    dprintf(client_fd,
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: image/jpeg\r\n"
            "Content-Length: %ld\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Cache-Control: no-cache\r\n\r\n", sz);
    write(client_fd, buf, sz);
    free(buf);
}

static void http_serve_stats(int client_fd, receiver_t *r)
{
    uint64_t rdma  = atomic_load(&r->rdma_frames);
    uint64_t dec   = atomic_load(&r->decoded_frames);
    uint64_t errs  = atomic_load(&r->decode_errors);

    /* Use windowed averages (updated every 2s by main loop) */
    double avg_dec   = r->win.avg_decode_ms;
    double avg_enc   = r->win.avg_encode_ms;
    double avg_rdma  = r->win.avg_rdma_ms;     /* legacy residual */
    double avg_e2e   = r->win.avg_e2e_ms;
    double real_rdma = r->win.real_rdma_ms;     /* actual RDMA from sender */
    double transit   = r->win.transit_ms;       /* shm→ST22→switch→sender */
    double transit_shm_to_comptx = r->win.shm_to_comptx_ms;
    double transit_comptx_stage  = r->win.comptx_stage_ms;
    double transit_st22_path     = r->win.st22_path_ms;
    double sender_fps_delta      = r->win.sender_fps_delta;
    double sender_fps_cum        = r->win.sender_fps_cum;
    double fps       = r->win.fps;

    char body[768];
    int len = snprintf(body, sizeof(body),
        "{\"rdma_frames\":%lu,\"decoded_frames\":%lu,"
        "\"decode_errors\":%lu,\"avg_decode_ms\":%.1f,"
        "\"avg_encode_ms\":%.1f,\"avg_rdma_ms\":%.1f,"
        "\"real_rdma_ms\":%.1f,\"transit_ms\":%.1f,"
        "\"transit_shm_to_comptx_ms\":%.1f,"
        "\"transit_comptx_stage_ms\":%.1f,"
        "\"transit_st22_path_ms\":%.1f,"
        "\"sender_fps_delta\":%.1f,"
        "\"sender_fps_cum\":%.1f,"
        "\"avg_e2e_ms\":%.1f,\"fps\":%.1f}",
        (unsigned long)rdma, (unsigned long)dec,
        (unsigned long)errs, avg_dec, avg_enc, avg_rdma,
        real_rdma, transit,
        transit_shm_to_comptx, transit_comptx_stage, transit_st22_path,
        sender_fps_delta, sender_fps_cum,
        avg_e2e, fps);

    dprintf(client_fd,
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %d\r\n"
            "Access-Control-Allow-Origin: *\r\n\r\n%s",
            len, body);
}

/* ── MJPEG stream thread (one per /stream client) ── */
typedef struct {
    int         fd;
    receiver_t *recv;
    char        thumb_path[512];
} mjpeg_client_t;

static void *mjpeg_stream_thread(void *arg)
{
    mjpeg_client_t *mc = (mjpeg_client_t *)arg;
    int fd = mc->fd;
    receiver_t *r = mc->recv;

    /* Send multipart header */
    const char *hdr =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=--poc8kframe\r\n"
        "Cache-Control: no-cache, no-store\r\n"
        "Connection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n\r\n";
    if (send(fd, hdr, strlen(hdr), MSG_NOSIGNAL) < 0) goto done;

    /* Use nanosecond-precision mtime so we detect every thumbnail update
       (time_t st_mtime has only 1-second resolution → ~1 fps before) */
    struct timespec last_mtim = {0, 0};

    while (r->running) {
        struct stat st;
        if (stat(mc->thumb_path, &st) < 0) {
            usleep(50000); /* 50ms — file not yet created */
            continue;
        }
        if (st.st_mtim.tv_sec  == last_mtim.tv_sec &&
            st.st_mtim.tv_nsec == last_mtim.tv_nsec) {
            usleep(15000); /* 15ms poll — supports up to ~60 fps detection */
            continue;
        }
        last_mtim = st.st_mtim;

        FILE *f = fopen(mc->thumb_path, "rb");
        if (!f) { usleep(15000); continue; }

        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz <= 0 || sz > 2*1024*1024) { fclose(f); usleep(15000); continue; }

        char *buf = malloc(sz);
        if (!buf) { fclose(f); continue; }
        fread(buf, 1, sz, f);
        fclose(f);

        char part_hdr[256];
        int plen = snprintf(part_hdr, sizeof(part_hdr),
            "--poc8kframe\r\n"
            "Content-Type: image/jpeg\r\n"
            "Content-Length: %ld\r\n\r\n", sz);

        if (send(fd, part_hdr, plen, MSG_NOSIGNAL) < 0) { free(buf); goto done; }
        if (send(fd, buf, sz, MSG_NOSIGNAL) < 0)        { free(buf); goto done; }
        if (send(fd, "\r\n", 2, MSG_NOSIGNAL) < 0)     { free(buf); goto done; }
        free(buf);
    }

done:
    close(fd);
    free(mc);
    return NULL;
}

/* HTTP server thread */
static void *http_thread(void *arg)
{
    receiver_t *r = (receiver_t *)arg;
    char thumb_path[512];
    snprintf(thumb_path, sizeof(thumb_path), "%s/thumb.jpg", r->cfg.thumb_dir);

    printf("[HTTP] Listening on port %d\n", r->cfg.http_port);

    while (r->running) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);

        /* Use select with timeout to check for shutdown */
        fd_set fds;
        struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
        FD_ZERO(&fds);
        FD_SET(r->http_fd, &fds);

        int sel = select(r->http_fd + 1, &fds, NULL, NULL, &tv);
        if (sel <= 0) continue;

        int cfd = accept(r->http_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (cfd < 0) continue;

        /* Read request (minimal parsing) */
        char req[1024] = {0};
        read(cfd, req, sizeof(req) - 1);

        if (strstr(req, "GET /stream")) {
            /* Spawn detached thread for MJPEG streaming */
            mjpeg_client_t *mc = calloc(1, sizeof(*mc));
            mc->fd = cfd;
            mc->recv = r;
            snprintf(mc->thumb_path, sizeof(mc->thumb_path), "%s", thumb_path);
            pthread_t tid;
            pthread_create(&tid, NULL, mjpeg_stream_thread, mc);
            pthread_detach(tid);
            /* Do NOT close cfd — thread owns it */
        } else if (strstr(req, "GET /snapshot")) {
            http_serve_snapshot(cfd, thumb_path);
            close(cfd);
        } else if (strstr(req, "GET /stats")) {
            http_serve_stats(cfd, r);
            close(cfd);
        } else {
            /* Default: index page */
            dprintf(cfd,
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: text/html\r\n\r\n"
                    "<html><body><h1>poc_8k Receiver</h1>"
                    "<p><a href=\"/stream\">MJPEG Stream</a></p>"
                    "<p><a href=\"/snapshot\">Snapshot</a></p>"
                    "<p><a href=\"/stats\">Stats</a></p>"
                    "<p><img src=\"/stream\" style=\"max-width:100%%\"/></p>"
                    "</body></html>\n");
            close(cfd);
        }
    }

    printf("[HTTP] Server stopped\n");
    return NULL;
}

/* ═══════════════════════════════════════════════
 * CLI Argument Parsing
 * ═══════════════════════════════════════════════ */

static void set_defaults(receiver_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    strncpy(cfg->mxl_domain, "/dev/shm/mxl_8k_rx", sizeof(cfg->mxl_domain));
    strncpy(cfg->provider, "verbs", sizeof(cfg->provider));
    cfg->bind_ip[0] = '\0';
    strncpy(cfg->bind_port, "5100", sizeof(cfg->bind_port));
    cfg->decoder_threads = 30;
    strncpy(cfg->thumb_dir, "/dev/shm/poc_8k_thumbs", sizeof(cfg->thumb_dir));
    cfg->http_port = 8088;
}

static void print_usage(const char *prog)
{
    printf("Usage: %s [options]\n"
           "  --mxl-domain <path>   MXL domain\n"
           "  --flow-json <path>    MXL flow JSON\n"
           "  --provider <name>     Fabrics provider (verbs/tcp)\n"
           "  --bind-ip <IP>        RDMA receiver IP\n"
           "  --bind-port <N>       Fabrics bind port\n"
           "  --decoder-threads <N> SVT-JPEG-XS threads (default: 16)\n"
           "  --thumb-dir <path>    Thumbnail output dir\n"
           "  --http-port <N>       HTTP server port (default: 8088)\n"
           "  --duration <sec>      Duration (0=infinite)\n"
           "  --help\n", prog);
}

static int parse_args(int argc, char **argv, receiver_config_t *cfg)
{
    static struct option long_opts[] = {
        {"mxl-domain",      required_argument, NULL, 1001},
        {"flow-json",       required_argument, NULL, 1002},
        {"provider",        required_argument, NULL, 1003},
        {"bind-ip",         required_argument, NULL, 1004},
        {"bind-port",       required_argument, NULL, 1005},
        {"decoder-threads", required_argument, NULL, 1006},
        {"thumb-dir",       required_argument, NULL, 1007},
        {"http-port",       required_argument, NULL, 1008},
        {"duration",        required_argument, NULL, 1009},
        {"help",            no_argument,       NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "h", long_opts, NULL)) != -1) {
        switch (opt) {
        case 1001: strncpy(cfg->mxl_domain, optarg, sizeof(cfg->mxl_domain)-1); break;
        case 1002: strncpy(cfg->flow_json_path, optarg, sizeof(cfg->flow_json_path)-1); break;
        case 1003: strncpy(cfg->provider, optarg, sizeof(cfg->provider)-1); break;
        case 1004: strncpy(cfg->bind_ip, optarg, sizeof(cfg->bind_ip)-1); break;
        case 1005: strncpy(cfg->bind_port, optarg, sizeof(cfg->bind_port)-1); break;
        case 1006: cfg->decoder_threads = atoi(optarg); break;
        case 1007: strncpy(cfg->thumb_dir, optarg, sizeof(cfg->thumb_dir)-1); break;
        case 1008: cfg->http_port = atoi(optarg); break;
        case 1009: cfg->duration_sec = atoi(optarg); break;
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
    receiver_t *r = &g_recv;
    memset(r, 0, sizeof(*r));
    r->running = true;
    r->http_fd = -1;
    pthread_t http_tid = 0;

    setbuf(stdout, NULL);  /* Force unbuffered stdout for nohup */

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    ignore_sigpipe();      /* Prevent SIGPIPE crash from MJPEG clients */

    /* Parse config */
    set_defaults(&r->cfg);
    if (parse_args(argc, argv, &r->cfg) != 0)
        return 1;

    receiver_config_t *cfg = &r->cfg;

    if (!cfg->flow_json_path[0]) {
        fprintf(stderr, "Error: --flow-json is required\n");
        return 1;
    }
    if (!cfg->bind_ip[0]) {
        fprintf(stderr, "Error: --bind-ip is required (e.g. 192.168.2.x)\n");
        return 1;
    }

    printf("\n══════════════════════════════════════════════════════\n");
    printf("  poc_8k Receiver — MXL RDMA RX + JPEG XS Decode\n");
    printf("══════════════════════════════════════════════════════\n");
    printf("  RDMA bind: %s:%s\n", cfg->bind_ip, cfg->bind_port);
    printf("  Decoder: SVT-JPEG-XS, threads=%d\n", cfg->decoder_threads);
    printf("  Thumbnail: %s (%dx%d)\n", cfg->thumb_dir, THUMB_W, THUMB_H);
    printf("  HTTP: port %d\n", cfg->http_port);
    printf("══════════════════════════════════════════════════════\n\n");

    /* ── 1. Init thumbnail subsystem ── */
    if (thumb_init(&r->thumb, cfg->thumb_dir) != 0) {
        fprintf(stderr, "[RECEIVER] Thumbnail init failed\n");
        return 1;
    }

    /* ── 2. Init MXL sink + fabrics target ── */
    if (mxl_init(r) != 0) {
        fprintf(stderr, "[RECEIVER] MXL init failed\n");
        goto cleanup;
    }

    /* ── 3. Init async decode queue ── */
    if (dq_init(&r->dq) != 0) {
        fprintf(stderr, "[RECEIVER] Decode queue init failed\n");
        goto cleanup;
    }

    /* ── 4. Start HTTP server ── */
    r->http_fd = http_start(cfg->http_port);
    if (r->http_fd < 0) {
        fprintf(stderr, "[RECEIVER] HTTP server failed to start on port %d\n",
                cfg->http_port);
        goto cleanup;
    }

    pthread_create(&http_tid, NULL, http_thread, r);

    /* ── 5. Start decode worker thread ── */
    if (pthread_create(&r->dq.thread, NULL, decode_worker_thread, r) != 0) {
        fprintf(stderr, "[RECEIVER] Failed to start decode worker thread\n");
        goto cleanup;
    }

    printf("[RECEIVER] Waiting for RDMA connection...\n\n");

    /* ── 6. Consumer loop: drain RDMA grains → copy-out → release → queue ── */
    time_t start_time = time(NULL);
    uint64_t last_stats_us = now_us();
    uint32_t idle_spins = 0;
    uint64_t queue_drops = 0;

    while (r->running) {
        /* Drain all available RDMA grains as fast as possible */
        int events = 0;
        for (;;) {
            uint16_t entry_idx = 0, slice_idx = 0;
            mxlStatus st = mxlFabricsTargetReadGrainNonBlocking(
                r->target, &entry_idx, &slice_idx);

            if (st == MXL_ERR_NOT_READY)
                break;
            if (st != MXL_STATUS_OK) {
                fprintf(stderr, "[CONSUMER] TargetReadGrain error: %d\n", st);
                break;
            }
            events++;
            atomic_fetch_add(&r->rdma_frames, 1);

            /* Read grain info for codestream size */
            mxlGrainInfo ginfo;
            memset(&ginfo, 0, sizeof(ginfo));
            mxlFlowWriterGetGrainInfo(r->writer, entry_idx, &ginfo);

            /* Get grain payload */
            uint8_t *payload = NULL;
            mxlStatus ost = mxlFlowWriterOpenGrain(r->writer, entry_idx, &ginfo, &payload);
            if (ost != MXL_STATUS_OK || !payload) {
                fprintf(stderr, "[CONSUMER] OpenGrain failed: %d\n", ost);
                continue;
            }

            /* Determine codestream size from grain */
            size_t cs_size = ginfo.grainSize;
            if (cs_size == 0 || cs_size > MAX_CODESTREAM_SIZE) {
                ginfo.validSlices = 0;
                mxlFlowWriterCommitGrain(r->writer, &ginfo);
                continue;
            }

            /* Extract latency header (prepended by compositor) */
            poc_8k_frame_header_t frame_hdr = {0};
            uint8_t *codestream = payload;
            size_t   cs_decode_size = cs_size;
            if (cs_size > POC_8K_HEADER_SIZE) {
                memcpy(&frame_hdr, payload, POC_8K_HEADER_SIZE);
                codestream = payload + POC_8K_HEADER_SIZE;
                if (frame_hdr.codestream_size > 0 &&
                    frame_hdr.codestream_size <= (cs_size - POC_8K_HEADER_SIZE)) {
                    cs_decode_size = frame_hdr.codestream_size;
                } else {
                    cs_decode_size = cs_size - POC_8K_HEADER_SIZE;
                }
            }

            /* Copy codestream into decode queue and RELEASE grain immediately.
             * This is the key change: the grain is no longer held during the
             * ~33 ms blocking decode, eliminating RDMA back-pressure. */
            int qrc = dq_push(&r->dq, codestream, cs_decode_size, &frame_hdr);

            /* Release grain — sender can reuse this slot now */
            ginfo.validSlices = 0;
            mxlFlowWriterCommitGrain(r->writer, &ginfo);

            if (qrc != 0) {
                queue_drops++;
                if (queue_drops <= 5 || (queue_drops % 100) == 0)
                    fprintf(stderr, "[CONSUMER] Decode queue full, dropped frame "
                            "(total drops: %lu)\n", (unsigned long)queue_drops);
            }
        }

        /* Idle management */
        if (events == 0) {
            idle_spins++;
            if (idle_spins < 1000) {
                __asm__ volatile("pause");
            } else if (idle_spins < 100000) {
                usleep(10);
            } else {
                usleep(1000);
            }
        } else {
            idle_spins = 0;
        }

        /* Periodic stats — update windowed averages every 2s */
        uint64_t now_stat_us = now_us();
        if (now_stat_us - last_stats_us >= 2000000) {
            uint64_t rdma  = atomic_load(&r->rdma_frames);
            uint64_t dec   = atomic_load(&r->decoded_frames);
            uint64_t errs  = atomic_load(&r->decode_errors);
            uint64_t dec_us_total = atomic_load(&r->decode_time_us);
            uint64_t enc_us_total = atomic_load(&r->encode_time_us);
            uint64_t e2e_us_total = atomic_load(&r->e2e_latency_us);
            double   interval_s = (double)(now_stat_us - last_stats_us) / 1000000.0;

            /* Compute windowed (delta) averages */
            uint64_t d_dec = dec - r->win.prev_decoded;
            uint64_t d_dec_us = dec_us_total - r->win.prev_decode_us;
            uint64_t d_enc_us = enc_us_total - r->win.prev_encode_us;
            uint64_t d_e2e_us = e2e_us_total - r->win.prev_e2e_us;

            if (d_dec > 0) {
                r->win.avg_decode_ms = (double)d_dec_us / d_dec / 1000.0;
                r->win.avg_encode_ms = (double)d_enc_us / d_dec / 1000.0;
                r->win.avg_e2e_ms    = (double)d_e2e_us / d_dec / 1000.0;
                double e = r->win.avg_encode_ms;
                double d = r->win.avg_decode_ms;
                r->win.avg_rdma_ms = (r->win.avg_e2e_ms > e + d)
                                   ? r->win.avg_e2e_ms - e - d : 0.0;
                r->win.fps = (double)d_dec / interval_s;

                /* Read real RDMA latency from sender stats file */
                FILE *sf = fopen("/dev/shm/poc_8k_sender_stats.json", "r");
                if (sf) {
                    char sbuf[512];
                    if (fgets(sbuf, sizeof(sbuf), sf)) {
                        double rdma_val = 0.0, ingest_val = 0.0;
                        double shm_to_comptx = 0.0, comptx_stage = 0.0, st22_path = 0.0;

                        json_get_double(sbuf, "rdma_avg_ms", &rdma_val);
                        json_get_double(sbuf, "ingest_avg_ms", &ingest_val);
                        json_get_double(sbuf, "shm_to_comptx_ms", &shm_to_comptx);
                        json_get_double(sbuf, "comptx_stage_ms", &comptx_stage);
                        json_get_double(sbuf, "st22_path_ms", &st22_path);
                        json_get_double(sbuf, "fps", &r->win.sender_fps_delta);
                        json_get_double(sbuf, "fps_cum", &r->win.sender_fps_cum);

                        if (rdma_val > 0.0)
                            r->win.real_rdma_ms = rdma_val;

                        r->win.shm_to_comptx_ms = shm_to_comptx;
                        r->win.comptx_stage_ms = comptx_stage;
                        r->win.st22_path_ms = st22_path;

                        if (ingest_val > 0.0) {
                            r->win.transit_ms = ingest_val;
                        } else {
                            double transit = r->win.avg_e2e_ms - e - r->win.real_rdma_ms - d;
                            r->win.transit_ms = transit > 0 ? transit : 0.0;
                        }
                    }
                    fclose(sf);
                }
            }

            /* Save snapshot for next window */
            r->win.prev_decoded   = dec;
            r->win.prev_decode_us = dec_us_total;
            r->win.prev_encode_us = enc_us_total;
            r->win.prev_e2e_us    = e2e_us_total;
            r->win.prev_time_us   = now_stat_us;

            printf("[RECEIVER] %lds: rdma=%lu decoded=%lu errors=%lu drops=%lu "
                     "fps=%.1f avg_decode=%.1fms rdma=%.1fms transit=%.1fms "
                     "(shm->comptx=%.1f comptx=%.1f st22=%.1f) avg_e2e=%.1fms\n",
                   (long)(time(NULL) - start_time),
                   (unsigned long)rdma, (unsigned long)dec, (unsigned long)errs,
                   (unsigned long)queue_drops,
                   r->win.fps,
                   r->win.avg_decode_ms,
                   r->win.real_rdma_ms,
                   r->win.transit_ms,
                     r->win.shm_to_comptx_ms,
                     r->win.comptx_stage_ms,
                     r->win.st22_path_ms,
                   r->win.avg_e2e_ms);

            last_stats_us = now_stat_us;
        }

        /* Duration check */
        if (cfg->duration_sec > 0 &&
            (int)(time(NULL) - start_time) >= cfg->duration_sec) {
            printf("[RECEIVER] Duration reached (%ds)\n", cfg->duration_sec);
            break;
        }
    }

cleanup:
    printf("\n[RECEIVER] Shutting down...\n");
    r->running = false;

    /* Stop decode worker */
    dq_stop(&r->dq);
    pthread_join(r->dq.thread, NULL);
    dq_cleanup(&r->dq);

    if (r->http_fd >= 0) {
        close(r->http_fd);
        if (http_tid)
            pthread_join(http_tid, NULL);
    }

    thumb_cleanup(&r->thumb);
    decoder_close(r);
    mxl_cleanup(r);

    printf("[RECEIVER] Clean shutdown.\n");
    return 0;
}
