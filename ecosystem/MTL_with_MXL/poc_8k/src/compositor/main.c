/* SPDX-License-Identifier: BSD-3-Clause
 * poc_8k — 8K Tiled Compositor
 *
 * Receives 16 × ST2110-20 1080p streams via st20p_rx (pipeline API),
 * tiles them into a 4×4 8K (7680×4320) YUV422PLANAR10LE framebuffer,
 * encodes with SVT-JPEG-XS, and writes compressed codestreams to a
 * shared-memory ring buffer for the companion compositor_tx process.
 *
 * Data flow:
 *   16× multicast → st20p_rx → v210→planar conversion (MTL) →
 *   tile 4×4 → SVT-JPEG-XS encode → shm_ring → (compositor_tx → ST22 TX)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <getopt.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <time.h>
#include <errno.h>
#include <stdatomic.h>
#include <sched.h>
#include <dirent.h>
#include <sys/mman.h>
#include <immintrin.h>

/* libjpeg (for thumbnail generation) */
#include <jpeglib.h>

/* MTL */
#include <mtl/mtl_api.h>
#include <mtl/st20_api.h>
#include <mtl/st_pipeline_api.h>

/* SVT-JPEG-XS */
#include <svt-jpegxs/SvtJpegxs.h>
#include <svt-jpegxs/SvtJpegxsEnc.h>

/* Shared-memory ring buffer (IPC with compositor_tx) */
#include "../common/shm_ring.h"

/* ── Non-temporal memcpy — bypasses writer's L2 cache ──
 * Used for two cross-core hot paths:
 *   1) ingest_copy_to_shadow: ingest workers → tile workers (8.3 MB/tile × 16)
 *   2) SHM ring codestream:   encoder thread → compositor_tx process (1–4 MB)
 * In both cases the writer never reads the data back and a different core/process
 * reads it.  NT stores put data directly in L3 (LLC), avoiding L2 pollution.
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

/* ── Constants ── */
#define MAX_TILES         16
#define GRID_COLS          4
#define GRID_ROWS          4
#define TILE_W          1920
#define TILE_H          1080
#define OUT_W           7680   /* GRID_COLS * TILE_W */
#define OUT_H           4320   /* GRID_ROWS * TILE_H */

/* YUV422PLANAR10LE: 16-bit per sample (10 bits used) */
#define BYTES_PER_SAMPLE   2

/* Plane sizes for 8K output */
#define OUT_Y_STRIDE    (OUT_W * BYTES_PER_SAMPLE)        /* 15360 */
#define OUT_U_STRIDE    ((OUT_W / 2) * BYTES_PER_SAMPLE)  /*  7680 */
#define OUT_V_STRIDE    OUT_U_STRIDE                      /*  7680 */
#define OUT_Y_SIZE      (OUT_Y_STRIDE * OUT_H)            /* 66355200 */
#define OUT_U_SIZE      (OUT_U_STRIDE * OUT_H)            /* 33177600 */
#define OUT_V_SIZE      OUT_U_SIZE                        /* 33177600 */
#define OUT_FRAME_SIZE  (OUT_Y_SIZE + OUT_U_SIZE + OUT_V_SIZE) /* 132710400 */

/* Plane sizes for 1080p tile */
#define TILE_Y_STRIDE   (TILE_W * BYTES_PER_SAMPLE)       /* 3840 */
#define TILE_U_STRIDE   ((TILE_W / 2) * BYTES_PER_SAMPLE) /* 1920 */
#define TILE_V_STRIDE   TILE_U_STRIDE                     /* 1920 */
#define TILE_Y_SIZE     (TILE_Y_STRIDE * TILE_H)
#define TILE_U_SIZE     (TILE_U_STRIDE * TILE_H)
#define TILE_V_SIZE     TILE_U_SIZE
#define SHADOW_FRAME_SIZE (TILE_Y_SIZE + TILE_U_SIZE + TILE_V_SIZE)

/* Maximum compressed codestream size (bpp=4 → ~16.6 MB, add 20% margin) */
#define MAX_CODESTREAM_SIZE  (20 * 1024 * 1024)  /* 20 MB */

/* Tile worker threads for parallel memcpy — splits 16 tiles across N threads.
 * 4 workers each handle 4 tiles of 1080 rows × 3 planes, reducing tiling
 * time from ~37ms (single-thread) to ~9-12ms. */
#define TILE_WORKERS 4
#define INGEST_WORKERS 4

/* Thumbnail (downscaled preview of 8K composite) */
#define THUMB_W       960
#define THUMB_H       540
#define THUMB_QUALITY 75
#define THUMB_RGB_SIZE ((size_t)THUMB_W * THUMB_H * 3)

/* ── Latency tracking header — prepended to each codestream for e2e measurement ──
 * Requires synchronized clocks across TX/RX hosts (ptp4l + phc2sys recommended). */
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

/* ── Configuration ── */
typedef struct {
    /* MTL ports — both used for RX (8 streams each) */
    char     rx_port[64];        /* VF BDF for RX port P (streams 0-7) */
    uint8_t  rx_sip[4];          /* Source IP for RX port P */
    char     tx_port[64];        /* VF BDF for RX port R (streams 8-15) */
    uint8_t  tx_sip[4];          /* Source IP for RX port R */

    /* Lcores */
    char     lcores[128];

    /* Encoder */
    int      codec_threads;      /* SVT-JPEG-XS thread count */
    int      compression_ratio;  /* e.g. 5 for 5:1 (bpp = 20/ratio) */

    /* FPS */
    uint32_t fps_num;
    uint32_t fps_den;

    /* Source streams */
    int      num_streams;
    struct {
        uint8_t  multicast_ip[4];
        uint16_t udp_port;
        char     label[64];
    } streams[MAX_TILES];

    /* Duration */
    int      duration_sec;       /* 0 = infinite */

    /* Thumbnail */
    char     thumb_dir[256];     /* empty = no thumbnails */
} comp_config_t;

/* ── Thumbnail triple-buffer (same pattern as poc / poc_14 / receiver) ── */
typedef struct {
    uint8_t *hot;        /* hot-path writes downscaled RGB here */
    uint8_t *ready;      /* latest completed frame, swapped under mutex */
    uint8_t *encode_buf; /* encoder thread's private buffer */
    pthread_mutex_t  lock;
    pthread_cond_t   cond;
    volatile bool    new_frame;
    volatile bool    running;
    pthread_t        thread;
    char             dir[256];
} thumb_ctx_t;

/* ── Tile worker thread (for parallel tile_compose) ── */
typedef struct {
    pthread_t          thread;
    int                worker_id;
    int                tile_start;  /* first tile index */
    int                tile_count;  /* number of tiles to process */
    pthread_barrier_t *start_bar;   /* main signals start */
    pthread_barrier_t *done_bar;    /* workers signal completion */
    atomic_bool        running;
} tile_worker_t;

/* ── Per-stream RX context ── */
typedef struct {
    st20p_rx_handle rx;
    atomic_int       frame_avail;    /* signaled by notify callback */

    uint8_t         *shadow_buf[2];
    uint8_t         *shadow_y[2];
    uint8_t         *shadow_u[2];
    uint8_t         *shadow_v[2];
    atomic_int       shadow_active_idx;  /* 0/1 */
    atomic_bool      shadow_valid;
} rx_stream_t;

typedef struct {
    pthread_t        thread;
    int              worker_id;
    int              stream_start;
    int              stream_count;
    atomic_bool      running;
} ingest_worker_t;

