/* SPDX-License-Identifier: BSD-3-Clause */
#include <rte_atomic.h>
#include <rte_eal.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_log.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_udp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "st20_api.h"
#include "st2110/st_fmt.h"
#include "st2110/st_pkt.h"
#include "st2110/st_rx_video_session.h"
#include "st40_api.h"
#include "st41_api.h"
#include "st_api.h"

extern int mt_set_log_global_level(enum mtl_log_level level);

static void st22_fuzz_log_printer(enum mtl_log_level level, const char* format, ...) {
  MTL_MAY_UNUSED(level);
  va_list args;
  va_start(args, format);
  vfprintf(stderr, format, args);
  va_end(args);
}

#define ST22_FUZZ_POOL_SIZE 2048
#define ST22_FUZZ_POOL_NAME "st22_rx_fuzz_pool"
#define ST22_FUZZ_MAX_PKT_SIZE 4096
#define ST22_FUZZ_MIN_PKT_SIZE (sizeof(struct st22_rfc9134_video_hdr))
#define ST22_FUZZ_FRAME_COUNT 2
#define ST22_FUZZ_WIDTH 1920
#define ST22_FUZZ_HEIGHT 1080
#define ST22_FUZZ_FRAME_SIZE_BYTES (1024 * 1024)
#define ST22_FUZZ_BITMAP_SIZE ((ST22_FUZZ_FRAME_SIZE_BYTES / 800 / 8) + 16)

static struct mtl_main_impl g_impl;
static struct st_rx_video_sessions_mgr g_mgr;
static struct st_rx_video_session_impl g_session;
static struct rte_mempool* g_pool;
static bool g_eal_ready;

static struct st_frame_trans g_frames[ST22_FUZZ_FRAME_COUNT];
static uint8_t g_frame_storage[ST22_FUZZ_FRAME_COUNT][ST22_FUZZ_FRAME_SIZE_BYTES];
static uint8_t g_frame_user_meta[ST22_FUZZ_FRAME_COUNT][256];
static uint8_t g_slot_bitmaps[ST_VIDEO_RX_REC_NUM_OFO][ST22_FUZZ_BITMAP_SIZE];
static struct st22_rx_video_info g_st22_info;

static void st22_fuzz_enable_logging(void) {
  static bool logging_ready;
  if (logging_ready) return;

  mt_set_log_global_level(MTL_LOG_LEVEL_DEBUG);
  rte_log_set_global_level(RTE_LOG_DEBUG);
  mtl_set_log_printer(st22_fuzz_log_printer);
  if (mtl_openlog_stream(stderr) < 0) {
    fprintf(stderr, "st22 fuzz: failed to route MTL logs to stderr\n");
  }
  logging_ready = true;
}

static uint64_t st22_fuzz_ptp_time(struct mtl_main_impl* impl, enum mtl_port port) {
  MTL_MAY_UNUSED(port);
  impl->ptp_usync += NS_PER_US;
  impl->ptp_usync_tsc = mt_get_tsc(impl);
  return impl->ptp_usync;
}

static void st22_fuzz_prepare_frames(void) {
  for (int i = 0; i < ST22_FUZZ_FRAME_COUNT; i++) {
    struct st_frame_trans* frame = &g_frames[i];
    memset(frame, 0, sizeof(*frame));
    frame->idx = i;
    frame->addr = g_frame_storage[i];
    frame->user_meta = g_frame_user_meta[i];
    frame->user_meta_buffer_size = sizeof(g_frame_user_meta[i]);
    rte_atomic32_set(&frame->refcnt, 0);
  }
}

static int st22_fuzz_notify_frame_ready(void* priv, void* frame,
                                        struct st22_rx_frame_meta* meta) {
  MTL_MAY_UNUSED(meta);
  struct st_rx_video_session_impl* s = priv;
  if (!s || !frame) return 0;

  for (int i = 0; i < s->st20_frames_cnt; i++) {
    if (s->st20_frames && s->st20_frames[i].addr == frame) {
      rte_atomic32_set(&s->st20_frames[i].refcnt, 0);
      break;
    }
  }
  return 0;
}

static int st22_fuzz_release_frame(void* priv, void* frame,
                                   struct st20_rx_frame_meta* meta) {
  MTL_MAY_UNUSED(meta);
  return st22_fuzz_notify_frame_ready(priv, frame, NULL);
}

