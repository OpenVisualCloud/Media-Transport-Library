/*
 * Copyright (C) 2024 Intel Corporation
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "gst_mtl_common.h"

/* Shared handle for the MTL library used across plugins in the pipeline */
struct gst_common_handle {
  mtl_handle mtl_handle;
  int mtl_handle_reference_count;
  pthread_mutex_t mutex;
};

static struct gst_common_handle common_handle = {0, 0, PTHREAD_MUTEX_INITIALIZER};

gboolean gst_mtl_common_parse_input_finfo(const GstVideoFormatInfo* finfo,
                                          enum st_frame_fmt* fmt) {
  if (finfo->format == GST_VIDEO_FORMAT_v210) {
    *fmt = ST_FRAME_FMT_V210;
  } else if (finfo->format == GST_VIDEO_FORMAT_I422_10LE) {
    *fmt = ST_FRAME_FMT_YUV422PLANAR10LE;
  } else {
    return FALSE;
  }

  return TRUE;
}

/* includes all formats supported by the library for future support */
gboolean gst_mtl_common_parse_pixel_format(const char* format, enum st_frame_fmt* fmt) {
  if (!fmt || !format) {
    GST_ERROR("%s, invalid input\n", __func__);
    return FALSE;
  }

  if (strcmp(format, "YUV422PLANAR10LE") == 0) {
    *fmt = ST_FRAME_FMT_YUV422PLANAR10LE;
  } else if (strcmp(format, "v210") == 0) {
    *fmt = ST_FRAME_FMT_V210;
  } else if (strcmp(format, "Y210") == 0) {
    *fmt = ST_FRAME_FMT_Y210;
  } else if (strcmp(format, "YUV422PLANAR8") == 0) {
    *fmt = ST_FRAME_FMT_YUV422PLANAR8;
  } else if (strcmp(format, "UYVY") == 0) {
    *fmt = ST_FRAME_FMT_UYVY;
  } else if (strcmp(format, "YUV422RFC4175PG2BE10") == 0) {
    *fmt = ST_FRAME_FMT_YUV422RFC4175PG2BE10;
  } else if (strcmp(format, "YUV422PLANAR12LE") == 0) {
    *fmt = ST_FRAME_FMT_YUV422PLANAR12LE;
  } else if (strcmp(format, "YUV422RFC4175PG2BE12") == 0) {
    *fmt = ST_FRAME_FMT_YUV422RFC4175PG2BE12;
  } else if (strcmp(format, "YUV444PLANAR10LE") == 0) {
    *fmt = ST_FRAME_FMT_YUV444PLANAR10LE;
  } else if (strcmp(format, "YUV444RFC4175PG4BE10") == 0) {
    *fmt = ST_FRAME_FMT_YUV444RFC4175PG4BE10;
  } else if (strcmp(format, "YUV444PLANAR12LE") == 0) {
    *fmt = ST_FRAME_FMT_YUV444PLANAR12LE;
  } else if (strcmp(format, "YUV444RFC4175PG2BE12") == 0) {
    *fmt = ST_FRAME_FMT_YUV444RFC4175PG2BE12;
  } else if (strcmp(format, "YUV420CUSTOM8") == 0) {
    *fmt = ST_FRAME_FMT_YUV420CUSTOM8;
  } else if (strcmp(format, "YUV422CUSTOM8") == 0) {
    *fmt = ST_FRAME_FMT_YUV422CUSTOM8;
  } else if (strcmp(format, "YUV420PLANAR8") == 0) {
    *fmt = ST_FRAME_FMT_YUV420PLANAR8;
  } else if (strcmp(format, "ARGB") == 0) {
    *fmt = ST_FRAME_FMT_ARGB;
  } else if (strcmp(format, "BGRA") == 0) {
    *fmt = ST_FRAME_FMT_BGRA;
  } else if (strcmp(format, "RGB8") == 0) {
    *fmt = ST_FRAME_FMT_RGB8;
  } else if (strcmp(format, "GBRPLANAR10LE") == 0) {
    *fmt = ST_FRAME_FMT_GBRPLANAR10LE;
  } else if (strcmp(format, "RGBRFC4175PG4BE10") == 0) {
    *fmt = ST_FRAME_FMT_RGBRFC4175PG4BE10;
  } else if (strcmp(format, "GBRPLANAR12LE") == 0) {
    *fmt = ST_FRAME_FMT_GBRPLANAR12LE;
  } else if (strcmp(format, "RGBRFC4175PG2BE12") == 0) {
    *fmt = ST_FRAME_FMT_RGBRFC4175PG2BE12;
  } else {
    GST_ERROR("invalid output format %s\n", format);
    return FALSE;
  }

  return TRUE;
}

