/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * C harness that exposes the ST20p TX sink's static finalize path
 * (gst_mtl_st20p_tx_finalize) to the unit tests, so its graceful-shutdown
 * grace period can be exercised without real MTL hardware.
 *
 * The sink is created with its MTL handles left NULL, so finalize's
 * st20p_tx_free() / gst_mtl_common_deinit_handle() branches are skipped and
 * only the pending-GstBuffer drain runs — the behaviour under test.
 */

#ifndef UT_GST_MTL_ST20P_TX_HARNESS_H
#define UT_GST_MTL_ST20P_TX_HARNESS_H

#include <glib.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque to the tests; the real struct lives in the production header. */
typedef struct _Gst_Mtl_St20p_Tx Gst_Mtl_St20p_Tx;

/* One-time GStreamer init. */
void ut_st20p_tx_init(void);

/* Create a sink whose MTL handles are NULL, confining finalize to the
 * graceful pending-buffer wait. Returns NULL on failure. */
Gst_Mtl_St20p_Tx* ut_st20p_tx_new(void);

void ut_st20p_tx_set_pending(Gst_Mtl_St20p_Tx* sink, gint pending);
gint ut_st20p_tx_get_pending(Gst_Mtl_St20p_Tx* sink);
void ut_st20p_tx_dec_pending(Gst_Mtl_St20p_Tx* sink);

/* Invoke the real (static) finalize — the graceful-shutdown unit under test. */
void ut_st20p_tx_finalize(Gst_Mtl_St20p_Tx* sink);

/* --- Fake MTL transport ---------------------------------------------------
 * Models the st20_frames[] slot-refcnt contract from st_tx_video_session.c so
 * the zero-copy teardown path can be exercised without a NIC. A transport slot
 * is refcnt=1 from the moment a frame is handed to the (fake) NIC until the
 * frame's packet mbufs are "freed" — mirroring how the real driver drives
 * notify_frame_done. */

/* Reset all fake-transport state between tests. */
void ut_st20p_tx_fake_reset(void);

/* Point sink->tx_handle at the fake transport and put the sink in zero-copy
 * mode with a small frame size; MTL lib handle stays NULL. */
void ut_st20p_tx_fake_attach(Gst_Mtl_St20p_Tx* sink);

/* Model chain()+zero_copy()+tasklet-acquire for one single-memory GstBuffer:
 * allocate+map a real GstBuffer, build the plugin's parent/child structs,
 * increment pending, bind to a transport slot and take the refcnt (0->1).
 * Returns the slot index (>=0) or -1 on failure. */
int ut_st20p_tx_put_ext_buffer(Gst_Mtl_St20p_Tx* sink);

/* Model the tasklet's get_next_frame acquire on an already-bound slot (the
 * slot-reuse-before-drain window). Trips the "frame refcnt not zero" guard if
 * the slot is still referenced. */
void ut_st20p_tx_transport_reacquire(Gst_Mtl_St20p_Tx* sink, int slot);

/* Model tv_frame_free_cb after the NIC freed every packet mbuf: fire the real
 * app frame_done (unmap + unref + pending--), then drop the transport refcnt. */
void ut_st20p_tx_transport_complete(Gst_Mtl_St20p_Tx* sink, int slot);

/* Observers. */
int ut_st20p_tx_fake_stuck_frames(void);     /* slots left refcnt!=0 */
int ut_st20p_tx_fake_refcnt_violation(void); /* "refcnt not zero" guard tripped */

#ifdef __cplusplus
}
#endif

#endif /* UT_GST_MTL_ST20P_TX_HARNESS_H */
