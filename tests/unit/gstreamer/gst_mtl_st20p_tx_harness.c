/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Pulls in the production ST20p TX sink so the static finalize symbol is
 * reachable. MTL handles are kept NULL, so finalize's st20p_tx_free() and
 * gst_mtl_common_deinit_handle() branches are skipped and only the graceful
 * pending-GstBuffer wait runs — exactly the path under test.
 */

#include <glib.h>

/* Keep the timeout-path tests fast and deterministic: override the production
 * finalize grace (1s) before pulling in the plugin. Exercises the same drain
 * logic on a unit-friendly timescale. */
#define GST_MTL_ST20P_TX_FINALIZE_GRACE_MS 200

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "gst_mtl_st20p_tx.c"
#pragma GCC diagnostic pop

#include "gstreamer/gst_mtl_st20p_tx_harness.h"

void ut_st20p_tx_init(void) {
  gst_init(NULL, NULL);
}

Gst_Mtl_St20p_Tx* ut_st20p_tx_new(void) {
  Gst_Mtl_St20p_Tx* sink = g_object_new(GST_TYPE_MTL_ST20P_TX, NULL);
  if (!sink) return NULL;

  /* Keep teardown confined to the graceful drain: no real session/library. */
  sink->tx_handle = NULL;
  sink->mtl_lib_handle = NULL;
  sink->async_session_create = FALSE;
  g_atomic_int_set(&sink->pending_gst_buffers, 0);
  return sink;
}

void ut_st20p_tx_set_pending(Gst_Mtl_St20p_Tx* sink, gint pending) {
  g_atomic_int_set(&sink->pending_gst_buffers, pending);
}

gint ut_st20p_tx_get_pending(Gst_Mtl_St20p_Tx* sink) {
  return g_atomic_int_get(&sink->pending_gst_buffers);
}

void ut_st20p_tx_dec_pending(Gst_Mtl_St20p_Tx* sink) {
  g_atomic_int_dec_and_test(&sink->pending_gst_buffers);
}

void ut_st20p_tx_finalize(Gst_Mtl_St20p_Tx* sink) {
  gst_mtl_st20p_tx_finalize(G_OBJECT(sink));
}

/* --- Fake MTL transport ---------------------------------------------------
 * Faithfully models the slot-refcnt contract of st_tx_video_session.c without a
 * NIC. Each slot mirrors one st_frame_trans: refcnt is taken when the frame is
 * handed to the (fake) NIC and dropped when its packet mbufs are "freed". */

#define UT_FAKE_SLOTS 4

struct ut_fake_transport {
  gint refcnt[UT_FAKE_SLOTS];           /* mirrors st20_frames[i].refcnt */
  struct st_frame frame[UT_FAKE_SLOTS]; /* st_frame handed to the app; opaque=child */
  int bound[UT_FAKE_SLOTS];             /* slot holds an in-flight ext buffer */
  int stuck_frames;                     /* slots left refcnt!=0 at free */
  int refcnt_violation;                 /* "frame refcnt not zero" guard tripped */
};

static struct ut_fake_transport g_ut_fake;

void ut_st20p_tx_fake_reset(void) {
  memset(&g_ut_fake, 0, sizeof(g_ut_fake));
}

void ut_st20p_tx_fake_attach(Gst_Mtl_St20p_Tx* sink) {
  sink->tx_handle = (st20p_tx_handle)&g_ut_fake;
  sink->mtl_lib_handle = NULL;
  sink->zero_copy = TRUE;
  sink->frame_size = 1024;
  g_atomic_int_set(&sink->pending_gst_buffers, 0);
}

/* Models the transport get_next_frame refcnt guard
 * (st_tx_video_session.c: "frame %u refcnt not zero"). */
static int ut_fake_acquire(int slot) {
  if (g_atomic_int_get(&g_ut_fake.refcnt[slot]) != 0) {
    g_printerr("frame %d refcnt not zero %d\n", slot,
               g_atomic_int_get(&g_ut_fake.refcnt[slot]));
    g_ut_fake.refcnt_violation = 1;
    return -1;
  }
  g_atomic_int_set(&g_ut_fake.refcnt[slot], 1);
  return 0;
}