gboolean gst_mtl_common_parse_ptime(const char* ptime_str, enum st30_ptime* ptime) {
  if (!ptime_str || !ptime) {
    GST_ERROR("%s, invalid input\n", __func__);
    return FALSE;
  }

  if (strcmp(ptime_str, "1ms") == 0) {
    *ptime = ST30_PTIME_1MS;
  } else if (strcmp(ptime_str, "125us") == 0) {
    *ptime = ST30_PTIME_125US;
  } else if (strcmp(ptime_str, "250us") == 0) {
    *ptime = ST30_PTIME_250US;
  } else if (strcmp(ptime_str, "333us") == 0) {
    *ptime = ST30_PTIME_333US;
  } else if (strcmp(ptime_str, "4ms") == 0) {
    *ptime = ST30_PTIME_4MS;
  } else if (strcmp(ptime_str, "80us") == 0) {
    *ptime = ST31_PTIME_80US;
  } else if (strcmp(ptime_str, "1.09ms") == 0) {
    *ptime = ST31_PTIME_1_09MS;
  } else if (strcmp(ptime_str, "0.14ms") == 0) {
    *ptime = ST31_PTIME_0_14MS;
  } else if (strcmp(ptime_str, "0.09ms") == 0) {
    *ptime = ST31_PTIME_0_09MS;
  } else {
    GST_ERROR("invalid packet time %s\n", ptime_str);
    return FALSE;
  }

  return TRUE;
}

gboolean gst_mtl_common_parse_audio_format(const char* format, enum st30_fmt* audio) {
  if (!audio || !format) {
    GST_ERROR("%s, invalid input\n", __func__);
    return FALSE;
  }

  if (strcmp(format, "PCM8") == 0) {
    *audio = ST30_FMT_PCM8;
  } else if (strcmp(format, "PCM16") == 0) {
    *audio = ST30_FMT_PCM16;
  } else if (strcmp(format, "PCM24") == 0) {
    *audio = ST30_FMT_PCM24;
  } else if (strcmp(format, "AM824") == 0) {
    *audio = ST31_FMT_AM824;
  } else {
    GST_ERROR("%s, invalid audio format %s\n", __func__, format);
    return FALSE;
  }

  return TRUE;
}

gboolean gst_mtl_common_gst_to_st_sampling(gint sampling,
                                           enum st30_sampling* st_sampling) {
  if (!st_sampling) {
    GST_ERROR("Invalid st_sampling pointer");
    return FALSE;
  }

  switch (sampling) {
    case GST_MTL_SUPPORTED_AUDIO_SAMPLING_44_1K:
      *st_sampling = ST31_SAMPLING_44K;
      return TRUE;
    case GST_MTL_SUPPORTED_AUDIO_SAMPLING_48K:
      *st_sampling = ST30_SAMPLING_48K;
      return TRUE;
    case GST_MTL_SUPPORTED_AUDIO_SAMPLING_96K:
      *st_sampling = ST30_SAMPLING_96K;
      return TRUE;
    default:
      GST_ERROR("Unsupported sampling value");
      return FALSE;
  }
}

