/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "linux-mtl.h"

#if TODO_OUTPUT

#define MTL_TX_SESSION(voidptr) struct mtl_tx_session* s = voidptr;

/**
 * Data structure for the mtl source
 */
struct mtl_tx_session {
  /* settings */
  char* lcores;
  char* port;
  char* sip;
  char* ip;
  uint16_t udp_port;
  uint8_t payload_type;
  enum st20_fmt t_fmt;
  enum mtl_log_level log_level;
  uint8_t framebuffer_cnt;

  /* internal data */
  obs_output_t* output;
  mtl_handle dev_handle;

  int idx;
  st20p_tx_handle handle;

  uint64_t total_bytes;
};

/* forward declarations */
static void mtl_output_init(struct mtl_tx_session* s);
static void mtl_output_terminate(struct mtl_tx_session* s);
static void mtl_output_update(void* vptr, obs_data_t* settings);

static const char* mtl_output_getname(void* unused) {
  UNUSED_PARAMETER(unused);
  return obs_module_text("MTLOutput");
}

static void mtl_output_defaults(obs_data_t* settings) {
  obs_data_set_default_string(settings, "port", "0000:4b:00.1");
  obs_data_set_default_string(settings, "lcores", "4,5");
  obs_data_set_default_string(settings, "sip", "192.168.96.2");
  obs_data_set_default_string(settings, "ip", "192.168.96.1");
  obs_data_set_default_int(settings, "udp_port", 20000);
  obs_data_set_default_int(settings, "payload_type", 112);
  obs_data_set_default_int(settings, "t_fmt", ST20_FMT_YUV_420_10BIT);
  obs_data_set_default_int(settings, "framebuffer_cnt", 3);
  obs_data_set_default_int(settings, "log_level", MTL_LOG_LEVEL_ERROR);
}