/* ── Compositor global context ── */
typedef struct {
    /* MTL */
    mtl_handle          mtl;
    rx_stream_t         rx[MAX_TILES];
    int                 num_rx;

    /* Shared-memory ring buffer (IPC → compositor_tx) */
    shm_ring_producer_t shm_ring;

    /* Double-buffered tiling (8K YUV422P10LE, 3 contiguous planes).
     * Two 132 MB hugepage buffers allow the main (RX) thread to tile
     * into one buffer while the encoder thread encodes the other. */
    uint8_t            *tile_bufs[2];         /* raw mmap'd regions */
    uint8_t            *tile_planes[2][3];    /* [buf][plane] Y/U/V */
    bool                tile_hugepages[2];
    int                 tile_write_idx;        /* which buf main tiles into */

    /* Active plane ptrs — set per-frame to tile_planes[tile_write_idx] */
    uint8_t            *tile_y;
    uint8_t            *tile_u;
    uint8_t            *tile_v;

    /* SVT-JPEG-XS encoder */
    svt_jpeg_xs_encoder_api_t enc_api;
    uint8_t            *bitstream_buf;   /* encoder output buffer */
    size_t              bitstream_alloc;

    /* Runtime */
    volatile bool       running;
    comp_config_t       cfg;

    /* Stats */
    atomic_uint_fast64_t frames_composed;
    atomic_uint_fast64_t frames_encoded;
    atomic_uint_fast64_t frames_written;  /* written to shm ring */
    atomic_uint_fast64_t encode_time_us;
    atomic_uint_fast64_t tile_time_us;
    atomic_uint_fast64_t enc_wait_time_us;
    atomic_uint_fast64_t shm_wait_time_us;
    atomic_uint_fast64_t shm_copy_time_us;
    uint64_t             frame_counter;  /* monotonic frame index for latency header */

    /* Thumbnail (triple-buffered, async) */
    thumb_ctx_t          thumb;
    bool                 thumb_enabled;

    /* Tile worker pool — parallel tiling across TILE_WORKERS threads */
    tile_worker_t        tile_workers[TILE_WORKERS];
    pthread_barrier_t    tile_start_bar;
    pthread_barrier_t    tile_done_bar;
    bool                 tile_pool_active;

    /* RX ingest workers — continuously drain MTL and refresh stream shadows */
    ingest_worker_t      ingest_workers[INGEST_WORKERS];
    bool                 ingest_pool_active;
    atomic_uint_fast64_t ingest_copy_time_us;
    atomic_uint_fast64_t ingest_updates;

    /* Async encoder pipeline — runs in a dedicated thread so the main
     * RX → tile → release hot-path never blocks during encoding. */
    pthread_t            enc_thread;
    pthread_mutex_t      enc_mutex;
    pthread_cond_t       enc_cond_avail; /* encoder → main: buffer freed */
    pthread_cond_t       enc_cond_work;  /* main → encoder: work ready  */
    int                  enc_pending_buf; /* buf index to encode, -1=none */
    int                  enc_active_buf;  /* buf being encoded, -1=idle */
    bool                 enc_thread_running;
    uint64_t             enc_frame_idx;   /* frame counter for pending encode */
} compositor_t;

static compositor_t g_comp;

/* ── Signal handler ── */
static void sig_handler(int sig) { (void)sig; g_comp.running = false; }

/* ── Helper: current time in microseconds ── */
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

/* ═══════════════════════════════════════════════
 * Thumbnail: YUV422P10LE → downscale → RGB → JPEG
 * Triple-buffered, async — same approach as poc/poc_14
 * ═══════════════════════════════════════════════ */

static void yuv422p10le_to_rgb_thumb(const uint16_t *y_plane,
                                     const uint16_t *u_plane,
                                     const uint16_t *v_plane,
                                     int src_w, int src_h,
                                     uint8_t *rgb_out,
                                     int dst_w, int dst_h)
{
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

            int y_val = y_row[sx] >> 2;
            int u_val = u_row[sx / 2] >> 2;
            int v_val = v_row[sx / 2] >> 2;

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

static int write_jpeg_thumb(const char *path, const uint8_t *rgb,
                            int width, int height, int quality)
{
    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "%s/.thumb.jpg.tmp", path);
    char final_path[512];
    snprintf(final_path, sizeof(final_path), "%s/thumb.jpg", path);

    FILE *f = fopen(tmp_path, "wb");
    if (!f) {
        static int err_cnt = 0;
        if (++err_cnt <= 3)
            fprintf(stderr, "[COMP-THUMB] Cannot open %s: %m\n", tmp_path);
        return -1;
    }

    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);
    jpeg_stdio_dest(&cinfo, f);

    cinfo.image_width      = width;
    cinfo.image_height     = height;
    cinfo.input_components = 3;
    cinfo.in_color_space   = JCS_RGB;
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

    rename(tmp_path, final_path);
    return 0;
}

static void *thumb_encode_thread(void *arg)
{
    thumb_ctx_t *tc = (thumb_ctx_t *)arg;
    printf("[COMP-THUMB] Encoder thread started → %s/thumb.jpg\n", tc->dir);

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

        write_jpeg_thumb(tc->dir, tc->encode_buf, THUMB_W, THUMB_H, THUMB_QUALITY);
    }

    printf("[COMP-THUMB] Encoder thread stopped\n");
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
    if (!tc->hot || !tc->ready || !tc->encode_buf) {
        fprintf(stderr, "[COMP-THUMB] Failed to allocate triple buffers\n");
        free(tc->hot); free(tc->ready); free(tc->encode_buf);
        return -1;
    }

    pthread_mutex_init(&tc->lock, NULL);
    pthread_cond_init(&tc->cond, NULL);
    tc->running   = true;
    tc->new_frame = false;

    if (pthread_create(&tc->thread, NULL, thumb_encode_thread, tc) != 0) {
        fprintf(stderr, "[COMP-THUMB] pthread_create failed\n");
        free(tc->hot); free(tc->ready); free(tc->encode_buf);
        return -1;
    }

    printf("[COMP-THUMB] Initialised: %dx%d → %dx%d (triple-buffered, async) → %s/thumb.jpg\n",
           OUT_W, OUT_H, THUMB_W, THUMB_H, dir);
    return 0;
}