gboolean gst_mtl_common_st_to_gst_sampling(enum st30_sampling st_sampling,
                                           gint* gst_sampling) {
  if (!gst_sampling) {
    GST_ERROR("Invalid gst_sampling pointer");
    return FALSE;
  }

  switch (st_sampling) {
    case ST31_SAMPLING_44K:
      *gst_sampling = GST_MTL_SUPPORTED_AUDIO_SAMPLING_44_1K;
      return TRUE;
    case ST30_SAMPLING_48K:
      *gst_sampling = GST_MTL_SUPPORTED_AUDIO_SAMPLING_48K;
      return TRUE;
    case ST30_SAMPLING_96K:
      *gst_sampling = GST_MTL_SUPPORTED_AUDIO_SAMPLING_96K;
      return TRUE;
    default:
      GST_ERROR("Unsupported st_sampling value");
      return FALSE;
  }
}

void gst_mtl_common_init_general_arguments(GObjectClass* gobject_class) {
  g_object_class_install_property(
      gobject_class, PROP_GENERAL_LOG_LEVEL,
      g_param_spec_uint("log-level", "Log Level", "Set the log level (INFO 1 to CRIT 5).",
                        1, MTL_LOG_LEVEL_MAX, 1,
                        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_GENERAL_DEV_ARGS_PORT,
      g_param_spec_string("dev-port", "DPDK device port",
                          "DPDK port for synchronous ST 2110 data"
                          "video transmission, bound to the VFIO DPDK driver. ",
                          NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_GENERAL_DEV_ARGS_PORT_R,
      g_param_spec_string("dev-port-red", "DPDK device port redundant",
                          "DPDK redundant port for synchronous ST 2110 data"
                          "video transmission, bound to the VFIO DPDK driver. ",
                          NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_GENERAL_DEV_ARGS_SIP,
      g_param_spec_string("dev-ip", "Local device IP",
                          "Local IP address that the port will be "
                          "identified by. This is the address from which ARP "
                          "responses will be sent.",
                          NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_GENERAL_DEV_ARGS_SIP_R,
      g_param_spec_string("dev-ip-red", "Local redundant device IP",
                          "Local IP address redundant that the port will be "
                          "identified by. This is the address from which ARP "
                          "responses will be sent.",
                          NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_GENERAL_DEV_ARGS_DMA_DEV,
      g_param_spec_string("dma-dev", "DPDK DMA port",
                          "DPDK port for the MTL direct memory functionality.", NULL,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_GENERAL_SESSION_PORT,
      g_param_spec_string("port", "Transmission Device Port",
                          "DPDK device for the session to use.", NULL,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_GENERAL_SESSION_PORT_R,
      g_param_spec_string("port-red", "Transmission Device Port",
                          "DPDK device for the session to use as redundant port.", NULL,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_GENERAL_PORT_IP,
      g_param_spec_string("ip", "Sender node's IP", "Receiving MTL node IP address.",
                          NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_GENERAL_PORT_IP_R,
      g_param_spec_string("ip-red", "Sender node's IP", "Receiving MTL node IP address.",
                          NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_GENERAL_PORT_UDP_PORT,
      g_param_spec_uint("udp-port", "Sender UDP port", "Receiving MTL node UDP port.", 0,
                        G_MAXUINT, 20000, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_GENERAL_PORT_UDP_PORT_R,
      g_param_spec_uint("udp-port-red", "Sender UDP port", "Receiving MTL node UDP port.",
                        0, G_MAXUINT, 20000, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_GENERAL_PORT_TX_QUEUES,
      g_param_spec_uint("tx-queues", "Number of TX queues",
                        "Number of TX queues to initialize in DPDK backend.", 0,
                        G_MAXUINT, 16, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_GENERAL_PORT_RX_QUEUES,
      g_param_spec_uint("rx-queues", "Number of RX queues",
                        "Number of RX queues to initialize in DPDK backend.", 0,
                        G_MAXUINT, 16, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_GENERAL_PORT_PAYLOAD_TYPE,
      g_param_spec_uint("payload-type", "ST 2110 payload type",
                        "SMPTE ST 2110 payload type.", 0, G_MAXUINT, 112,
                        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_GENERAL_ENABLE_ONBOARD_PTP,
      g_param_spec_boolean("enable-ptp", "Enable onboard PTP",
                           "Enable onboard PTP client", FALSE,
                           G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_GENERAL_ENABLE_DMA_OFFLOAD,
      g_param_spec_boolean("enable-dma-offload", "Enable DMA offload",
                           "Request DMA offload for compatible sessions.", FALSE,
                           G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_GENERAL_DEV_ARGS_ALLOW_DOWN_P,
      g_param_spec_boolean("allow-port-down", "Allow primary port down",
                           "Allow MTL to initialize even if the primary port link is down.",
                           FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_GENERAL_DEV_ARGS_ALLOW_DOWN_R,
      g_param_spec_boolean(
          "allow-port-down-red", "Allow redundant port down",
          "Allow MTL to initialize even if the redundant port link is down.", FALSE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
}

void gst_mtl_common_set_general_arguments(GObject* object, guint prop_id,
                                          const GValue* value, GParamSpec* pspec,
                                          GeneralArgs* general_args,
                                          SessionPortArgs* portArgs) {
  switch (prop_id) {
    case PROP_GENERAL_LOG_LEVEL:
      general_args->log_level = g_value_get_uint(value);
      break;
    case PROP_GENERAL_DEV_ARGS_PORT:
      strncpy(general_args->port[MTL_PORT_P], g_value_get_string(value),
              MTL_PORT_MAX_LEN);
      break;
    case PROP_GENERAL_DEV_ARGS_PORT_R:
      strncpy(general_args->port[MTL_PORT_R], g_value_get_string(value),
              MTL_PORT_MAX_LEN);
      break;
    case PROP_GENERAL_DEV_ARGS_SIP:
      strncpy(general_args->local_ip_string[MTL_PORT_P], g_value_get_string(value),
              MTL_PORT_MAX_LEN);
      break;
    case PROP_GENERAL_DEV_ARGS_SIP_R:
      strncpy(general_args->local_ip_string[MTL_PORT_R], g_value_get_string(value),
              MTL_PORT_MAX_LEN);
      break;
    case PROP_GENERAL_DEV_ARGS_DMA_DEV:
      strncpy(general_args->dma_dev, g_value_get_string(value), MTL_PORT_MAX_LEN);
      break;
    case PROP_GENERAL_SESSION_PORT:
      strncpy(portArgs->port[MTL_PORT_P], g_value_get_string(value), MTL_PORT_MAX_LEN);
      break;
    case PROP_GENERAL_SESSION_PORT_R:
      strncpy(portArgs->port[MTL_PORT_R], g_value_get_string(value), MTL_PORT_MAX_LEN);
      break;
    case PROP_GENERAL_PORT_IP:
      strncpy(portArgs->session_ip_string[MTL_PORT_P], g_value_get_string(value),
              MTL_PORT_MAX_LEN);
      break;
    case PROP_GENERAL_PORT_IP_R:
      strncpy(portArgs->session_ip_string[MTL_PORT_R], g_value_get_string(value),
              MTL_PORT_MAX_LEN);
      break;
    case PROP_GENERAL_PORT_UDP_PORT:
      portArgs->udp_port[MTL_PORT_P] = g_value_get_uint(value);
      break;
    case PROP_GENERAL_PORT_UDP_PORT_R:
      portArgs->udp_port[MTL_PORT_R] = g_value_get_uint(value);
      break;
    case PROP_GENERAL_PORT_PAYLOAD_TYPE:
      portArgs->payload_type = g_value_get_uint(value);
      break;
    case PROP_GENERAL_PORT_RX_QUEUES:
      general_args->rx_queues_cnt[MTL_PORT_P] = g_value_get_uint(value);
      general_args->rx_queues_cnt[MTL_PORT_R] = g_value_get_uint(value);
      break;
    case PROP_GENERAL_PORT_TX_QUEUES:
      general_args->tx_queues_cnt[MTL_PORT_P] = g_value_get_uint(value);
      general_args->tx_queues_cnt[MTL_PORT_R] = g_value_get_uint(value);
      break;
    case PROP_GENERAL_ENABLE_ONBOARD_PTP:
      general_args->enable_onboard_ptp = g_value_get_boolean(value);
      break;
    case PROP_GENERAL_ENABLE_DMA_OFFLOAD:
      general_args->enable_dma_offload = g_value_get_boolean(value);
      break;
    case PROP_GENERAL_DEV_ARGS_ALLOW_DOWN_P:
      general_args->allow_port_down[MTL_PORT_P] = g_value_get_boolean(value);
      break;
    case PROP_GENERAL_DEV_ARGS_ALLOW_DOWN_R:
      general_args->allow_port_down[MTL_PORT_R] = g_value_get_boolean(value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
}

void gst_mtl_common_get_general_arguments(GObject* object, guint prop_id,
                                          const GValue* value, GParamSpec* pspec,
                                          GeneralArgs* general_args,
                                          SessionPortArgs* portArgs) {
  switch (prop_id) {
    case PROP_GENERAL_LOG_LEVEL:
      g_value_set_uint(value, general_args->log_level);
      break;
    case PROP_GENERAL_DEV_ARGS_PORT:
      g_value_set_string(value, general_args->port[MTL_PORT_P]);
      break;
    case PROP_GENERAL_DEV_ARGS_PORT_R:
      g_value_set_string(value, general_args->port[MTL_PORT_R]);
      break;
    case PROP_GENERAL_DEV_ARGS_SIP:
      g_value_set_string(value, general_args->local_ip_string[MTL_PORT_P]);
      break;
    case PROP_GENERAL_DEV_ARGS_SIP_R:
      g_value_set_string(value, general_args->local_ip_string[MTL_PORT_R]);
      break;
    case PROP_GENERAL_DEV_ARGS_DMA_DEV:
      g_value_set_string(value, general_args->dma_dev);
      break;
    case PROP_GENERAL_SESSION_PORT:
      g_value_set_string(value, portArgs->port[MTL_PORT_P]);
      break;
    case PROP_GENERAL_SESSION_PORT_R:
      g_value_set_string(value, portArgs->port[MTL_PORT_R]);
      break;
    case PROP_GENERAL_PORT_IP:
      g_value_set_string(value, portArgs->session_ip_string[MTL_PORT_P]);
      break;
    case PROP_GENERAL_PORT_IP_R:
      g_value_set_string(value, portArgs->session_ip_string[MTL_PORT_R]);
      break;
    case PROP_GENERAL_PORT_UDP_PORT:
      g_value_set_uint(value, portArgs->udp_port[MTL_PORT_P]);
      break;
    case PROP_GENERAL_PORT_UDP_PORT_R:
      g_value_set_uint(value, portArgs->udp_port[MTL_PORT_R]);
      break;
    case PROP_GENERAL_PORT_PAYLOAD_TYPE:
      g_value_set_uint(value, portArgs->payload_type);
      break;
    case PROP_GENERAL_PORT_RX_QUEUES:
      g_value_set_uint(value, general_args->rx_queues_cnt[MTL_PORT_P]);
      break;
    case PROP_GENERAL_PORT_TX_QUEUES:
      g_value_set_uint(value, general_args->tx_queues_cnt[MTL_PORT_P]);
      break;
    case PROP_GENERAL_ENABLE_ONBOARD_PTP:
      g_value_set_boolean(value, general_args->enable_onboard_ptp);
      break;
    case PROP_GENERAL_ENABLE_DMA_OFFLOAD:
      g_value_set_boolean(value, general_args->enable_dma_offload);
      break;
    case PROP_GENERAL_DEV_ARGS_ALLOW_DOWN_P:
      g_value_set_boolean(value, general_args->allow_port_down[MTL_PORT_P]);
      break;
    case PROP_GENERAL_DEV_ARGS_ALLOW_DOWN_R:
      g_value_set_boolean(value, general_args->allow_port_down[MTL_PORT_R]);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
}

/**
 * Copies general initialization port values to session-specific port arguments
 * if not specified. If the primary port is not specified, the redundant port
 * argument will be copied from the general initialization ports regardless of
 * its specification. If the UDP port for the redundant port is not specified,
 * it will be incremented by one over the primary port.
 *
 * @param general_args Pointer to the structure containing general
 * initialization parameters.
 * @param port_args Pointer to the structure containing session-specific port
 * arguments.
 */
void gst_mtl_common_copy_general_to_session_args(GeneralArgs* general_args,
                                                 SessionPortArgs* port_args) {
  gboolean redundant = (strlen(general_args->port[MTL_PORT_R]) > 0);

  if (strlen(port_args->port[MTL_PORT_P]) == 0) {
    strncpy(port_args->port[MTL_PORT_P], general_args->port[MTL_PORT_P],
            MTL_PORT_MAX_LEN);

    if (redundant && strlen(port_args->port[MTL_PORT_R]) == 0) {
      strncpy(port_args->port[MTL_PORT_R], general_args->port[MTL_PORT_R],
              MTL_PORT_MAX_LEN);
    }
  }

  if (redundant && port_args->udp_port[MTL_PORT_R] == 0) {
    port_args->udp_port[MTL_PORT_R] = port_args->udp_port[MTL_PORT_P] + 1;
  }
}

/**
 * Parses the transmission port arguments and initializes the tranmission port
 * structure. Validates and sets the destination IP address, port number, UDP
 * port, and payload type.
 *
 * @param port Pointer to the transmission port structure to be initialized.
 * @param port_args Pointer to the structure containing the port arguments.
 * @return The number of initialized ports, or 0 if an error occurred.
 */
guint gst_mtl_common_parse_tx_port_arguments(struct st_tx_port* port,
                                             SessionPortArgs* port_args) {
  guint mtl_port_idx = MTL_PORT_P;

  while (mtl_port_idx <= MTL_PORT_R && strlen(port_args->port[mtl_port_idx]) > 0) {
    if (inet_pton(AF_INET, port_args->session_ip_string[mtl_port_idx],
                  port->dip_addr[mtl_port_idx]) != 1) {
      GST_ERROR("Invalid destination IP address: \"%s\" on port %u",
                port_args->session_ip_string[mtl_port_idx], mtl_port_idx);
      return 0;
    }

    if (strlen(port_args->port[mtl_port_idx]) == 0) {
      GST_ERROR("Invalid port number %u", mtl_port_idx);
      return 0;
    }

    strncpy(port->port[mtl_port_idx], port_args->port[mtl_port_idx], MTL_PORT_MAX_LEN);

    if ((port_args->udp_port[mtl_port_idx] < 0) ||
        (port_args->udp_port[mtl_port_idx] > 0xFFFF)) {
      GST_ERROR("%s, invalid UDP port: %d\n", __func__,
                port_args->udp_port[mtl_port_idx]);
      return 0;
    }

    port->udp_port[mtl_port_idx] = port_args->udp_port[mtl_port_idx];
    mtl_port_idx++;
  }

  if ((port_args->payload_type < 0) || (port_args->payload_type > 0x7F)) {
    GST_ERROR("%s, invalid payload_type: %d\n", __func__, port_args->payload_type);
    return 0;
  }

  port->payload_type = port_args->payload_type;

  return mtl_port_idx;
}

/**
 * Parses the transmission port arguments and initializes the receive port
 * structure. Validates and sets the destination IP address, port number, UDP
 * port, and payload type.
 *
 * @param port Pointer to the transmission port structure to be initialized.
 * @param port_args Pointer to the structure containing the port arguments.
 * @return The number of initialized ports, or 0 if an error occurred.
 */
guint gst_mtl_common_parse_rx_port_arguments(struct st_rx_port* port,
                                             SessionPortArgs* port_args) {
  guint mtl_port_idx = MTL_PORT_P;

  while (mtl_port_idx <= MTL_PORT_R && strlen(port_args->port[mtl_port_idx])) {
    if (inet_pton(AF_INET, port_args->session_ip_string[mtl_port_idx],
                  port->ip_addr[mtl_port_idx]) != 1) {
      GST_ERROR("Invalid source IP address: %s",
                port_args->session_ip_string[mtl_port_idx]);
      return 0;
    }

    strncpy(port->port[mtl_port_idx], port_args->port[mtl_port_idx], MTL_PORT_MAX_LEN);

    if ((port_args->udp_port[mtl_port_idx] < 0) ||
        (port_args->udp_port[mtl_port_idx] > 0xFFFF)) {
      GST_ERROR("%s, invalid UDP port: %d\n", __func__,
                port_args->udp_port[mtl_port_idx]);
      return 0;
    }

    port->udp_port[mtl_port_idx] = port_args->udp_port[mtl_port_idx];
    mtl_port_idx++;
  }

  /* check primary port */
  if (strlen(port_args->port[MTL_PORT_P]) == 0) {
    GST_ERROR("Invalid port number %u", mtl_port_idx);
    return 0;
  }

  if ((port_args->payload_type < 0) || (port_args->payload_type > 0x7F)) {
    GST_ERROR("%s, invalid payload_type: %d\n", __func__, port_args->payload_type);
    return 0;
  }

  port->payload_type = port_args->payload_type;

  return mtl_port_idx;
}

gboolean gst_mtl_common_parse_general_arguments(struct mtl_init_params* mtl_init_params,
                                                GeneralArgs* general_args) {
  gint mtl_port_idx = MTL_PORT_P;
  gint ret;

  /*
   * Log levels range from 1 to LOG_LEVEL_MAX.
   * We avoid using 0 (DEBUG) in normal scenarios,
   * so it's acceptable to use 0 as a placeholder.
   */
  if (general_args->log_level && general_args->log_level < MTL_LOG_LEVEL_MAX) {
    mtl_init_params->log_level = general_args->log_level;
  } else {
    mtl_init_params->log_level = MTL_LOG_LEVEL_INFO;
  }

  general_args->log_level = mtl_init_params->log_level;

  if (general_args->enable_onboard_ptp) {
    mtl_init_params->flags |= MTL_FLAG_PTP_ENABLE;
    GST_INFO("Using MTL library's onboard PTP");
  }

  while (mtl_port_idx <= MTL_PORT_R && strlen(general_args->port[mtl_port_idx]) != 0) {
    strncpy(mtl_init_params->port[mtl_port_idx], general_args->port[mtl_port_idx],
            MTL_PORT_MAX_LEN);

    ret = inet_pton(AF_INET, general_args->local_ip_string[mtl_port_idx],
                    mtl_init_params->sip_addr[mtl_port_idx]);
    if (ret != 1) {
      GST_ERROR("%s, sip %s is not valid ip address\n", __func__,
                general_args->local_ip_string[MTL_PORT_P]);
      return FALSE;
    }

    if (general_args->rx_queues_cnt[mtl_port_idx]) {
      mtl_init_params->rx_queues_cnt[mtl_port_idx] =
          general_args->rx_queues_cnt[mtl_port_idx];
    } else {
      mtl_init_params->rx_queues_cnt[mtl_port_idx] = 16;
    }

    if (general_args->tx_queues_cnt[mtl_port_idx]) {
      mtl_init_params->tx_queues_cnt[mtl_port_idx] =
          general_args->tx_queues_cnt[mtl_port_idx];
    } else {
      mtl_init_params->tx_queues_cnt[mtl_port_idx] = 16;
    }

    if (general_args->allow_port_down[mtl_port_idx]) {
      mtl_init_params->port_params[mtl_port_idx].flags |=
          MTL_PORT_FLAG_ALLOW_DOWN_INITIALIZATION;
      GST_INFO("Port %d: allow-port-down enabled", mtl_port_idx);
    }

    mtl_init_params->num_ports++;

    mtl_port_idx++;
  }

  if (general_args->dma_dev && strlen(general_args->dma_dev)) {
    gchar** dma_tokens = g_strsplit(general_args->dma_dev, ",", MTL_DMA_DEV_MAX + 1);
    gint idx = 0;

    while (dma_tokens && dma_tokens[idx] && idx < MTL_DMA_DEV_MAX) {
      gchar* token = g_strstrip(dma_tokens[idx]);
      if (token && strlen(token)) {
        strncpy(mtl_init_params->dma_dev_port[mtl_init_params->num_dma_dev_port], token,
                MTL_PORT_MAX_LEN);
        mtl_init_params->num_dma_dev_port++;
      }
      idx++;
    }

    if (dma_tokens) g_strfreev(dma_tokens);
  }

  return ret;
}

/**
 * Initializes the device with the given parameters.
 *
 * If the common handle (MTL instance already initialized in the pipeline)
 * is already in use, the input parameters for the device
 * (rx_queues, tx_queues, dev_ip, dev_port, and log_level) will be ignored.
 * You can force to initialize another MTL instance to avoid this behavior with
 * force_to_initialize_new_instance flag.
 *
 * @param force_to_initialize_new_instance Force the creation of a new MTL
 *                                         instance, ignoring any existing one.
 * @param general_args General initialization parameters for mtl handle
 *                (ignored if using an existing MTL instance).
 */
mtl_handle gst_mtl_common_init_handle(GeneralArgs* general_args,
                                      gboolean force_to_initialize_new_instance) {
  struct mtl_init_params mtl_init_params = {0};
  mtl_handle handle;
  gint ret;

  pthread_mutex_lock(&common_handle.mutex);

  if (!force_to_initialize_new_instance && common_handle.mtl_handle) {
    GST_INFO("Mtl is already initialized with shared handle %p",
             common_handle.mtl_handle);
    common_handle.mtl_handle_reference_count++;

    pthread_mutex_unlock(&common_handle.mutex);
    return common_handle.mtl_handle;
  }

  if (!general_args) {
    GST_ERROR("Invalid input");
    pthread_mutex_unlock(&common_handle.mutex);
    return NULL;
  }

  ret = gst_mtl_common_parse_general_arguments(&mtl_init_params, general_args);
  if (!ret) {
    GST_ERROR("Failed to parse dev arguments");
    pthread_mutex_unlock(&common_handle.mutex);
    return NULL;
  }

  handle = mtl_init(&mtl_init_params);
  if (!handle) {
    GST_ERROR("Failed to initialize MTL library");
    pthread_mutex_unlock(&common_handle.mutex);
    return NULL;
  }

  ret = mtl_start(handle);
  if (ret) {
    GST_ERROR("Failed to start MTL library");
    pthread_mutex_unlock(&common_handle.mutex);
    return NULL;
  }

  if (force_to_initialize_new_instance) {
    GST_INFO("Forced initialization: Bypassing MTL shared handle functionality");
    pthread_mutex_unlock(&common_handle.mutex);
    return handle;
  }

  common_handle.mtl_handle_reference_count++;
  GST_INFO("MTL shared handle reference count incremented to: %d",
           common_handle.mtl_handle_reference_count);
  common_handle.mtl_handle = handle;
  pthread_mutex_unlock(&common_handle.mutex);
  return common_handle.mtl_handle;
}

/**
 * Deinitialize the MTL handle.
 * If the handle is the shared handle, the reference count is decremented.
 * If the reference count reaches zero, the handle is deinitialized.
 * If the handle is not the shared handle, it is deinitialized immediately.
 */
gint gst_mtl_common_deinit_handle(mtl_handle* handle) {
  gint ret;

  if (!handle) {
    GST_ERROR("Invalid handle");
    return -EINVAL;
  }

  pthread_mutex_lock(&common_handle.mutex);

  if (*handle == common_handle.mtl_handle) {
    common_handle.mtl_handle_reference_count--;

    if (common_handle.mtl_handle_reference_count > 0) {
      GST_INFO("Shared handle is still in use, reference count: %d",
               common_handle.mtl_handle_reference_count);
      pthread_mutex_unlock(&common_handle.mutex);
      return 0;
    } else if (common_handle.mtl_handle_reference_count < 0) {
      GST_ERROR("Invalid reference count: %d", common_handle.mtl_handle_reference_count);
      pthread_mutex_unlock(&common_handle.mutex);
      return -EINVAL;
    }

    GST_INFO("Deinitializing shared handle");
  }

  ret = mtl_stop(*handle);
  if (ret) {
    GST_ERROR("Failed to stop MTL library");
    pthread_mutex_unlock(&common_handle.mutex);
    return ret;
  }

  ret = mtl_uninit(*handle);
  *handle = NULL;
  pthread_mutex_unlock(&common_handle.mutex);
  return ret;
}
