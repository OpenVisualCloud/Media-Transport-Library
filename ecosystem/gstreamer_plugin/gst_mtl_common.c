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
guint gst_mtl_port_idx = MTL_PORT_P;

gboolean gst_mtl_common_parse_input_finfo(const GstVideoFormatInfo *finfo,
                                          enum st_frame_fmt *fmt) {
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
gboolean gst_mtl_common_parse_pixel_format(const char *format, enum st_frame_fmt *fmt) {
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

gboolean gst_mtl_common_parse_ptime(const char *ptime_str, enum st30_ptime *ptime) {
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

gboolean gst_mtl_common_parse_audio_format(const char *format, enum st30_fmt *audio) {
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
                                           enum st30_sampling *st_sampling) {
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
                                           gint *gst_sampling) {
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

void gst_mtl_common_init_general_arguments(GObjectClass *gobject_class) {
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
      gobject_class, PROP_GENERAL_DEV_ARGS_SIP,
      g_param_spec_string("dev-ip", "Local device IP",
                          "Local IP address that the port will be "
                          "identified by. This is the address from which ARP "
                          "responses will be sent.",
                          NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_GENERAL_DEV_ARGS_DMA_DEV,
      g_param_spec_string("dma-dev", "DPDK DMA port",
                          "DPDK port for the MTL direct memory functionality.", NULL,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_GENERAL_PORT_PORT,
      g_param_spec_string("port", "Transmission Device Port",
                          "DPDK device port initialized for the transmission.", NULL,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_GENERAL_PORT_IP,
      g_param_spec_string("ip", "Sender node's IP", "Receiving MTL node IP address.",
                          NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_GENERAL_PORT_UDP_PORT,
      g_param_spec_uint("udp-port", "Sender UDP port", "Receiving MTL node UDP port.", 0,
                        G_MAXUINT, 20000, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

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
}

void gst_mtl_common_set_general_arguments(GObject *object, guint prop_id,
                                          const GValue *value, GParamSpec *pspec,
                                          StDevArgs *devArgs, SessionPortArgs *portArgs,
                                          guint *log_level) {
  switch (prop_id) {
    case PROP_GENERAL_LOG_LEVEL:
      *log_level = g_value_get_uint(value);
      break;
    case PROP_GENERAL_DEV_ARGS_PORT:
      strncpy(devArgs->port, g_value_get_string(value), MTL_PORT_MAX_LEN);
      break;
    case PROP_GENERAL_DEV_ARGS_SIP:
      strncpy(devArgs->local_ip_string, g_value_get_string(value), MTL_PORT_MAX_LEN);
      break;
    case PROP_GENERAL_DEV_ARGS_DMA_DEV:
      strncpy(devArgs->dma_dev, g_value_get_string(value), MTL_PORT_MAX_LEN);
      break;
    case PROP_GENERAL_PORT_PORT:
      strncpy(portArgs->port, g_value_get_string(value), MTL_PORT_MAX_LEN);
      break;
    case PROP_GENERAL_PORT_IP:
      strncpy(portArgs->session_ip_string, g_value_get_string(value), MTL_PORT_MAX_LEN);
      break;
    case PROP_GENERAL_PORT_UDP_PORT:
      portArgs->udp_port = g_value_get_uint(value);
      break;
    case PROP_GENERAL_PORT_PAYLOAD_TYPE:
      portArgs->payload_type = g_value_get_uint(value);
      break;
    case PROP_GENERAL_PORT_RX_QUEUES:
      devArgs->rx_queues_cnt[MTL_PORT_P] = g_value_get_uint(value);
      break;
    case PROP_GENERAL_PORT_TX_QUEUES:
      devArgs->tx_queues_cnt[MTL_PORT_P] = g_value_get_uint(value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
}

void gst_mtl_common_get_general_arguments(GObject *object, guint prop_id,
                                          const GValue *value, GParamSpec *pspec,
                                          StDevArgs *devArgs, SessionPortArgs *portArgs,
                                          guint *log_level) {
  switch (prop_id) {
    case PROP_GENERAL_LOG_LEVEL:
      g_value_set_uint(value, *log_level);
      break;
    case PROP_GENERAL_DEV_ARGS_PORT:
      g_value_set_string(value, devArgs->port);
      break;
    case PROP_GENERAL_DEV_ARGS_SIP:
      g_value_set_string(value, devArgs->local_ip_string);
      break;
    case PROP_GENERAL_DEV_ARGS_DMA_DEV:
      g_value_set_string(value, devArgs->dma_dev);
      break;
    case PROP_GENERAL_PORT_PORT:
      g_value_set_string(value, portArgs->port);
      break;
    case PROP_GENERAL_PORT_IP:
      g_value_set_string(value, portArgs->session_ip_string);
      break;
    case PROP_GENERAL_PORT_UDP_PORT:
      g_value_set_uint(value, portArgs->udp_port);
      break;
    case PROP_GENERAL_PORT_PAYLOAD_TYPE:
      g_value_set_uint(value, portArgs->payload_type);
      break;
    case PROP_GENERAL_PORT_RX_QUEUES:
      g_value_set_uint(value, devArgs->rx_queues_cnt[MTL_PORT_P]);
      break;
    case PROP_GENERAL_PORT_TX_QUEUES:
      g_value_set_uint(value, devArgs->tx_queues_cnt[MTL_PORT_P]);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
}

gboolean gst_mtl_common_parse_dev_arguments(struct mtl_init_params *mtl_init_params,
                                            StDevArgs *devArgs) {
  gint ret;

  if (gst_mtl_port_idx > MTL_PORT_R) {
    GST_ERROR("%s, invalid port number %d\n", __func__, gst_mtl_port_idx);
    return FALSE;
  }

  strncpy(mtl_init_params->port[gst_mtl_port_idx], devArgs->port, MTL_PORT_MAX_LEN);

  ret = inet_pton(AF_INET, devArgs->local_ip_string,
                  mtl_init_params->sip_addr[gst_mtl_port_idx]);
  if (ret != 1) {
    GST_ERROR("%s, sip %s is not valid ip address\n", __func__, devArgs->local_ip_string);
    return FALSE;
  }

  if (devArgs->rx_queues_cnt[gst_mtl_port_idx]) {
    mtl_init_params->rx_queues_cnt[gst_mtl_port_idx] =
        devArgs->rx_queues_cnt[gst_mtl_port_idx];
  } else {
    mtl_init_params->rx_queues_cnt[gst_mtl_port_idx] = 16;
  }

  if (devArgs->tx_queues_cnt[gst_mtl_port_idx]) {
    mtl_init_params->tx_queues_cnt[gst_mtl_port_idx] =
        devArgs->tx_queues_cnt[gst_mtl_port_idx];
  } else {
    mtl_init_params->tx_queues_cnt[gst_mtl_port_idx] = 16;
  }

  mtl_init_params->num_ports++;

  if (devArgs->dma_dev && strlen(devArgs->dma_dev)) {
    strncpy(mtl_init_params->dma_dev_port[0], devArgs->dma_dev, MTL_PORT_MAX_LEN);
  }

  gst_mtl_port_idx++;
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
 * @param devArgs Initialization parameters for the DPDK port
 *                (ignored if using an existing MTL instance).
 * @param log_level Log level for the library (ignored if using an
 *                  existing MTL instance).
 */
mtl_handle gst_mtl_common_init_handle(StDevArgs *devArgs, guint *log_level,
                                      gboolean force_to_initialize_new_instance) {
  struct mtl_init_params mtl_init_params = {0};
  mtl_handle ret;
  pthread_mutex_lock(&common_handle.mutex);

  if (!force_to_initialize_new_instance && common_handle.mtl_handle) {
    GST_INFO("Mtl is already initialized with shared handle %p",
             common_handle.mtl_handle);
    common_handle.mtl_handle_reference_count++;

    pthread_mutex_unlock(&common_handle.mutex);
    return common_handle.mtl_handle;
  }

  if (!devArgs || !log_level) {
    GST_ERROR("Invalid input");
    pthread_mutex_unlock(&common_handle.mutex);
    return NULL;
  }

  mtl_init_params.num_ports = 0;

  if (gst_mtl_common_parse_dev_arguments(&mtl_init_params, devArgs) == FALSE) {
    GST_ERROR("Failed to parse dev arguments");
    pthread_mutex_unlock(&common_handle.mutex);
    return NULL;
  }
  mtl_init_params.flags |= MTL_FLAG_BIND_NUMA;

  /*
   * Log levels range from 1 to LOG_LEVEL_MAX.
   * We avoid using 0 (DEBUG) in normal scenarios,
   * so it's acceptable to use 0 as a placeholder.
   */
  if (*log_level && *log_level < MTL_LOG_LEVEL_MAX) {
    mtl_init_params.log_level = *log_level;
  } else {
    mtl_init_params.log_level = MTL_LOG_LEVEL_INFO;
  }
  *log_level = mtl_init_params.log_level;

  if (force_to_initialize_new_instance) {
    GST_INFO("MTL shared handle ignored");

    ret = mtl_init(&mtl_init_params);
    pthread_mutex_unlock(&common_handle.mutex);

    return ret;
  }

  common_handle.mtl_handle = mtl_init(&mtl_init_params);
  common_handle.mtl_handle_reference_count++;
  pthread_mutex_unlock(&common_handle.mutex);

  return common_handle.mtl_handle;
}

/**
 * Deinitializes the MTL handle.
 * If the handle is the shared handle, the reference count is decremented.
 * If the reference count reaches zero, the handle is deinitialized.
 * If the handle is not the shared handle, it is deinitialized immediately.
 *
 * @param handle MTL handle to deinitialize (Null is an akceptable value then
 * shared value will be used).
 */
gint gst_mtl_common_deinit_handle(mtl_handle handle) {
  gint ret;

  pthread_mutex_lock(&common_handle.mutex);

  if (handle && handle != common_handle.mtl_handle) {
    ret = mtl_uninit(handle);
    pthread_mutex_unlock(&common_handle.mutex);
    return ret;
  }

  common_handle.mtl_handle_reference_count--;

  if (common_handle.mtl_handle_reference_count > 0) {
    common_handle.mtl_handle_reference_count--;

    pthread_mutex_unlock(&common_handle.mutex);
    return 0;
  }
  ret = mtl_uninit(common_handle.mtl_handle);

  pthread_mutex_unlock(&common_handle.mutex);
  pthread_mutex_destroy(&common_handle.mutex);
  return ret;
}
