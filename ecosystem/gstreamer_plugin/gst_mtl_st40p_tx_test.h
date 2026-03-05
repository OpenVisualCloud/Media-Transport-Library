/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright(c) 2025 Intel Corporation
 *
 * GStreamer ST40 TX test-mode hooks.
 *
 * In debug builds (MTL_SIMULATE_PACKET_DROPS defined) the functions below register
 * test-only GObject properties, handle set/get, configure ops_tx.test,
 * and dispatch to the synthetic test-frame builder.
 *
 * In release builds every function is a no-op / constant so the main
 * source carries no #ifdef noise.
 */

#ifndef __GST_MTL_ST40P_TX_TEST_H__
#define __GST_MTL_ST40P_TX_TEST_H__

#include "gst_mtl_st40p_tx.h"

/* ------------------------------------------------------------------ */
#ifdef MTL_SIMULATE_PACKET_DROPS
/* ========================  DEBUG BUILD  =========================== */

#include <st40_api.h>

/* --- GType for the test-mode enum --------------------------------- */
static inline GType gst_mtl_st40p_tx_test_mode_get_type(void) {
  static gsize g_define_type_id__ = 0;
  if (g_once_init_enter(&g_define_type_id__)) {
    static const GEnumValue values[] = {
        {GST_MTL_ST40P_TX_TEST_MODE_NONE, "None", "none"},
        {GST_MTL_ST40P_TX_TEST_MODE_NO_MARKER, "NoMarker", "no-marker"},
        {GST_MTL_ST40P_TX_TEST_MODE_SEQ_GAP, "SeqGap", "seq-gap"},
        {GST_MTL_ST40P_TX_TEST_MODE_BAD_PARITY, "BadParity", "bad-parity"},
        {GST_MTL_ST40P_TX_TEST_MODE_PACED, "Paced", "paced"},
        {0, NULL, NULL}};
    GType g_define_type_id = g_enum_register_static("GstMtlSt40pTxTestMode", values);
    g_once_init_leave(&g_define_type_id__, g_define_type_id);
  }
  return g_define_type_id__;
}
#define GST_TYPE_MTL_ST40P_TX_TEST_MODE (gst_mtl_st40p_tx_test_mode_get_type())

