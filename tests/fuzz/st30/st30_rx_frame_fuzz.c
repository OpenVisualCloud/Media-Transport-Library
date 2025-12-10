/* SPDX-License-Identifier: BSD-3-Clause */
#include <rte_atomic.h>
#include <rte_eal.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_lcore.h>
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
#include "st2110/st_pkt.h"
#include "st2110/st_rx_audio_session.h"
#include "st30_api.h"
#include "st40_api.h"
#include "st41_api.h"
#include "st_api.h"

extern int mt_set_log_global_level(enum mtl_log_level level);

static void st30_fuzz_log_printer(enum mtl_log_level level, const char* format, ...) {
  MTL_MAY_UNUSED(level);
  va_list args;
  va_start(args, format);
  vfprintf(stderr, format, args);
  va_end(args);
}

#define ST30_FUZZ_POOL_SIZE 2048
#define ST30_FUZZ_POOL_NAME "st30_rx_fuzz_pool"
#define ST30_FUZZ_MAX_PKT_SIZE 2048
#define ST30_FUZZ_MIN_PKT_SIZE (sizeof(struct st_rfc3550_audio_hdr))
#define ST30_FUZZ_FRAME_COUNT 2
#define ST30_FUZZ_FRAME_CAPACITY 8192

static struct mtl_main_impl g_impl;
static struct st_rx_audio_sessions_mgr g_mgr;
static struct st_rx_audio_session_impl g_session;
static struct rte_mempool* g_pool;
static bool g_eal_ready;

static struct st_frame_trans g_frames[ST30_FUZZ_FRAME_COUNT];
static uint8_t g_frame_storage[ST30_FUZZ_FRAME_COUNT][ST30_FUZZ_FRAME_CAPACITY];

static void st30_fuzz_enable_logging(void) {
  static bool logging_ready;
  if (logging_ready) return;

  mt_set_log_global_level(MTL_LOG_LEVEL_DEBUG);
  rte_log_set_global_level(RTE_LOG_DEBUG);
  mtl_set_log_printer(st30_fuzz_log_printer);
  if (mtl_openlog_stream(stderr) < 0) {
    fprintf(stderr, "st30 fuzz: failed to route MTL logs to stderr\n");
  }
  logging_ready = true;
}

static int st30_fuzz_notify_frame_ready(void* priv, void* frame,
                                        struct st30_rx_frame_meta* meta) {
  MTL_MAY_UNUSED(meta);
  struct st_rx_audio_session_impl* s = priv;
  if (!s || !frame) return 0;

  for (int i = 0; i < s->st30_frames_cnt; i++) {
    if (!s->st30_frames) break;
    if (s->st30_frames[i].addr == frame) {
      rte_atomic32_dec(&s->st30_frames[i].refcnt);
      break;
    }
  }
  return 0;
}

static void st30_fuzz_prepare_frames(void) {
  for (int i = 0; i < ST30_FUZZ_FRAME_COUNT; i++) {
    memset(&g_frames[i], 0, sizeof(g_frames[i]));
    g_frames[i].idx = i;
    g_frames[i].addr = g_frame_storage[i];
    rte_atomic32_set(&g_frames[i].refcnt, 0);
  }
  g_session.st30_frames = g_frames;
  g_session.st30_frames_cnt = ST30_FUZZ_FRAME_COUNT;
}

static void st30_fuzz_init_impl(void) {
  st30_fuzz_enable_logging();
  if (g_pool) return;

  if (!g_eal_ready) {
    static char st30_arg0[] = "st30_rx_fuzz";
    static char st30_arg1[] = "--no-huge";
    static char st30_arg2[] = "--no-shconf";
    static char st30_arg3[] = "-c1";
    static char st30_arg4[] = "-n1";
    static char* eal_args[] = {st30_arg0, st30_arg1, st30_arg2, st30_arg3, st30_arg4};
    static const int eal_argc = (int)(sizeof(eal_args) / sizeof(eal_args[0]));

    if (rte_eal_init(eal_argc, eal_args) < 0) {
      abort();
    }
    g_eal_ready = true;
  }

  g_pool = rte_pktmbuf_pool_create(ST30_FUZZ_POOL_NAME, ST30_FUZZ_POOL_SIZE, 0, 0,
                                   RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
  if (!g_pool) abort();
}

static void st30_fuzz_reset_context(size_t payload_len) {
  if (!g_pool) return;

  memset(&g_impl, 0, sizeof(g_impl));
  memset(&g_mgr, 0, sizeof(g_mgr));
  memset(&g_session, 0, sizeof(g_session));

  g_impl.type = MT_HANDLE_MAIN;
  g_impl.tsc_hz = rte_get_tsc_hz();

  g_mgr.parent = &g_impl;
  g_mgr.idx = 0;
  g_mgr.sessions[0] = &g_session;
  g_mgr.max_idx = 1;

  g_session.idx = 0;
  g_session.socket_id = rte_socket_id();
  g_session.mgr = &g_mgr;
  g_session.attached = true;
  g_session.ops.type = ST30_TYPE_FRAME_LEVEL;
  g_session.ops.num_port = 1;
  g_session.ops.channel = 2;
  g_session.ops.sampling = ST30_SAMPLING_48K;
  g_session.ops.fmt = ST30_FMT_PCM16;
  g_session.ops.ptime = ST30_PTIME_1MS;
  g_session.ops.framebuff_cnt = ST30_FUZZ_FRAME_COUNT;
  g_session.ops.notify_frame_ready = st30_fuzz_notify_frame_ready;
  g_session.ops.priv = &g_session;
  g_session.ops.name = "st30_rx_fuzz";

  st30_fuzz_prepare_frames();

  size_t payload = payload_len ? payload_len : 1;
  size_t pkt_multiple = ST30_FUZZ_FRAME_CAPACITY / payload;
  if (!pkt_multiple) pkt_multiple = 1;
  size_t frame_bytes = pkt_multiple * payload;
  if (!frame_bytes) {
    frame_bytes = payload;
    pkt_multiple = 1;
  }

  g_session.pkt_len = (uint32_t)payload;
  g_session.st30_total_pkts = (int)pkt_multiple;
  g_session.st30_frame_size = frame_bytes;
  g_session.ops.framebuff_size = (uint32_t)frame_bytes;
  g_session.st30_pkt_size = (uint32_t)(payload + sizeof(struct st_rfc3550_audio_hdr));
  g_session.port_maps[MTL_SESSION_PORT_P] = MTL_PORT_P;
  g_session.usdt_dump_fd = -1;

  st_rx_audio_session_fuzz_reset(&g_session);
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (!data) return 0;

  st30_fuzz_init_impl();

  size_t pkt_size = size;
  if (pkt_size > ST30_FUZZ_MAX_PKT_SIZE) pkt_size = ST30_FUZZ_MAX_PKT_SIZE;
  if (pkt_size < ST30_FUZZ_MIN_PKT_SIZE) return 0;

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

  size_t payload = pkt_size - sizeof(struct st_rfc3550_audio_hdr);
  st30_fuzz_reset_context(payload);

  st_rx_audio_session_fuzz_handle_pkt(&g_impl, &g_session, mbuf, MTL_SESSION_PORT_P);
  rte_pktmbuf_free(mbuf);
  return 0;
}