static obs_properties_t* mtl_output_properties(void* vptr) {
  MTL_TX_SESSION(vptr);

  obs_properties_t* props = obs_properties_create();

  obs_properties_add_text(props, "port", obs_module_text("Port"), OBS_TEXT_DEFAULT);

  obs_properties_add_text(props, "lcores", obs_module_text("Lcores"), OBS_TEXT_DEFAULT);
  obs_properties_add_text(props, "sip", obs_module_text("InterfaceIP"), OBS_TEXT_DEFAULT);
  obs_properties_add_text(props, "ip", obs_module_text("IP"), OBS_TEXT_DEFAULT);

  obs_properties_add_int(props, "udp_port", obs_module_text("UdpPort"), 1000, 65536, 1);
  obs_properties_add_int(props, "payload_type", obs_module_text("PayloadType"), 0, 255,
                         1);
  obs_properties_add_int(props, "framebuffer_cnt", obs_module_text("FramebuffCnt"), 2,
                         128, 1);

  obs_property_t* t_fmt_list =
      obs_properties_add_list(props, "t_fmt", obs_module_text("TransportFormat"),
                              OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
  obs_property_list_add_int(t_fmt_list, obs_module_text("YUV422_10bit"),
                            ST20_FMT_YUV_422_10BIT);
  obs_property_list_add_int(t_fmt_list, obs_module_text("YUV422_8bit"),
                            ST20_FMT_YUV_422_8BIT);
  obs_property_list_add_int(t_fmt_list, obs_module_text("YUV420_8bit"),
                            ST20_FMT_YUV_420_8BIT);

  obs_property_t* log_level_list =
      obs_properties_add_list(props, "log_level", obs_module_text("LogLevel"),
                              OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
  obs_property_list_add_int(log_level_list, "ERROR", MTL_LOG_LEVEL_ERROR);
  obs_property_list_add_int(log_level_list, "INFO", MTL_LOG_LEVEL_INFO);
  obs_property_list_add_int(log_level_list, "NOTICE", MTL_LOG_LEVEL_NOTICE);
  obs_property_list_add_int(log_level_list, "WARNING", MTL_LOG_LEVEL_WARNING);
  obs_property_list_add_int(log_level_list, "DEBUG", MTL_LOG_LEVEL_DEBUG);

  obs_data_t* settings = obs_output_get_settings(s->output);
  obs_data_release(settings);

  return props;
}

static void mtl_output_terminate(struct mtl_tx_session* s) {
  if (s->dev_handle) {
    mtl_stop(s->dev_handle);
  }

  if (s->handle) {
    st20p_tx_free(s->handle);
    s->handle = NULL;
  }

  if (s->dev_handle) {
    mtl_uninit(s->dev_handle);
    s->dev_handle = NULL;
  }
}

static void mtl_output_destroy(void* vptr) {
  MTL_TX_SESSION(vptr);

  if (!s) return;

  mtl_output_terminate(s);

  bfree(s);
}

static void mtl_output_init(struct mtl_tx_session* s) {
  struct mtl_init_params param;

  memset(&param, 0, sizeof(param));
  param.num_ports = 1;
  snprintf(param.port[MTL_PORT_P], MTL_PORT_MAX_LEN, "%s", s->port);
  inet_pton(AF_INET, s->sip, param.sip_addr[MTL_PORT_P]);
  param.pmd[MTL_PORT_P] = MTL_PMD_DPDK_USER;
  param.xdp_info[MTL_PORT_P].start_queue = 1;
  param.flags = MTL_FLAG_BIND_NUMA;  // default bind to numa
  param.log_level = s->log_level;    // mtl lib log level
  param.priv = s;                    // usr ctx pointer
  // user register ptp func, if not register, the internal ptp will be used
  param.ptp_get_time_fn = NULL;
  param.tx_queues_cnt[MTL_PORT_P] = 1;
  param.rx_queues_cnt[MTL_PORT_P] = 0;
  param.lcores = s->lcores;
  // create device
  mtl_handle dev_handle = mtl_init(&param);
  if (!dev_handle) {
    blog(LOG_ERROR, "mtl_init fail\n");
    return;
  }
  s->dev_handle = dev_handle;
  s->idx = 0;

  video_t* video = obs_output_video(s->output);
  const struct video_output_info* vo_info = video_output_get_info(video);

  struct st20p_tx_ops ops_tx;
  memset(&ops_tx, 0, sizeof(ops_tx));
  ops_tx.name = "mtl-input";
  ops_tx.priv = s;  // app handle register to lib
  ops_tx.port.num_port = 1;
  inet_pton(AF_INET, s->ip, ops_tx.port.dip_addr[MTL_PORT_P]);
  snprintf(ops_tx.port.port[MTL_PORT_P], MTL_PORT_MAX_LEN, "%s", s->port);
  ops_tx.port.udp_port[MTL_PORT_P] = s->udp_port;  // user config the udp port.
  ops_tx.width = vo_info->width;
  ops_tx.height = vo_info->height;
  ops_tx.fps = obs_to_mtl_fps(vo_info->fps_num, vo_info->fps_den);
  ops_tx.input_fmt = obs_to_mtl_format(vo_info->format);
  ops_tx.transport_fmt = s->t_fmt;
  ops_tx.framebuff_cnt = s->framebuffer_cnt;
  ops_tx.port.payload_type = s->payload_type;

  s->handle = st20p_tx_create(dev_handle, &ops_tx);
  if (!s->handle) {
    blog(LOG_ERROR, "tx_session is not correctly created\n");
    goto error;
  }

  mtl_start(s->dev_handle);
  return;

error:
  blog(LOG_ERROR, "Initialization failed, errno: %s", strerror(errno));
  mtl_output_terminate(s);
}

static void mtl_output_update(void* vptr, obs_data_t* settings) {
  MTL_TX_SESSION(vptr);

  s->port = (char*)obs_data_get_string(settings, "port");
  s->lcores = (char*)obs_data_get_string(settings, "lcores");
  s->sip = (char*)obs_data_get_string(settings, "sip");
  s->ip = (char*)obs_data_get_string(settings, "ip");
  s->udp_port = obs_data_get_int(settings, "udp_port");
  s->payload_type = obs_data_get_int(settings, "payload_type");
  s->t_fmt = obs_data_get_int(settings, "t_fmt");
  s->framebuffer_cnt = obs_data_get_int(settings, "framebuffer_cnt");
  s->log_level = obs_data_get_int(settings, "log_level");

  mtl_output_init(s);
}

static void* mtl_output_create(obs_data_t* settings, obs_output_t* output) {
  struct mtl_tx_session* s = bzalloc(sizeof(struct mtl_tx_session));
  s->output = output;

  mtl_output_update(s, settings);

  return s;
}

static void mtl_output_video_frame(void* vptr, struct video_data* obs_frame) {
  MTL_TX_SESSION(vptr);
  st20p_tx_handle handle = s->handle;
  struct st_frame* frame;
  size_t data_size = 0;
  frame = st20p_tx_get_frame(handle);
  if (!frame) return;

  uint8_t planes = st_frame_fmt_planes(frame->fmt);
  for (uint8_t plane = 0; plane < planes; plane++) { /* assume planes continuous */
    size_t plane_size =
        st_frame_least_linesize(frame->fmt, frame->width, plane) * frame->height;
    mtl_memcpy(frame->addr[plane], obs_frame->data[plane], plane_size);
    data_size += plane_size;
  }
  frame->tfmt = ST10_TIMESTAMP_FMT_MEDIA_CLK;
  frame->timestamp = obs_frame->timestamp;
  frame->data_size = data_size;

  st20p_tx_put_frame(handle, frame);
  s->total_bytes += frame->data_size;
}

static uint64_t mtl_output_total_bytes(void* vptr) {
  MTL_TX_SESSION(vptr);
  return s->total_bytes;
}

struct obs_output_info mtl_output = {
    .id = "mtl_output",
    .flags = OBS_OUTPUT_VIDEO,
    .get_name = mtl_output_getname,
    .create = mtl_output_create,
    .destroy = mtl_output_destroy,
    .raw_video = mtl_output_video_frame,
    .get_total_bytes = mtl_output_total_bytes,
    .update = mtl_output_update,
    .get_defaults = mtl_output_defaults,
    .get_properties = mtl_output_properties,
};

#endif