static void thumb_submit_frame(thumb_ctx_t *tc,
                               const uint16_t *y_plane,
                               const uint16_t *u_plane,
                               const uint16_t *v_plane,
                               int src_w, int src_h)
{
    /* Downscale into hot buffer (off the encode critical path) */
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
 * st20p_rx notify_frame_available callback
 * ═══════════════════════════════════════════════ */

static int rx_notify_frame(void *priv)
{
    rx_stream_t *rs = (rx_stream_t *)priv;
    atomic_store(&rs->frame_avail, 1);
    return 0;
}

/* ═══════════════════════════════════════════════
 * Tiling: compose 16 × 1080p into 8K
 *
 * Each source frame is YUV422PLANAR10LE with 3 planes:
 *   addr[0] = Y  (w × h × 2 bytes)
 *   addr[1] = U  (w/2 × h × 2 bytes)
 *   addr[2] = V  (w/2 × h × 2 bytes)
 * ═══════════════════════════════════════════════ */

static void tile_compose(compositor_t *c)
{
    for (int t = 0; t < c->num_rx; t++) {
        rx_stream_t *rs = &c->rx[t];
        if (!atomic_load_explicit(&rs->shadow_valid, memory_order_acquire))
            continue;
        int si = atomic_load_explicit(&rs->shadow_active_idx, memory_order_acquire);
        int col = t % GRID_COLS;
        int row = t / GRID_COLS;

        const uint8_t *src_y = rs->shadow_y[si];
        const uint8_t *src_u = rs->shadow_u[si];
        const uint8_t *src_v = rs->shadow_v[si];

        int dst_y_off = col * TILE_W * BYTES_PER_SAMPLE;
        int dst_u_off = col * (TILE_W / 2) * BYTES_PER_SAMPLE;
        int dst_v_off = dst_u_off;
        int row_base  = row * TILE_H;

        for (int y = 0; y < TILE_H; y++) {
            int dst_row = row_base + y;
            memcpy(c->tile_y + dst_row * OUT_Y_STRIDE + dst_y_off,
                 src_y + y * TILE_Y_STRIDE,
                   TILE_W * BYTES_PER_SAMPLE);
            memcpy(c->tile_u + dst_row * OUT_U_STRIDE + dst_u_off,
                 src_u + y * TILE_U_STRIDE,
                   (TILE_W / 2) * BYTES_PER_SAMPLE);
            memcpy(c->tile_v + dst_row * OUT_V_STRIDE + dst_v_off,
                 src_v + y * TILE_V_STRIDE,
                   (TILE_W / 2) * BYTES_PER_SAMPLE);
        }
    }
}

/* ═══════════════════════════════════════════════
 * Parallel tiling — split tiles across TILE_WORKERS threads
 *
 * Each worker handles a contiguous range of tiles so their
 * destination writes are spatially separated in the 8K buffer,
 * minimising false-sharing and cache-line contention.
 *
 * Synchronisation uses a double-barrier pattern:
 *   1.  Workers block on start_bar.
 *   2.  Main arrives at start_bar → all threads released.
 *   3.  Workers memcpy their tiles → arrive at done_bar.
 *   4.  Main arrives at done_bar → tiling complete.
 * ═══════════════════════════════════════════════ */

/* Tile a contiguous range of tiles — called by each worker thread */
static void tile_compose_range(compositor_t *c, int tile_start, int tile_count)
{
    int tile_end = tile_start + tile_count;
    if (tile_end > c->num_rx) tile_end = c->num_rx;

    for (int t = tile_start; t < tile_end; t++) {
        rx_stream_t *rs = &c->rx[t];
        if (!atomic_load_explicit(&rs->shadow_valid, memory_order_acquire))
            continue;
        int si = atomic_load_explicit(&rs->shadow_active_idx, memory_order_acquire);

        int col = t % GRID_COLS;
        int row = t / GRID_COLS;

        const uint8_t *src_y = rs->shadow_y[si];
        const uint8_t *src_u = rs->shadow_u[si];
        const uint8_t *src_v = rs->shadow_v[si];

        int dst_y_off = col * TILE_W * BYTES_PER_SAMPLE;
        int dst_u_off = col * (TILE_W / 2) * BYTES_PER_SAMPLE;
        int dst_v_off = dst_u_off;
        int row_base  = row * TILE_H;

        for (int y = 0; y < TILE_H; y++) {
            int dst_row = row_base + y;

            memcpy(c->tile_y + dst_row * OUT_Y_STRIDE + dst_y_off,
                 src_y + y * TILE_Y_STRIDE,
                   TILE_W * BYTES_PER_SAMPLE);

            memcpy(c->tile_u + dst_row * OUT_U_STRIDE + dst_u_off,
                 src_u + y * TILE_U_STRIDE,
                   (TILE_W / 2) * BYTES_PER_SAMPLE);

            memcpy(c->tile_v + dst_row * OUT_V_STRIDE + dst_v_off,
                 src_v + y * TILE_V_STRIDE,
                   (TILE_W / 2) * BYTES_PER_SAMPLE);
        }
    }
}

static void *tile_worker_fn(void *arg)
{
    tile_worker_t *w = (tile_worker_t *)arg;
    compositor_t  *c = &g_comp;

    char name[16];
    snprintf(name, sizeof(name), "tile_w%d", w->worker_id);
    pthread_setname_np(pthread_self(), name);

    while (1) {
        pthread_barrier_wait(w->start_bar);
        if (!atomic_load(&w->running))
            break;

        tile_compose_range(c, w->tile_start, w->tile_count);

        pthread_barrier_wait(w->done_bar);
    }
    return NULL;
}

static int tile_workers_init(compositor_t *c)
{
    int n = c->num_rx;
    int per = n / TILE_WORKERS;
    int extra = n % TILE_WORKERS;

    /* Barriers: TILE_WORKERS + 1 (main thread participates) */
    pthread_barrier_init(&c->tile_start_bar, NULL, TILE_WORKERS + 1);
    pthread_barrier_init(&c->tile_done_bar,  NULL, TILE_WORKERS + 1);

    /* Restrict main thread affinity before creating workers so they
     * inherit the restriction (exclude DPDK scheduler lcores). */
    cpu_set_t saved, exclude_set, full_set, restricted;
    CPU_ZERO(&exclude_set);
    CPU_ZERO(&full_set);
    CPU_ZERO(&restricted);
    sched_getaffinity(0, sizeof(saved), &saved);

    char lbuf[256];
    strncpy(lbuf, c->cfg.lcores, sizeof(lbuf) - 1);
    lbuf[sizeof(lbuf) - 1] = '\0';
    for (char *tok = strtok(lbuf, ","); tok; tok = strtok(NULL, ",")) {
        int core = atoi(tok);
        if (core >= 0 && core < CPU_SETSIZE)
            CPU_SET(core, &exclude_set);
    }
    for (int i = 0; i < CPU_SETSIZE; i++)
        CPU_SET(i, &full_set);
    CPU_XOR(&restricted, &full_set, &exclude_set);
    sched_setaffinity(0, sizeof(restricted), &restricted);

    int offset = 0;
    for (int i = 0; i < TILE_WORKERS; i++) {
        tile_worker_t *w = &c->tile_workers[i];
        w->worker_id  = i;
        w->tile_start = offset;
        w->tile_count = per + (i < extra ? 1 : 0);
        offset       += w->tile_count;
        w->start_bar  = &c->tile_start_bar;
        w->done_bar   = &c->tile_done_bar;
        atomic_init(&w->running, true);

        int rc = pthread_create(&w->thread, NULL, tile_worker_fn, w);
        if (rc != 0) {
            fprintf(stderr, "[COMP] tile worker %d create failed: %s\n",
                    i, strerror(rc));
            /* Revert barriers and already-created threads */
            for (int j = 0; j < i; j++) {
                atomic_store(&c->tile_workers[j].running, false);
            }
            /* Wake all for shutdown */
            if (i > 0) {
                /* Can't use barriers with wrong count — fall back to serial */
                pthread_barrier_destroy(&c->tile_start_bar);
                pthread_barrier_destroy(&c->tile_done_bar);
            }
            sched_setaffinity(0, sizeof(saved), &saved);
            return -1;
        }
    }

    /* Restore main thread affinity */
    sched_setaffinity(0, sizeof(saved), &saved);
    c->tile_pool_active = true;

    printf("[COMP] Tile worker pool: %d threads (",  TILE_WORKERS);
    for (int i = 0; i < TILE_WORKERS; i++) {
        printf("w%d: tiles %d-%d%s", i, c->tile_workers[i].tile_start,
               c->tile_workers[i].tile_start + c->tile_workers[i].tile_count - 1,
               i < TILE_WORKERS - 1 ? ", " : "");
    }
    printf(")\n");

    return 0;
}

static void tile_workers_destroy(compositor_t *c)
{
    if (!c->tile_pool_active) return;

    /* Signal shutdown: set all workers to not-running, then release start barrier */
    for (int i = 0; i < TILE_WORKERS; i++)
        atomic_store(&c->tile_workers[i].running, false);

    pthread_barrier_wait(&c->tile_start_bar);  /* wake workers for exit */

    for (int i = 0; i < TILE_WORKERS; i++)
        pthread_join(c->tile_workers[i].thread, NULL);

    pthread_barrier_destroy(&c->tile_start_bar);
    pthread_barrier_destroy(&c->tile_done_bar);
    c->tile_pool_active = false;
    printf("[COMP] Tile worker pool destroyed\n");
}

/* ═══════════════════════════════════════════════
 * SVT-JPEG-XS Encoder
 * ═══════════════════════════════════════════════ */

static int encoder_init(compositor_t *c)
{
    svt_jpeg_xs_encoder_api_t *enc = &c->enc_api;
    memset(enc, 0, sizeof(*enc));

    /* Load defaults */
    SvtJxsErrorType_t err = svt_jpeg_xs_encoder_load_default_parameters(
        SVT_JPEGXS_API_VER_MAJOR, SVT_JPEGXS_API_VER_MINOR, enc);
    if (err != SvtJxsErrorNone) {
        fprintf(stderr, "[ENC] load_default_parameters failed: %d\n", err);
        return -1;
    }

    /* Set parameters */
    enc->source_width     = OUT_W;
    enc->source_height    = OUT_H;
    enc->input_bit_depth  = 10;
    enc->colour_format    = COLOUR_FORMAT_PLANAR_YUV422;

    /* bpp = 20 / compression_ratio.
     * E.g. ratio=5 → bpp=4 (numerator=4, denominator=1) */
    int bpp_num = 20 / c->cfg.compression_ratio;
    if (bpp_num < 1) bpp_num = 1;
    enc->bpp_numerator   = (uint32_t)bpp_num;
    enc->bpp_denominator = 1;

    enc->threads_num      = (uint32_t)c->cfg.codec_threads;
    enc->verbose          = 0;

    /* Init encoder */
    err = svt_jpeg_xs_encoder_init(SVT_JPEGXS_API_VER_MAJOR,
                                   SVT_JPEGXS_API_VER_MINOR, enc);
    if (err != SvtJxsErrorNone) {
        fprintf(stderr, "[ENC] encoder_init failed: %d\n", err);
        return -1;
    }

    /* Allocate bitstream buffer */
    c->bitstream_alloc = MAX_CODESTREAM_SIZE;
    c->bitstream_buf = malloc(c->bitstream_alloc);
    if (!c->bitstream_buf) {
        fprintf(stderr, "[ENC] bitstream buffer alloc failed\n");
        return -1;
    }

    printf("[COMP] SVT-JPEG-XS encoder initialized: %dx%d, bpp=%d/%d, threads=%d\n",
           OUT_W, OUT_H, enc->bpp_numerator, enc->bpp_denominator,
           enc->threads_num);
    return 0;
}

static int encoder_encode_buf(compositor_t *c,
                              uint8_t *enc_y, uint8_t *enc_u, uint8_t *enc_v,
                              size_t *out_size)
{
    /* Set up input frame pointing to the supplied tile buffer */
    svt_jpeg_xs_frame_t in_frame;
    memset(&in_frame, 0, sizeof(in_frame));

    /* Plane pointers — width/height/format are set on the encoder API, not on the frame */
    in_frame.image.data_yuv[0] = (uint16_t *)enc_y;
    in_frame.image.data_yuv[1] = (uint16_t *)enc_u;
    in_frame.image.data_yuv[2] = (uint16_t *)enc_v;

    /* Strides in PIXELS (not bytes) for SVT */
    in_frame.image.stride[0] = OUT_W;
    in_frame.image.stride[1] = OUT_W / 2;
    in_frame.image.stride[2] = OUT_W / 2;

    /* Allocation sizes */
    in_frame.image.alloc_size[0] = OUT_Y_SIZE;
    in_frame.image.alloc_size[1] = OUT_U_SIZE;
    in_frame.image.alloc_size[2] = OUT_V_SIZE;

    /* Bitstream output buffer */
    in_frame.bitstream.buffer          = c->bitstream_buf;
    in_frame.bitstream.allocation_size = (uint32_t)c->bitstream_alloc;
    in_frame.bitstream.used_size       = 0;

    /* Send picture (blocking) */
    SvtJxsErrorType_t err = svt_jpeg_xs_encoder_send_picture(
        &c->enc_api, &in_frame, 1 /* blocking */);
    if (err != SvtJxsErrorNone) {
        fprintf(stderr, "[ENC] send_picture failed: %d\n", err);
        return -1;
    }

    /* Get encoded packet (blocking) */
    svt_jpeg_xs_frame_t out_frame;
    memset(&out_frame, 0, sizeof(out_frame));

    err = svt_jpeg_xs_encoder_get_packet(&c->enc_api, &out_frame, 1 /* blocking */);
    if (err != SvtJxsErrorNone) {
        fprintf(stderr, "[ENC] get_packet failed: %d\n", err);
        return -1;
    }

    /* SVT encoder may adjust bitstream.buffer internally (ring-buffer offset).
     * Copy from the ACTUAL output pointer, not the original c->bitstream_buf. */
    if (out_frame.bitstream.buffer != c->bitstream_buf) {
        memmove(c->bitstream_buf, out_frame.bitstream.buffer,
                out_frame.bitstream.used_size);
    }

    *out_size = out_frame.bitstream.used_size;
    return 0;
}

static void encoder_close(compositor_t *c)
{
    svt_jpeg_xs_encoder_close(&c->enc_api);
    free(c->bitstream_buf);
    c->bitstream_buf = NULL;
}

/* ═══════════════════════════════════════════════
 * Async encoder pipeline thread
 *
 * Runs in its own thread so the main (RX → tile → release)
 * hot-path never blocks during the ~15 ms encode.
 * Uses a double-buffer scheme: the main thread tiles into
 * tile_bufs[W] then hands it off; the encoder encodes from
 * tile_bufs[W] while the main thread switches to tile_bufs[W^1].
 * ═══════════════════════════════════════════════ */

static void *enc_pipeline_thread(void *arg)
{
    compositor_t *c = (compositor_t *)arg;
    pthread_setname_np(pthread_self(), "enc_pipeline");

    while (c->enc_thread_running) {
        /* Wait for work */
        pthread_mutex_lock(&c->enc_mutex);
        while (c->enc_pending_buf < 0 && c->enc_thread_running)
            pthread_cond_wait(&c->enc_cond_work, &c->enc_mutex);

        if (!c->enc_thread_running) {
            pthread_mutex_unlock(&c->enc_mutex);
            break;
        }

        int buf_idx = c->enc_pending_buf;
        uint64_t frame_idx = c->enc_frame_idx;
        c->enc_pending_buf = -1;
        c->enc_active_buf = buf_idx;
        pthread_mutex_unlock(&c->enc_mutex);

        /* Encode from tile_planes[buf_idx] */
        uint64_t t_enc_ns = now_ns();
        uint64_t t_enc = now_us();
        size_t codestream_size = 0;

        uint8_t *ey = c->tile_planes[buf_idx][0];
        uint8_t *eu = c->tile_planes[buf_idx][1];
        uint8_t *ev = c->tile_planes[buf_idx][2];

        if (encoder_encode_buf(c, ey, eu, ev, &codestream_size) != 0) {
            fprintf(stderr, "[ENC-PIPE] Encode failed (buf %d), skipping\n", buf_idx);
            goto done;
        }

        uint64_t enc_elapsed = now_us() - t_enc;
        atomic_fetch_add(&c->encode_time_us, enc_elapsed);
        atomic_fetch_add(&c->frames_encoded, 1);

        /* Write codestream to shared-memory ring buffer */
        {
            uint64_t t_shm_wait = now_us();
            int spin = 0;
            int slot;
            while ((slot = shm_ring_producer_try_acquire(&c->shm_ring)) < 0 &&
                   c->enc_thread_running) {
                if (++spin > 10000)
                    usleep(100);
                else
                    __asm__ volatile("pause");
            }
            atomic_fetch_add(&c->shm_wait_time_us, now_us() - t_shm_wait);

            if (!c->enc_thread_running) goto done;

            uint64_t t_shm_copy = now_us();
            shm_slot_header_t *sh = shm_ring_slot_hdr(c->shm_ring.base, (uint32_t)slot);
            sh->encode_start_ns    = t_enc_ns;
            sh->frame_idx          = (uint32_t)frame_idx;
            sh->encode_duration_us = (uint32_t)enc_elapsed;
            sh->payload_size       = (uint32_t)codestream_size;
            sh->comptx_pop_ns      = 0;
            sh->comptx_tx_ready_ns = 0;

            uint8_t *payload = shm_ring_slot_payload(c->shm_ring.base, (uint32_t)slot);
            /* NT stores: encoder thread → compositor_tx process (cross-process) */
            nt_memcpy(payload, c->bitstream_buf, codestream_size);
            sh->shm_commit_ns = now_ns();
            atomic_fetch_add(&c->shm_copy_time_us, now_us() - t_shm_copy);

            shm_ring_producer_commit(&c->shm_ring);
            atomic_fetch_add(&c->frames_written, 1);
        }

done:
        /* Signal main thread: buffer freed */
        pthread_mutex_lock(&c->enc_mutex);
        c->enc_active_buf = -1;
        pthread_cond_signal(&c->enc_cond_avail);
        pthread_mutex_unlock(&c->enc_mutex);
    }

    printf("[ENC-PIPE] Encoder pipeline thread stopped\n");
    return NULL;
}

static int enc_pipeline_start(compositor_t *c)
{
    pthread_mutex_init(&c->enc_mutex, NULL);
    pthread_cond_init(&c->enc_cond_avail, NULL);
    pthread_cond_init(&c->enc_cond_work, NULL);
    c->enc_pending_buf = -1;
    c->enc_active_buf = -1;
    c->enc_thread_running = true;
    c->enc_frame_idx = 0;

    int rc = pthread_create(&c->enc_thread, NULL, enc_pipeline_thread, c);
    if (rc != 0) {
        fprintf(stderr, "[ENC-PIPE] pthread_create failed: %s\n", strerror(rc));
        return -1;
    }
    printf("[ENC-PIPE] Async encoder pipeline started (double-buffer)\n");
    return 0;
}

static void enc_pipeline_stop(compositor_t *c)
{
    pthread_mutex_lock(&c->enc_mutex);
    c->enc_thread_running = false;
    pthread_cond_signal(&c->enc_cond_work);
    pthread_mutex_unlock(&c->enc_mutex);

    pthread_join(c->enc_thread, NULL);
    pthread_mutex_destroy(&c->enc_mutex);
    pthread_cond_destroy(&c->enc_cond_avail);
    pthread_cond_destroy(&c->enc_cond_work);
    printf("[ENC-PIPE] Async encoder pipeline stopped\n");
}

/* ═══════════════════════════════════════════════
 * RX Ingest workers
 *
 * Continuously drain st20p_rx sessions and copy newest frame into
 * per-stream double-buffered shadows. Main compose loop reads shadows
 * only and no longer blocks on get_frame/put_frame calls.
 * ═══════════════════════════════════════════════ */

static void ingest_copy_to_shadow(rx_stream_t *rs, struct st_frame *f)
{
    int active = atomic_load_explicit(&rs->shadow_active_idx, memory_order_relaxed);
    int dst = active ^ 1;

    size_t src_y_stride = f->linesize[0] ? f->linesize[0] : TILE_Y_STRIDE;
    size_t src_u_stride = f->linesize[1] ? f->linesize[1] : TILE_U_STRIDE;
    size_t src_v_stride = f->linesize[2] ? f->linesize[2] : TILE_V_STRIDE;

    const uint8_t *src_y = (const uint8_t *)f->addr[0];
    const uint8_t *src_u = (const uint8_t *)f->addr[1];
    const uint8_t *src_v = (const uint8_t *)f->addr[2];

    uint8_t *dst_y = rs->shadow_y[dst];
    uint8_t *dst_u = rs->shadow_u[dst];
    uint8_t *dst_v = rs->shadow_v[dst];

    /* NT stores: ingest worker → tile worker (cross-core) */
    if (src_y_stride == TILE_Y_STRIDE) {
        nt_memcpy(dst_y, src_y, TILE_Y_SIZE);
    } else {
        for (int y = 0; y < TILE_H; y++)
            nt_memcpy(dst_y + y * TILE_Y_STRIDE, src_y + y * src_y_stride, TILE_Y_STRIDE);
    }

    if (src_u_stride == TILE_U_STRIDE && src_v_stride == TILE_V_STRIDE) {
        nt_memcpy(dst_u, src_u, TILE_U_SIZE);
        nt_memcpy(dst_v, src_v, TILE_V_SIZE);
    } else {
        for (int y = 0; y < TILE_H; y++) {
            nt_memcpy(dst_u + y * TILE_U_STRIDE, src_u + y * src_u_stride, TILE_U_STRIDE);
            nt_memcpy(dst_v + y * TILE_V_STRIDE, src_v + y * src_v_stride, TILE_V_STRIDE);
        }
    }

    atomic_store_explicit(&rs->shadow_active_idx, dst, memory_order_release);
    atomic_store_explicit(&rs->shadow_valid, true, memory_order_release);
}

static void *ingest_worker_fn(void *arg)
{
    ingest_worker_t *w = (ingest_worker_t *)arg;
    compositor_t *c = &g_comp;

    char name[16];
    snprintf(name, sizeof(name), "ingest_%d", w->worker_id);
    pthread_setname_np(pthread_self(), name);

    while (atomic_load(&w->running) && c->running) {
        int updates = 0;
        for (int i = w->stream_start; i < w->stream_start + w->stream_count; i++) {
            if (i >= c->num_rx) break;
            rx_stream_t *rs = &c->rx[i];
            if (!rs->rx) continue;

            struct st_frame *newest = NULL;
            for (;;) {
                struct st_frame *f = st20p_rx_get_frame(rs->rx);
                if (!f) break;
                if (newest)
                    st20p_rx_put_frame(rs->rx, newest);
                newest = f;
            }

            if (!newest)
                continue;

            uint64_t t_copy = now_us();
            ingest_copy_to_shadow(rs, newest);
            atomic_fetch_add(&c->ingest_copy_time_us, now_us() - t_copy);
            atomic_fetch_add(&c->ingest_updates, 1);
            updates++;
            st20p_rx_put_frame(rs->rx, newest);
        }

        if (updates == 0)
            usleep(50);
    }
    return NULL;
}

static int ingest_workers_init(compositor_t *c)
{
    int n = c->num_rx;
    int per = n / INGEST_WORKERS;
    int extra = n % INGEST_WORKERS;
    int offset = 0;

    for (int i = 0; i < INGEST_WORKERS; i++) {
        ingest_worker_t *w = &c->ingest_workers[i];
        w->worker_id = i;
        w->stream_start = offset;
        w->stream_count = per + (i < extra ? 1 : 0);
        offset += w->stream_count;
        atomic_init(&w->running, true);

        if (pthread_create(&w->thread, NULL, ingest_worker_fn, w) != 0) {
            fprintf(stderr, "[COMP] ingest worker %d create failed\n", i);
            for (int j = 0; j < i; j++)
                atomic_store(&c->ingest_workers[j].running, false);
            for (int j = 0; j < i; j++)
                pthread_join(c->ingest_workers[j].thread, NULL);
            c->ingest_pool_active = false;
            return -1;
        }
    }

    c->ingest_pool_active = true;
    printf("[COMP] Ingest worker pool: %d threads\n", INGEST_WORKERS);
    return 0;
}

static void ingest_workers_stop(compositor_t *c)
{
    if (!c->ingest_pool_active)
        return;
    for (int i = 0; i < INGEST_WORKERS; i++)
        atomic_store(&c->ingest_workers[i].running, false);
    for (int i = 0; i < INGEST_WORKERS; i++)
        pthread_join(c->ingest_workers[i].thread, NULL);
    c->ingest_pool_active = false;
    printf("[COMP] Ingest worker pool destroyed\n");
}

static int count_valid_shadows(compositor_t *c)
{
    int valid = 0;
    for (int i = 0; i < c->num_rx; i++) {
        if (atomic_load_explicit(&c->rx[i].shadow_valid, memory_order_acquire))
            valid++;
    }
    return valid;
}

/* ═══════════════════════════════════════════════
 * CLI Argument Parsing
 * ═══════════════════════════════════════════════ */

static void print_usage(const char *prog)
{
    printf("Usage: %s [options]\n"
           "  --rx-port <BDF>       VF for RX port P, streams 0-7 (required, e.g. 0000:xx:01.6)\n"
           "  --rx-sip <IP>         Source IP for RX port P (required, e.g. 192.168.1.81)\n"
           "  --tx-port <BDF>       VF for RX port R, streams 8-15 (required, e.g. 0000:xx:01.7)\n"
           "  --tx-sip <IP>         Source IP for RX port R (required, e.g. 192.168.1.82)\n"
           "  --lcores <list>       DPDK lcore list (default: 30,31,32,33,34,35,36,37)\n"
           "  --codec-threads <N>   SVT-JPEG-XS threads (default: 20)\n"
           "  --compression <N>     Compression ratio (default: 5)\n"
           "  --fps-num <N>         FPS numerator (default: 30000)\n"
           "  --fps-den <N>         FPS denominator (default: 1001)\n"
           "  --duration <sec>      Run duration, 0=infinite (default: 0)\n"
           "  --thumb-dir <dir>     Thumbnail output directory (default: none)\n"
           "  --help\n", prog);
}

static int parse_ip(const char *str, uint8_t out[4])
{
    unsigned a, b, c, d;
    if (sscanf(str, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return -1;
    out[0] = (uint8_t)a; out[1] = (uint8_t)b;
    out[2] = (uint8_t)c; out[3] = (uint8_t)d;
    return 0;
}

static void set_defaults(comp_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));

    cfg->rx_port[0] = '\0';
    memset(cfg->rx_sip, 0, sizeof(cfg->rx_sip));

    cfg->tx_port[0] = '\0';
    memset(cfg->tx_sip, 0, sizeof(cfg->tx_sip));

    strncpy(cfg->lcores, "30,31,32,33,34,35,36,37", sizeof(cfg->lcores));

    cfg->codec_threads      = 16;
    cfg->compression_ratio  = 5;
    cfg->fps_num            = 30000;
    cfg->fps_den            = 1001;
    cfg->duration_sec       = 0;

    /* Default 16 source streams */
    cfg->num_streams = 16;

    /* Tile 0-1: poc P/R paths */
    memset(cfg->streams[0].multicast_ip, 0, sizeof(cfg->streams[0].multicast_ip));
    cfg->streams[0].udp_port = 20000;
    snprintf(cfg->streams[0].label, 64, "poc P");

    memset(cfg->streams[1].multicast_ip, 0, sizeof(cfg->streams[1].multicast_ip));
    cfg->streams[1].udp_port = 20000;
    snprintf(cfg->streams[1].label, 64, "poc R");

    /* Tile 2-15: poc_14 streams 0-13 */
    for (int i = 2; i < 16; i++) {
        memset(cfg->streams[i].multicast_ip, 0, sizeof(cfg->streams[i].multicast_ip));
        cfg->streams[i].udp_port = 20000;
        snprintf(cfg->streams[i].label, 64, "poc14 s%d", i - 2);
    }
}

static int parse_args(int argc, char **argv, comp_config_t *cfg)
{
    static struct option long_opts[] = {
        {"rx-port",        required_argument, NULL, 1001},
        {"rx-sip",         required_argument, NULL, 1002},
        {"tx-port",        required_argument, NULL, 1003},
        {"tx-sip",         required_argument, NULL, 1004},
        {"lcores",         required_argument, NULL, 1008},
        {"codec-threads",  required_argument, NULL, 1009},
        {"compression",    required_argument, NULL, 1010},
        {"fps-num",        required_argument, NULL, 1011},
        {"fps-den",        required_argument, NULL, 1012},
        {"duration",       required_argument, NULL, 1013},
        {"thumb-dir",      required_argument, NULL, 1014},
        {"help",           no_argument,       NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "h", long_opts, NULL)) != -1) {
        switch (opt) {
        case 1001: strncpy(cfg->rx_port, optarg, sizeof(cfg->rx_port)-1); break;
        case 1002: parse_ip(optarg, cfg->rx_sip); break;
        case 1003: strncpy(cfg->tx_port, optarg, sizeof(cfg->tx_port)-1); break;
        case 1004: parse_ip(optarg, cfg->tx_sip); break;
        case 1008: strncpy(cfg->lcores, optarg, sizeof(cfg->lcores)-1); break;
        case 1009: cfg->codec_threads = atoi(optarg); break;
        case 1010: cfg->compression_ratio = atoi(optarg); break;
        case 1011: cfg->fps_num = (uint32_t)atoi(optarg); break;
        case 1012: cfg->fps_den = (uint32_t)atoi(optarg); break;
        case 1013: cfg->duration_sec = atoi(optarg); break;
        case 1014: strncpy(cfg->thumb_dir, optarg, sizeof(cfg->thumb_dir)-1); break;
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
    /* Disable stdout buffering so stats appear in nohup log immediately */
    setbuf(stdout, NULL);

    compositor_t *c = &g_comp;
    memset(c, 0, sizeof(*c));
    c->running = true;

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    /* ── 1. Parse config ── */
    set_defaults(&c->cfg);
    if (parse_args(argc, argv, &c->cfg) != 0)
        return 1;

    comp_config_t *cfg = &c->cfg;

    /* Validate required fields */
    if (!cfg->rx_port[0]) {
        fprintf(stderr, "Error: --rx-port is required (e.g. 0000:xx:01.6)\n");
        return 1;
    }
    if (cfg->rx_sip[0] == 0 && cfg->rx_sip[1] == 0 &&
        cfg->rx_sip[2] == 0 && cfg->rx_sip[3] == 0) {
        fprintf(stderr, "Error: --rx-sip is required (e.g. 192.168.1.81)\n");
        return 1;
    }
    if (!cfg->tx_port[0]) {
        fprintf(stderr, "Error: --tx-port is required (e.g. 0000:xx:01.7)\n");
        return 1;
    }
    if (cfg->tx_sip[0] == 0 && cfg->tx_sip[1] == 0 &&
        cfg->tx_sip[2] == 0 && cfg->tx_sip[3] == 0) {
        fprintf(stderr, "Error: --tx-sip is required (e.g. 192.168.1.82)\n");
        return 1;
    }

    printf("\n══════════════════════════════════════════════════════\n");
    printf("  poc_8k Compositor — %d × 1080p → %dx%d @ %u/%u fps\n",
           cfg->num_streams, OUT_W, OUT_H, cfg->fps_num, cfg->fps_den);
    printf("  port P (RX): %s (SIP: %u.%u.%u.%u) — streams 0-7\n",
           cfg->rx_port, cfg->rx_sip[0], cfg->rx_sip[1],
           cfg->rx_sip[2], cfg->rx_sip[3]);
    printf("  port R (RX): %s (SIP: %u.%u.%u.%u) — streams 8-15\n",
           cfg->tx_port, cfg->tx_sip[0], cfg->tx_sip[1],
           cfg->tx_sip[2], cfg->tx_sip[3]);
    printf("  Output → shm ring /poc_8k_ring (compositor_tx sends ST22)\n");
    printf("  Encoder: SVT-JPEG-XS, ratio=%d:1, threads=%d\n",
           cfg->compression_ratio, cfg->codec_threads);
    printf("  Lcores: %s\n", cfg->lcores);
    if (cfg->thumb_dir[0])
        printf("  Thumbnail: %s/thumb.jpg\n", cfg->thumb_dir);
    printf("══════════════════════════════════════════════════════\n\n");

    /* ── 2. Allocate double tile buffers (hugepages for reduced TLB pressure) ──
     * Two 132 MB buffers allow the main (RX) thread to tile into one
     * while the encoder thread encodes from the other.
     * 2 MB hugepages reduce TLB misses from ~32K entries to ~66 each. */
    for (int bi = 0; bi < 2; bi++) {
        c->tile_bufs[bi] = mmap(NULL, OUT_FRAME_SIZE, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
        if (c->tile_bufs[bi] == MAP_FAILED) {
            fprintf(stderr, "[COMP] Hugepage mmap failed for buf[%d] (%m), "
                    "falling back to calloc\n", bi);
            c->tile_bufs[bi] = calloc(1, OUT_FRAME_SIZE);
            c->tile_hugepages[bi] = false;
        } else {
            c->tile_hugepages[bi] = true;
        }
        if (!c->tile_bufs[bi] || c->tile_bufs[bi] == MAP_FAILED) {
            fprintf(stderr, "[COMP] Failed to allocate tile buf[%d] (%d bytes)\n",
                    bi, OUT_FRAME_SIZE);
            return 1;
        }
        /* Plane pointers for this buffer */
        c->tile_planes[bi][0] = c->tile_bufs[bi];                         /* Y */
        c->tile_planes[bi][1] = c->tile_bufs[bi] + OUT_Y_SIZE;            /* U */
        c->tile_planes[bi][2] = c->tile_bufs[bi] + OUT_Y_SIZE + OUT_U_SIZE; /* V */

        /* Initialize chroma planes to 512 (10-bit neutral) for proper black. */
        uint16_t *u16 = (uint16_t *)c->tile_planes[bi][1];
        size_t chroma_samples = OUT_U_SIZE / sizeof(uint16_t);
        for (size_t s = 0; s < chroma_samples; s++)
            u16[s] = 512;
        u16 = (uint16_t *)c->tile_planes[bi][2];
        for (size_t s = 0; s < chroma_samples; s++)
            u16[s] = 512;
    }

    /* Start with buffer 0 */
    c->tile_write_idx = 0;
    c->tile_y = c->tile_planes[0][0];
    c->tile_u = c->tile_planes[0][1];
    c->tile_v = c->tile_planes[0][2];

    printf("[COMP] Double tile buffers allocated: 2 × %d bytes (%.1f MB each) [%s/%s]\n",
           OUT_FRAME_SIZE, OUT_FRAME_SIZE / 1048576.0,
           c->tile_hugepages[0] ? "hugepages" : "regular",
           c->tile_hugepages[1] ? "hugepages" : "regular");

    /* ── 2b. Init thumbnail (triple-buffered, async) ── */
    if (cfg->thumb_dir[0]) {
        if (thumb_init(&c->thumb, cfg->thumb_dir) == 0)
            c->thumb_enabled = true;
        else
            fprintf(stderr, "[COMP] WARNING: thumbnail init failed, continuing without\n");
    }

    /* ── 3. Init SVT-JPEG-XS encoder ── 
     * CRITICAL: Before creating SVT threads, restrict this thread's CPU
     * affinity to exclude the DPDK lcore cores.  SVT-JPEG-XS spawns its
     * thread pool from this context — child pthreads inherit the caller's
     * cpuset.  Without this, encoder threads float onto scheduler cores
     * (30-33), steal ~75% of their CPU via CFS, inflate the avg poll loop
     * from <1 µs to 300-600 µs, and cause millions of rx_hw_dropped_packets.
     * DPDK lcore threads created later by mtl_init() always override the
     * inherited mask with explicit rte_lcore_set_affinity(), so they are
     * unaffected by this restriction.
     */
    {
        /* Parse lcore list (e.g. "30,31,32,33,34,35,36,37") into a set */
        cpu_set_t exclude, full, restricted;
        CPU_ZERO(&exclude);
        CPU_ZERO(&full);
        CPU_ZERO(&restricted);

        char lbuf[256];
        strncpy(lbuf, cfg->lcores, sizeof(lbuf) - 1);
        lbuf[sizeof(lbuf) - 1] = '\0';
        for (char *tok = strtok(lbuf, ","); tok; tok = strtok(NULL, ",")) {
            int core = atoi(tok);
            if (core >= 0 && core < CPU_SETSIZE)
                CPU_SET(core, &exclude);
        }

        /* Build the restricted set: all online cores minus lcores */
        for (int i = 0; i < CPU_SETSIZE; i++)
            CPU_SET(i, &full);
        CPU_XOR(&restricted, &full, &exclude);

        /* Save original affinity, restrict, init encoder, restore */
        cpu_set_t saved;
        sched_getaffinity(0, sizeof(saved), &saved);
        sched_setaffinity(0, sizeof(restricted), &restricted);
        printf("[COMP] Encoder threads restricted from DPDK lcore cores: %s\n",
               cfg->lcores);
    }

    if (encoder_init(c) != 0) {
        for (int bi = 0; bi < 2; bi++) {
            if (c->tile_hugepages[bi]) munmap(c->tile_bufs[bi], OUT_FRAME_SIZE);
            else free(c->tile_bufs[bi]);
        }
        return 1;
    }

    /* Start async encoder pipeline thread (inherits restricted cpuset,
     * so it won't float onto DPDK scheduler cores). */
    if (enc_pipeline_start(c) != 0) {
        encoder_close(c);
        for (int bi = 0; bi < 2; bi++) {
            if (c->tile_hugepages[bi]) munmap(c->tile_bufs[bi], OUT_FRAME_SIZE);
            else free(c->tile_bufs[bi]);
        }
        return 1;
    }

    {
        /* Restore main thread affinity so mtl_init runs unrestricted */
        cpu_set_t full;
        CPU_ZERO(&full);
        for (int i = 0; i < CPU_SETSIZE; i++)
            CPU_SET(i, &full);
        sched_setaffinity(0, sizeof(full), &full);
    }

    /* ── 4. Init MTL (dual VF: port P = RX 0-7, port R = RX 8-15) ──
     * Each E810 VF supports max 16 queue pairs (incl. 1 for CNI mgmt).
     * Splitting 16 RX sessions evenly across two VFs avoids the limit.
     * ST22 TX is handled by the separate compositor_tx process on its
     * own dedicated VF, communicated via shared-memory ring buffer.
     */
    struct mtl_init_params p;
    memset(&p, 0, sizeof(p));

    int half = cfg->num_streams / 2;  /* 8 */
    int rest = cfg->num_streams - half;

    p.num_ports = 2;
    /* Port P — VF6: first half of RX sessions */
    snprintf(p.port[MTL_PORT_P], MTL_PORT_MAX_LEN, "%s", cfg->rx_port);
    memcpy(p.sip_addr[MTL_PORT_P], cfg->rx_sip, MTL_IP_ADDR_LEN);
    /* Port R — VF7: second half of RX sessions (RX only, no TX) */
    snprintf(p.port[MTL_PORT_R], MTL_PORT_MAX_LEN, "%s", cfg->tx_port);
    memcpy(p.sip_addr[MTL_PORT_R], cfg->tx_sip, MTL_IP_ADDR_LEN);

    p.flags = MTL_FLAG_DEV_AUTO_START_STOP;
    p.log_level = MTL_LOG_LEVEL_WARNING;
    p.nb_rx_desc = 4096;
    p.data_quota_mbs_per_sch = 21000;  /* 8 sessions/sch → balanced 8/8 split */

    /* Both ports: RX only, no TX */
    mtl_para_rx_queues_cnt_set(&p, MTL_PORT_P, (uint16_t)half);
    mtl_para_tx_queues_cnt_set(&p, MTL_PORT_P, 0);
    mtl_para_rx_queues_cnt_set(&p, MTL_PORT_R, (uint16_t)rest);
    mtl_para_tx_queues_cnt_set(&p, MTL_PORT_R, 0);

    if (cfg->lcores[0]) {
        p.lcores = (char *)cfg->lcores;
        printf("[MTL] Pinning lcores to: %s\n", cfg->lcores);
    }

    c->mtl = mtl_init(&p);
    if (!c->mtl) {
        fprintf(stderr, "[COMP] mtl_init failed\n");
        enc_pipeline_stop(c);
        encoder_close(c);
        for (int bi = 0; bi < 2; bi++) {
            if (c->tile_hugepages[bi]) munmap(c->tile_bufs[bi], OUT_FRAME_SIZE);
            else free(c->tile_bufs[bi]);
        }
        return 1;
    }
    printf("[MTL] Initialized (port P: %s %d RX, port R: %s %d RX, no TX)\n",
           cfg->rx_port, half, cfg->tx_port, rest);

    /* ── 5. Create 16 × st20p_rx sessions ── */
    c->num_rx = cfg->num_streams;

    /* Source streams are at their original fps (60fps for poc/poc_14).
     * We create the RX sessions at source fps and just drain/skip frames
     * in the compositor to output at our target fps. */
    enum st_fps source_fps = ST_FPS_P59_94;

    for (int i = 0; i < c->num_rx; i++) {
        rx_stream_t *rs = &c->rx[i];
        memset(rs, 0, sizeof(*rs));
        atomic_init(&rs->frame_avail, 0);
        atomic_init(&rs->shadow_active_idx, 0);
        atomic_init(&rs->shadow_valid, false);

        struct st20p_rx_ops ops;
        memset(&ops, 0, sizeof(ops));

        char name[32];
        snprintf(name, sizeof(name), "comp_rx_%d", i);
        ops.name = name;
        ops.priv = rs;

        /* Port — split RX sessions across VF6 (port P, 0..half-1)
         * and VF7 (port R, half..N-1) to stay within VF queue limit */
        ops.port.num_port = 1;
        memcpy(ops.port.ip_addr[MTL_SESSION_PORT_P],
               cfg->streams[i].multicast_ip, MTL_IP_ADDR_LEN);
        const char *vf_bdf = (i < half) ? cfg->rx_port : cfg->tx_port;
        snprintf(ops.port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN,
                 "%s", vf_bdf);
        ops.port.udp_port[MTL_SESSION_PORT_P] = cfg->streams[i].udp_port;

        /* Video format */
        ops.width = TILE_W;
        ops.height = TILE_H;
        ops.fps = source_fps;
        ops.interlaced = false;
        ops.transport_fmt = ST20_FMT_YUV_422_10BIT;
        ops.output_fmt = ST_FRAME_FMT_YUV422PLANAR10LE;
        ops.device = ST_PLUGIN_DEVICE_AUTO;

        ops.framebuff_cnt = 4;
        ops.flags |= ST20P_RX_FLAG_RECEIVE_INCOMPLETE_FRAME;
        ops.notify_frame_available = rx_notify_frame;

        rs->rx = st20p_rx_create(c->mtl, &ops);
        if (!rs->rx) {
            char ip_str[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, cfg->streams[i].multicast_ip,
                      ip_str, sizeof(ip_str));
            fprintf(stderr, "[COMP] st20p_rx_create failed for stream %d (%s:%u)\n",
                    i, ip_str, cfg->streams[i].udp_port);
            /* Non-fatal: continue with remaining streams */
            continue;
        }

        /* Allocate double-buffered stream shadows (ingest writes one,
         * compose reads the other, then atomic index flip). */
        for (int bi = 0; bi < 2; bi++) {
            rs->shadow_buf[bi] = (uint8_t *)calloc(1, SHADOW_FRAME_SIZE);
            if (!rs->shadow_buf[bi]) {
                fprintf(stderr, "[COMP] shadow alloc failed stream %d buf %d\n", i, bi);
                goto cleanup;
            }
            rs->shadow_y[bi] = rs->shadow_buf[bi];
            rs->shadow_u[bi] = rs->shadow_y[bi] + TILE_Y_SIZE;
            rs->shadow_v[bi] = rs->shadow_u[bi] + TILE_U_SIZE;
        }

        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, cfg->streams[i].multicast_ip,
                  ip_str, sizeof(ip_str));
        printf("[COMP] RX stream %d: %s ← %s:%u\n",
               i, cfg->streams[i].label, ip_str, cfg->streams[i].udp_port);
    }

    /* ── 5b. Init ingest worker pool (continuous RX drain → shadows) ── */
    if (ingest_workers_init(c) != 0) {
        fprintf(stderr, "[COMP] ingest worker pool init failed\n");
        goto cleanup;
    }

    /* ── 5c. Init tile worker pool (parallel tiling) ── */
    if (tile_workers_init(c) != 0) {
        fprintf(stderr, "[COMP] tile worker pool init failed, using serial tiling\n");
        /* Non-fatal: falls back to serial tile_compose() */
    }

    /* ── 6. Create shared-memory ring buffer for IPC with compositor_tx ── */
    if (shm_ring_producer_create(&c->shm_ring, "/poc_8k_ring") != 0) {
        fprintf(stderr, "[COMP] shm_ring creation failed\n");
        goto cleanup;
    }

    /* ── 7. Main compositor loop ── */
    printf("\n[COMP] Compositor running. Press Ctrl+C to stop.\n\n");
    time_t start_time = time(NULL);
    uint64_t last_stats_us = now_us();
    int consecutive_empty = 0;

    /* Target frame interval in microseconds */
    double target_interval_us = 1000000.0 * cfg->fps_den / cfg->fps_num;
    uint64_t next_frame_time = now_us();

    while (c->running) {
        /* ── Rate-limit first, then compose from latest shadows ── */
        uint64_t t_now = now_us();
        if (t_now < next_frame_time) {
            uint64_t remain = next_frame_time - t_now;
            if (remain > 1000)
                usleep((useconds_t)(remain - 500));
            else
                usleep(100);
            continue;
        }

        /* Advance frame clock. Snap forward if fell behind. */
        next_frame_time += (uint64_t)target_interval_us;
        if (next_frame_time < t_now)
            next_frame_time = t_now + (uint64_t)target_interval_us;

        int valid = count_valid_shadows(c);

        if (valid == 0) {
            consecutive_empty++;
            if (consecutive_empty > 90) {  /* ~3 s with no frames at 30fps */
                usleep(10000);
            }
            continue;
        }
        consecutive_empty = 0;

        /* ── Ensure the encoder isn't still using the buffer we want ── */
        uint64_t t_enc_wait = now_us();
        pthread_mutex_lock(&c->enc_mutex);
        while (c->enc_active_buf == c->tile_write_idx && c->enc_thread_running)
            pthread_cond_wait(&c->enc_cond_avail, &c->enc_mutex);
        pthread_mutex_unlock(&c->enc_mutex);
        atomic_fetch_add(&c->enc_wait_time_us, now_us() - t_enc_wait);

        /* Point tile planes to the current write buffer */
        c->tile_y = c->tile_planes[c->tile_write_idx][0];
        c->tile_u = c->tile_planes[c->tile_write_idx][1];
        c->tile_v = c->tile_planes[c->tile_write_idx][2];

        /* Tile compose — parallel if worker pool is active, else serial */
        uint64_t t_tile = now_us();
        if (c->tile_pool_active) {
            /* Signal tile workers to start, then wait for completion */
            pthread_barrier_wait(&c->tile_start_bar);
            pthread_barrier_wait(&c->tile_done_bar);
        } else {
            tile_compose(c);
        }
        uint64_t tile_elapsed = now_us() - t_tile;
        atomic_fetch_add(&c->tile_time_us, tile_elapsed);
        atomic_fetch_add(&c->frames_composed, 1);

        /* Submit thumbnail (async triple-buffered — off hot path) */
        if (c->thumb_enabled) {
            thumb_submit_frame(&c->thumb,
                               (const uint16_t *)c->tile_y,
                               (const uint16_t *)c->tile_u,
                               (const uint16_t *)c->tile_v,
                               OUT_W, OUT_H);
        }

        /* Hand off tiled buffer to async encoder thread (non-blocking).
         * The encoder thread encodes + writes to shm_ring independently,
         * so the main loop can immediately proceed to the next compose.
         * This keeps the DPDK RX hot-path decoupled from the ~15ms encode. */
        pthread_mutex_lock(&c->enc_mutex);
        c->enc_pending_buf = c->tile_write_idx;
        c->enc_frame_idx = c->frame_counter++;
        pthread_cond_signal(&c->enc_cond_work);
        pthread_mutex_unlock(&c->enc_mutex);

        /* Flip to the other tile buffer for next frame */
        c->tile_write_idx ^= 1;

        /* Periodic stats */
        uint64_t now_stat_us = now_us();
        if (now_stat_us - last_stats_us >= 2000000) {
            uint64_t composed = atomic_load(&c->frames_composed);
            uint64_t encoded  = atomic_load(&c->frames_encoded);
            uint64_t written  = atomic_load(&c->frames_written);
                        uint64_t tile_us  = atomic_exchange(&c->tile_time_us, 0);
                        uint64_t enc_us   = atomic_exchange(&c->encode_time_us, 0);
                        uint64_t ingest_copy_us = atomic_exchange(&c->ingest_copy_time_us, 0);
                        uint64_t ingest_updates = atomic_exchange(&c->ingest_updates, 0);
                        uint64_t enc_wait_us = atomic_exchange(&c->enc_wait_time_us, 0);
                        uint64_t shm_wait_us = atomic_exchange(&c->shm_wait_time_us, 0);
                        uint64_t shm_copy_us = atomic_exchange(&c->shm_copy_time_us, 0);
            double   interval_s = (double)(now_stat_us - last_stats_us) / 1000000.0;
            double   fps        = (double)composed / interval_s;

            printf("[COMP] %lds: composed=%lu encoded=%lu written=%lu "
                                         "fps=%.1f ingest_copy_avg=%.2fms ingest_updates=%lu enc_wait_avg=%.2fms "
                     "tile_avg=%.2fms enc_avg=%.2fms shm_wait_avg=%.2fms shm_copy_avg=%.2fms\n",
                   (long)(time(NULL) - start_time),
                   (unsigned long)composed, (unsigned long)encoded,
                   (unsigned long)written,
                   fps,
                                     (ingest_updates > 0) ? (double)ingest_copy_us / ingest_updates / 1000.0 : 0.0,
                                     (unsigned long)ingest_updates,
                                     (composed > 0) ? (double)enc_wait_us / composed / 1000.0 : 0.0,
                                     (composed > 0) ? (double)tile_us / composed / 1000.0 : 0.0,
                                     (encoded > 0) ? (double)enc_us / encoded / 1000.0 : 0.0,
                                     (written > 0) ? (double)shm_wait_us / written / 1000.0 : 0.0,
                                     (written > 0) ? (double)shm_copy_us / written / 1000.0 : 0.0);

            /* Reset stats for next interval */
            atomic_store(&c->frames_composed, 0);
            atomic_store(&c->frames_encoded, 0);
            atomic_store(&c->frames_written, 0);
            last_stats_us = now_stat_us;
        }

        /* Duration check */
        if (cfg->duration_sec > 0 &&
            (int)(time(NULL) - start_time) >= cfg->duration_sec) {
            printf("[COMP] Duration reached (%ds)\n", cfg->duration_sec);
            break;
        }
    }

cleanup:
    printf("\n[COMP] Shutting down...\n");
    c->running = false;

    /* Stop async encoder pipeline (drain + join thread) */
    enc_pipeline_stop(c);

    /* Signal compositor_tx that no more frames are coming */
    shm_ring_producer_destroy(&c->shm_ring);

    /* Destroy tile workers */
    tile_workers_destroy(c);

    /* Stop ingest workers */
    ingest_workers_stop(c);

    /* Destroy RX sessions + shadow buffers */
    for (int i = 0; i < c->num_rx; i++) {
        if (c->rx[i].rx)
            st20p_rx_free(c->rx[i].rx);
        for (int bi = 0; bi < 2; bi++) {
            free(c->rx[i].shadow_buf[bi]);
            c->rx[i].shadow_buf[bi] = NULL;
        }
    }

    /* Destroy MTL */
    if (c->mtl)
        mtl_uninit(c->mtl);

    /* Cleanup encoder */
    encoder_close(c);

    /* Cleanup thumbnail */
    if (c->thumb_enabled)
        thumb_cleanup(&c->thumb);

    /* Free double tile buffers */
    for (int bi = 0; bi < 2; bi++) {
        if (c->tile_hugepages[bi])
            munmap(c->tile_bufs[bi], OUT_FRAME_SIZE);
        else
            free(c->tile_bufs[bi]);
    }

    printf("[COMP] Clean shutdown.\n");
    return 0;
}