static void st22_fuzz_init_eal(void) {
  st22_fuzz_enable_logging();
  if (g_pool) return;

  if (!g_eal_ready) {
    static char arg0[] = "st22_rx_fuzz";
    static char arg1[] = "--no-huge";
    static char arg2[] = "--no-shconf";
    static char arg3[] = "-c1";
    static char arg4[] = "-n1";
    static char arg5[] = "--no-pci";
    static char arg6[] = "--vdev=net_null0";
    char* eal_args[] = {arg0, arg1, arg2, arg3, arg4, arg5, arg6};
    const int eal_argc = (int)(sizeof(eal_args) / sizeof(eal_args[0]));
    if (rte_eal_init(eal_argc, eal_args) < 0) abort();
    g_eal_ready = true;
  }

  g_pool = rte_pktmbuf_pool_create(ST22_FUZZ_POOL_NAME, ST22_FUZZ_POOL_SIZE, 0, 0,
                                   RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
  if (!g_pool) abort();
}

static void st22_fuzz_reset_context(void) {
  if (!g_pool) return;

  memset(&g_impl, 0, sizeof(g_impl));
  memset(&g_mgr, 0, sizeof(g_mgr));
  memset(&g_session, 0, sizeof(g_session));
  memset(&g_st22_info, 0, sizeof(g_st22_info));

  g_impl.type = MT_HANDLE_MAIN;
  g_impl.tsc_hz = rte_get_tsc_hz();
  g_impl.inf[MTL_PORT_P].parent = &g_impl;
  g_impl.inf[MTL_PORT_P].port = MTL_PORT_P;
  g_impl.inf[MTL_PORT_P].ptp_get_time_fn = st22_fuzz_ptp_time;

  g_mgr.parent = &g_impl;
  g_mgr.idx = 0;
  g_mgr.max_idx = 1;
  g_mgr.sessions[0] = &g_session;

  g_session.idx = 0;
  g_session.socket_id = rte_socket_id();
  g_session.parent = &g_mgr;
  g_session.impl = &g_impl;
  g_session.attached = true;
  g_session.rx_burst_size = 4;
  g_session.port_maps[MTL_SESSION_PORT_P] = MTL_PORT_P;
  g_session.st20_dst_port[MTL_SESSION_PORT_P] = 10010;
  g_session.detector.status = ST20_DETECT_STAT_DISABLED;

  g_session.ops.num_port = 1;
  g_session.ops.type = ST20_TYPE_FRAME_LEVEL;
  g_session.ops.width = ST22_FUZZ_WIDTH;
  g_session.ops.height = ST22_FUZZ_HEIGHT;
  g_session.ops.fps = ST_FPS_P59_94;
  g_session.ops.interlaced = false;
  g_session.ops.fmt = ST20_FMT_YUV_422_10BIT;
  g_session.ops.payload_type = 0;
  g_session.ops.packing = ST20_PACKING_BPM;
  g_session.ops.framebuff_cnt = ST22_FUZZ_FRAME_COUNT;
  g_session.ops.notify_frame_ready = st22_fuzz_release_frame;
  g_session.ops.priv = &g_session;
  g_session.ops.name = "st22_rx_fuzz";
  g_session.ops.udp_port[MTL_SESSION_PORT_P] = 10010;

  if (st20_get_pgroup(g_session.ops.fmt, &g_session.st20_pg) < 0) abort();

  g_session.st20_frames = g_frames;
  g_session.st20_frames_cnt = ST22_FUZZ_FRAME_COUNT;
  g_session.st20_frame_size = ST22_FUZZ_FRAME_SIZE_BYTES;
  g_session.st20_fb_size = g_session.st20_frame_size;
  g_session.st20_uframe_size = 0;
  g_session.slice_lines = 0;
  g_session.slice_size = 0;
  g_session.st20_frame_bitmap_size = ST22_FUZZ_BITMAP_SIZE;

  size_t raw_bytes = (size_t)ST22_FUZZ_WIDTH * g_session.st20_pg.size;
  g_session.st20_bytes_in_line =
      (raw_bytes + g_session.st20_pg.coverage - 1) / g_session.st20_pg.coverage;
  g_session.st20_linesize = g_session.st20_bytes_in_line;

  double fps = st_frame_rate(g_session.ops.fps);
  if (fps <= 0.0) fps = 60.0;
  g_session.frame_time = (double)NS_PER_S / fps;
  g_session.frame_time_sampling = g_session.frame_time;
  int estimated_pkts = g_session.st20_frame_size / ST_VIDEO_BPM_SIZE;
  if (estimated_pkts <= 0) estimated_pkts = 1;
  g_session.trs = g_session.frame_time / estimated_pkts;

  g_session.st22_info = &g_st22_info;
  g_st22_info.notify_frame_ready = st22_fuzz_notify_frame_ready;
  g_session.st22_ops_flags = 0;
  g_session.st22_expect_frame_size = 0;
  g_session.st22_expect_size_per_frame = ST22_FUZZ_FRAME_SIZE_BYTES;

  st22_fuzz_prepare_frames();
  for (int i = 0; i < ST_VIDEO_RX_REC_NUM_OFO; i++) {
    g_session.slots[i].frame_bitmap = g_slot_bitmaps[i];
  }

  st_rx_video_session_fuzz_reset(&g_session);
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (!data || size < ST22_FUZZ_MIN_PKT_SIZE) return 0;

  size_t pkt_size = size;
  if (pkt_size > ST22_FUZZ_MAX_PKT_SIZE) pkt_size = ST22_FUZZ_MAX_PKT_SIZE;

  st22_fuzz_init_eal();
  st22_fuzz_reset_context();

  struct rte_mbuf* mbuf = rte_pktmbuf_alloc(g_pool);
  if (!mbuf) return 0;

  if (rte_pktmbuf_tailroom(mbuf) < pkt_size) {
    rte_pktmbuf_free(mbuf);
    return 0;
  }

  uint8_t* dst = rte_pktmbuf_mtod(mbuf, uint8_t*);
  memcpy(dst, data, pkt_size);
  mbuf->data_len = pkt_size;
  mbuf->pkt_len = pkt_size;

  st_rx_video_session_fuzz_handle_pkt(&g_session, mbuf, MTL_SESSION_PORT_P);
  rte_pktmbuf_free(mbuf);
  return 0;
}