/* --- Property installation ---------------------------------------- */
static inline void gst_mtl_st40p_tx_test_install_properties(GObjectClass* gobject_class) {
  g_object_class_install_property(
      gobject_class, PROP_ST40P_TX_TEST_MODE,
      g_param_spec_enum("tx-test-mode", "Test mutation mode",
                        "DEBUG: Apply test-only RTP/ANC mutations (for validation only)",
                        GST_TYPE_MTL_ST40P_TX_TEST_MODE, GST_MTL_ST40P_TX_TEST_MODE_NONE,
                        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_ST40P_TX_TEST_PKT_COUNT,
      g_param_spec_uint("tx-test-pkt-count", "Test packet count",
                        "DEBUG: Number of ANC packets to emit when a test mode is active"
                        " (0 uses a mode-specific default)",
                        0, G_MAXUINT, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_ST40P_TX_TEST_PACING_NS,
      g_param_spec_uint("tx-test-pacing-ns", "Test pacing interval (ns)",
                        "DEBUG: Inter-packet spacing to use when tx-test-mode=paced (ns)",
                        0, G_MAXUINT, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
}

/* --- Init defaults ------------------------------------------------ */
static inline void gst_mtl_st40p_tx_test_init(Gst_Mtl_St40p_Tx* sink) {
  sink->test_mode = GST_MTL_ST40P_TX_TEST_MODE_NONE;
  sink->test_pkt_count = 0;
  sink->test_pacing_ns = 0;
}

/* --- set_property / get_property ---------------------------------- */
static inline gboolean gst_mtl_st40p_tx_test_set_property(Gst_Mtl_St40p_Tx* self,
                                                          guint prop_id,
                                                          const GValue* value) {
  switch (prop_id) {
    case PROP_ST40P_TX_TEST_MODE:
      self->test_mode = (GstMtlSt40pTxTestMode)g_value_get_enum(value);
      return TRUE;
    case PROP_ST40P_TX_TEST_PKT_COUNT:
      self->test_pkt_count = g_value_get_uint(value);
      return TRUE;
    case PROP_ST40P_TX_TEST_PACING_NS:
      self->test_pacing_ns = g_value_get_uint(value);
      return TRUE;
    default:
      return FALSE;
  }
}

static inline gboolean gst_mtl_st40p_tx_test_get_property(Gst_Mtl_St40p_Tx* sink,
                                                          guint prop_id, GValue* value) {
  switch (prop_id) {
    case PROP_ST40P_TX_TEST_MODE:
      g_value_set_enum(value, sink->test_mode);
      return TRUE;
    case PROP_ST40P_TX_TEST_PKT_COUNT:
      g_value_set_uint(value, sink->test_pkt_count);
      return TRUE;
    case PROP_ST40P_TX_TEST_PACING_NS:
      g_value_set_uint(value, sink->test_pacing_ns);
      return TRUE;
    default:
      return FALSE;
  }
}

/* --- Session create: fill ops_tx.test ----------------------------- */
static inline void gst_mtl_st40p_tx_test_configure_ops(Gst_Mtl_St40p_Tx* sink,
                                                       struct st40p_tx_ops* ops_tx) {
  switch (sink->test_mode) {
    case GST_MTL_ST40P_TX_TEST_MODE_NO_MARKER:
      ops_tx->test.pattern = ST40_TX_TEST_NO_MARKER;
      break;
    case GST_MTL_ST40P_TX_TEST_MODE_SEQ_GAP:
      ops_tx->test.pattern = ST40_TX_TEST_SEQ_GAP;
      break;
    case GST_MTL_ST40P_TX_TEST_MODE_BAD_PARITY:
      ops_tx->test.pattern = ST40_TX_TEST_BAD_PARITY;
      break;
    case GST_MTL_ST40P_TX_TEST_MODE_PACED:
      ops_tx->test.pattern = ST40_TX_TEST_PACED;
      break;
    case GST_MTL_ST40P_TX_TEST_MODE_NONE:
    default:
      ops_tx->test.pattern = ST40_TX_TEST_NONE;
      break;
  }
  if (ops_tx->test.pattern != ST40_TX_TEST_NONE) {
    ops_tx->test.frame_count = 1;
    ops_tx->test.paced_pkt_count = sink->test_pkt_count;
    ops_tx->test.paced_gap_ns = sink->test_pacing_ns;
    if (!sink->split_anc_by_pkt) {
      ops_tx->flags |= ST40P_TX_FLAG_SPLIT_ANC_BY_PKT;
      sink->split_anc_by_pkt = TRUE;
    }
  }
}

/* --- Render: build synthetic test frame --------------------------- */
static inline GstFlowReturn gst_mtl_st40p_tx_prepare_test_frame(Gst_Mtl_St40p_Tx* sink,
                                                                GstMapInfo map_info,
                                                                GstBuffer* buf) {
  guint meta_count = sink->test_pkt_count;
  if (!meta_count) {
    switch (sink->test_mode) {
      case GST_MTL_ST40P_TX_TEST_MODE_SEQ_GAP:
        meta_count = 2;
        break;
      case GST_MTL_ST40P_TX_TEST_MODE_PACED:
        meta_count = 8;
        break;
      default:
        meta_count = 1;
        break;
    }
  }

  meta_count = MIN(meta_count, (guint)ST40_MAX_META);

  size_t max_udw = st40p_tx_max_udw_buff_size(sink->tx_handle);
  if (!max_udw) {
    GST_ERROR("Failed to query max UDW size for test frame");
    return GST_FLOW_ERROR;
  }

  guint per_udw = 4;
  if ((size_t)meta_count * per_udw > max_udw) {
    per_udw = max_udw / meta_count;
  }
  if (per_udw == 0) {
    GST_ERROR("Insufficient buffer for test frame (meta_count=%u)", meta_count);
    return GST_FLOW_ERROR;
  }

  struct st40_frame_info* frame_info = st40p_tx_get_frame(sink->tx_handle);
  if (!frame_info) {
    GST_ERROR("Failed to get frame for test mode");
    return GST_FLOW_ERROR;
  }

  guint total_bytes = per_udw * meta_count;
  memset(frame_info->udw_buff_addr, 0, total_bytes);
  guint copy_bytes = MIN(total_bytes, (guint)map_info.size);
  if (copy_bytes > 0) memcpy(frame_info->udw_buff_addr, map_info.data, copy_bytes);

  for (guint i = 0; i < meta_count; i++) {
    frame_info->meta[i].c = 0;
    frame_info->meta[i].line_number = 0;
    frame_info->meta[i].hori_offset = 0;
    frame_info->meta[i].s = 0;
    frame_info->meta[i].stream_num = 0;
    frame_info->meta[i].did = sink->did;
    frame_info->meta[i].sdid = sink->sdid;
    frame_info->meta[i].udw_size = per_udw;
    frame_info->meta[i].udw_offset = i * per_udw;
  }

  frame_info->meta_num = meta_count;
  frame_info->udw_buffer_fill = total_bytes;

  if (sink->use_pts_for_pacing) {
    frame_info->timestamp = GST_BUFFER_PTS(buf) + sink->pts_for_pacing_offset;
    frame_info->tfmt = ST10_TIMESTAMP_FMT_TAI;
  } else {
    frame_info->timestamp = 0;
  }

  if (st40p_tx_put_frame(sink->tx_handle, frame_info)) {
    GST_ERROR("Failed to put frame in test mode");
    return GST_FLOW_ERROR;
  }

  return GST_FLOW_OK;
}

/**
 * Render hook: return TRUE if a test-mode frame was built (caller should skip
 * normal rendering), FALSE otherwise.
 */
static inline gboolean gst_mtl_st40p_tx_test_render(Gst_Mtl_St40p_Tx* sink,
                                                    GstMapInfo map_info, GstBuffer* buf,
                                                    GstFlowReturn* ret) {
  if (sink->test_mode != GST_MTL_ST40P_TX_TEST_MODE_NONE &&
      sink->test_mode != GST_MTL_ST40P_TX_TEST_MODE_SEQ_GAP) {
    *ret = gst_mtl_st40p_tx_prepare_test_frame(sink, map_info, buf);
    return TRUE;
  }
  return FALSE;
}

/* ------------------------------------------------------------------ */
#else /* !MTL_SIMULATE_PACKET_DROPS */
/* ======================== RELEASE BUILD =========================== */

static inline void gst_mtl_st40p_tx_test_install_properties(GObjectClass* gobject_class
                                                            __attribute__((unused))) {
}
static inline void gst_mtl_st40p_tx_test_init(Gst_Mtl_St40p_Tx* sink
                                              __attribute__((unused))) {
}
static inline gboolean gst_mtl_st40p_tx_test_set_property(
    Gst_Mtl_St40p_Tx* self __attribute__((unused)), guint prop_id __attribute__((unused)),
    const GValue* value __attribute__((unused))) {
  return FALSE;
}
static inline gboolean gst_mtl_st40p_tx_test_get_property(
    Gst_Mtl_St40p_Tx* sink __attribute__((unused)), guint prop_id __attribute__((unused)),
    GValue* value __attribute__((unused))) {
  return FALSE;
}
static inline void gst_mtl_st40p_tx_test_configure_ops(Gst_Mtl_St40p_Tx* sink
                                                       __attribute__((unused)),
                                                       struct st40p_tx_ops* ops_tx
                                                       __attribute__((unused))) {
}
static inline gboolean gst_mtl_st40p_tx_test_render(
    Gst_Mtl_St40p_Tx* sink __attribute__((unused)),
    GstMapInfo map_info __attribute__((unused)), GstBuffer* buf __attribute__((unused)),
    GstFlowReturn* ret __attribute__((unused))) {
  return FALSE;
}

#endif /* MTL_SIMULATE_PACKET_DROPS */
/* ------------------------------------------------------------------ */

#endif /* __GST_MTL_ST40P_TX_TEST_H__ */
