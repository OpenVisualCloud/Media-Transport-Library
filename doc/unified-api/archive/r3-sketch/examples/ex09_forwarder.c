/* 10 §11 — zero-copy split forwarder 1 -> N (P5): one 2160p RX frame feeds four 1080p TX
   sessions concurrently. Each TX session has its own attached buffers over the RX library
   pool region (offset + half-width rows, DIRECT because stride >= row_bytes), and every
   submission holds the RX lease (.hold) until its TX result. */
#include "ex_common.h"

#define QUADS 4
#define MAX_SLOTS 16

static mtl_buffer_h txb[QUADS][MAX_SLOTS];

/* rx: a created 2160p RX session (library pool). tx[q]: created 1080p TX sessions with
   pool.source = MTL_POOL_ATTACHED, pool.count = the RX pool count, and the processor
   timing of 10 §12 (TAI + CAPTURE + min_tx_delay_ns). */
static int build_tx_layouts(mtl_session_h rx, mtl_session_h* tx, uint32_t* nslots) {
  mtl_buffer_h rxb[MAX_SLOTS];
  int ret = mtl_session_get_buffers(rx, rxb, MAX_SLOTS, nslots);
  if (ret < 0) return ret;
  if (*nslots > MAX_SLOTS) return -MTL_ENOSPC;
  for (uint32_t q = 0; q < QUADS; q++) {
    for (uint32_t j = 0; j < *nslots; j++) {
      struct mtl_buffer_desc d;
      mtl_buffer_desc_init(&d);
      /* region = the RX pool region (READ | WRITE) */
      ret = mtl_buffer_get_desc(rxb[j], &d);
      if (ret < 0) return ret;
      struct mtl_plane_desc* p = &d.plane[0];
      p->row_bytes /= 2; /* 1920 of 3840 pixels */
      p->rows /= 2;      /* 1080 of 2160 lines */
      p->offset +=
          (uint64_t)(q / 2) * p->rows * p->stride + (uint64_t)(q % 2) * p->row_bytes;
      p->span = (uint64_t)p->stride * (p->rows - 1) + p->row_bytes;
      d.user_cookie = j;
      ret = mtl_buffer_create(&d, &txb[q][j]);
      if (ret < 0) return ret;
    }
    ret = mtl_session_attach_buffers(tx[q], txb[q], *nslots); /* TX needs MTL_MEM_READ */
    if (ret < 0) return ret;
  }
  return 0;
}

static void reap_nonblocking(mtl_session_h* tx) {
  for (uint32_t q = 0; q < QUADS; q++) {
    struct mtl_tx_result r[8];
    /* ALL is forced for attached pools */
    (void)mtl_tx_reap(tx[q], r, sizeof(r[0]), 8, 0);
  }
}

int split_forward(mtl_session_h rx, mtl_session_h* tx) {
  uint32_t nslots;
  int ret = build_tx_layouts(rx, tx, &nslots);
  if (ret < 0) return ex_fail("layouts", ret);
  for (uint32_t q = 0; ret >= 0 && q < QUADS; q++) ret = mtl_session_start(tx[q], NULL);
  if (ret >= 0) ret = mtl_session_start(rx, NULL);
  if (ret < 0) return ex_fail("start", ret);

  while (g_running) {
    mtl_lease_h in;
    struct mtl_rx_unit u;
    reap_nonblocking(tx);
    ret = mtl_rx_dequeue(rx, &in, NULL, &u, sizeof(u), MTL_MS(50));
    if (ret == -MTL_ETIMEDOUT) continue;
    if (ret < 0) break;
    uint32_t j = mtl_buffer_index(mtl_lease_buffer(in));
    for (uint32_t q = 0; (u.hdr.time_valid & MTL_RT_MEDIA) && q < QUADS; q++) {
      mtl_lease_h out;
      int r = mtl_tx_acquire_buffer(tx[q], txb[q][j], &out, NULL, NULL, 0);
      /* -MTL_EBUSY: that surface is still in flight; this quad drops */
      if (r < 0) continue;
      struct mtl_tx_submission sub;
      mtl_tx_submission_init(&sub);
      /* derived RTP = input RTP if it was compliant */
      sub.media_tai_ns = u.timing.media_tai_ns;
      sub.hold = in; /* the RX slot stays allocated until this unit's TX result */
      if (mtl_tx_submit(tx[q], out, &sub) < 0) mtl_tx_release(tx[q], out);
    }
    ret = mtl_rx_release(rx, in); /* our reference; FREE once every hold has completed */
    if (ret < 0) break;
  }
  if (ret < 0) ex_fail("forward", ret);
  mtl_session_stop(rx, MTL_STOP_FLUSH, 0);
  for (uint32_t q = 0; q < QUADS; q++)
    mtl_session_stop(tx[q], MTL_STOP_DRAIN, MTL_MS(100));
  reap_nonblocking(tx);
  return ret;
}