int ut_st20p_tx_put_ext_buffer(Gst_Mtl_St20p_Tx* sink) {
  GstSt20pTxExternalDataParent* parent;
  GstSt20pTxExternalDataChild* child;
  GstBuffer* buf;
  int slot = -1;

  for (int i = 0; i < UT_FAKE_SLOTS; i++) {
    if (!g_ut_fake.bound[i]) {
      slot = i;
      break;
    }
  }
  if (slot < 0) return -1;

  buf = gst_buffer_new_allocate(NULL, sink->frame_size, NULL);
  if (!buf) return -1;

  parent = malloc(sizeof(*parent));
  parent->buf = buf;
  parent->child_count = 1;
  pthread_mutex_init(&parent->parent_mutex, NULL);

  child = malloc(sizeof(*child));
  memset(child, 0, sizeof(*child));
  child->parent = parent;
  child->gst_buffer_memory = gst_buffer_peek_memory(buf, 0);
  if (!gst_memory_map(child->gst_buffer_memory, &child->map_info, GST_MAP_READ)) {
    free(child);
    pthread_mutex_destroy(&parent->parent_mutex);
    gst_buffer_unref(buf);
    free(parent);
    return -1;
  }

  /* chain(): one in-flight GstBuffer */
  g_atomic_int_inc(&sink->pending_gst_buffers);

  /* put_ext_frame + tasklet acquire bind the ext buffer to a transport slot */
  g_ut_fake.frame[slot].opaque = child;
  g_ut_fake.bound[slot] = 1;
  ut_fake_acquire(slot);
  return slot;
}

void ut_st20p_tx_transport_reacquire(Gst_Mtl_St20p_Tx* sink, int slot) {
  (void)sink;
  if (slot < 0 || slot >= UT_FAKE_SLOTS) return;
  ut_fake_acquire(slot);
}

void ut_st20p_tx_transport_complete(Gst_Mtl_St20p_Tx* sink, int slot) {
  if (slot < 0 || slot >= UT_FAKE_SLOTS) return;
  /* tv_frame_free_cb: the NIC freed every packet mbuf. Drop the transport
   * refcnt first, then notify the app. Ordering refcnt-before-notify keeps the
   * pending counter authoritative for a concurrent finalize: once pending hits
   * 0 (decremented inside frame_done) the slot is already free, so st20p_tx_free
   * can never observe a half-completed frame. */
  g_atomic_int_set(&g_ut_fake.refcnt[slot], 0);
  g_ut_fake.bound[slot] = 0;
  gst_mtl_st20p_tx_frame_done(sink, &g_ut_fake.frame[slot]);
}

int ut_st20p_tx_fake_stuck_frames(void) {
  int n = 0;
  for (int i = 0; i < UT_FAKE_SLOTS; i++)
    if (g_atomic_int_get(&g_ut_fake.refcnt[i]) != 0) n++;
  return n;
}

int ut_st20p_tx_fake_refcnt_violation(void) {
  return g_ut_fake.refcnt_violation;
}

/* --- Fakes for the libmtl symbols the teardown path executes ----------------
 * The real symbols live in libmtl but need a NIC; -Wl,--allow-multiple-definition
 * (set in unit_link_args) lets these in-tree models win at link time because the
 * executable's own objects precede the archive. */
int st20p_tx_notify_ext_frame_free(st20p_tx_handle handle, struct st_frame* frame) {
  (void)handle;
  (void)frame; /* slot lifetime is modeled by the ut_* helpers */
  return 0;
}

int st20p_tx_free(st20p_tx_handle handle) {
  struct ut_fake_transport* t = (struct ut_fake_transport*)handle;
  if (!t) return 0;
  /* Models st20p_tx_free's bounded flush: it does NOT wait for IN_TRANSMITTING
   * frames to truly drain (the 50ms WA in tx_st20p_framebuffs_flush), so a frame
   * the NIC never completed is abandoned with refcnt still set — exactly the
   * stuck-frame / "frame are not free, refcnt" symptom. */
  for (int i = 0; i < UT_FAKE_SLOTS; i++) {
    if (g_atomic_int_get(&t->refcnt[i]) != 0) {
      g_printerr("frame %d are not free, refcnt %d\n", i,
                 g_atomic_int_get(&t->refcnt[i]));
      t->stuck_frames++;
      t->refcnt_violation = 1;
    }
  }
  return 0;
}
