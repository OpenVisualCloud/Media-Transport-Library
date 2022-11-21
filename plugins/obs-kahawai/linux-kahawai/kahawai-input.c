/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include <arpa/inet.h>
#include <inttypes.h>
#include <mtl/st_pipeline_api.h>
#include <obs/obs-module.h>
#include <obs/util/bmem.h>
#include <obs/util/threading.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../../plugin_platform.h"

#define KH_RX_SESSION(voidptr) struct kh_rx_session* s = voidptr;

#define timeval2ns(tv) \
  (((uint64_t)tv.tv_sec * 1000000000) + ((uint64_t)tv.tv_usec * 1000))

#define blog(level, msg, ...) blog(level, "kahawai-input: " msg, ##__VA_ARGS__)

/**
 * Data structure for the kahawai source
 */
struct kh_rx_session {
  /* settings */
  char* lcores;
  char* port;
  char* sip;
  char* ip;
  uint16_t udp_port;
  uint8_t payload_type;
  uint32_t width;
  uint32_t height;
  enum st_fps fps;
  enum st_frame_fmt out_fmt;
  enum st20_fmt t_fmt;
  enum mtl_log_level log_level;
  uint8_t framebuffer_cnt;

  /* internal data */
  obs_source_t* source;
  mtl_handle dev_handle;
  struct obs_source_frame out;
  size_t plane_offsets[MAX_AV_PLANES];

  int idx;
  st20p_rx_handle handle;

  bool stop;
  pthread_t thread;
  pthread_cond_t wake_cond;
  pthread_mutex_t wake_mutex;
};

/* forward declarations */
static void kahawai_init(struct kh_rx_session* s);
static void kahawai_terminate(struct kh_rx_session* s);
static void kahawai_update(void* vptr, obs_data_t* settings);

static inline enum video_format kahawai_to_obs_video_format(enum st_frame_fmt fmt) {
  switch (fmt) {
    case ST_FRAME_FMT_YUV422PACKED8:
      return VIDEO_FORMAT_UYVY;
    default:
      return VIDEO_FORMAT_NONE;
  }
}

/**
 * Prepare the frame for obs
 */
static void kahawai_prep_obs_frame(struct kh_rx_session* s,
                                   struct obs_source_frame* frame,
                                   size_t* plane_offsets) {
  memset(frame, 0, sizeof(struct obs_source_frame));
  memset(plane_offsets, 0, sizeof(size_t) * MAX_AV_PLANES);

  /* get obs fmt from st here */
  const enum video_format format = kahawai_to_obs_video_format(s->out_fmt);

  frame->width = s->width;
  frame->height = s->height;
  frame->format = format;
  video_format_get_parameters_for_format(VIDEO_CS_DEFAULT, VIDEO_RANGE_DEFAULT, format,
                                         frame->color_matrix, frame->color_range_min,
                                         frame->color_range_max);

  switch (s->out_fmt) {
#if 0
    case ST20_FMT_YUV_420_8BIT:
      frame->linesize[0] = s->width;
      frame->linesize[1] = s->width / 2;
      frame->linesize[2] = s->width / 2;
      plane_offsets[1] = s->width * s->height;
      plane_offsets[2] = s->width * s->height * 5 / 4;
      break;
#endif
    default:
      frame->linesize[0] = s->width * 2; /* only uyvy */
      break;
  }
}

static int notify_frame_available(void* priv) {
  KH_RX_SESSION(priv);

  if (!s->handle) return -EIO;

  st_pthread_mutex_lock(&s->wake_mutex);
  st_pthread_cond_signal(&s->wake_cond);
  st_pthread_mutex_unlock(&s->wake_mutex);

  return 0;
}

/*
 * Worker thread to get video data
 */
static void* kahawai_thread(void* vptr) {
  KH_RX_SESSION(vptr);
  uint64_t frames;
  st20p_rx_handle handle = s->handle;
  struct st_frame* frame;

  blog(LOG_DEBUG, "%s: new rx thread", s->port);
  os_set_thread_name("kahawai: rx");

  // start
  frames = 0;
  blog(LOG_DEBUG, "%s: obs frame prepared", s->port);

  while (!s->stop) {
    frame = st20p_rx_get_frame(handle);
    if (!frame) { /* no frame */
      st_pthread_mutex_lock(&s->wake_mutex);
      if (!s->stop) st_pthread_cond_wait(&s->wake_cond, &s->wake_mutex);
      st_pthread_mutex_unlock(&s->wake_mutex);
      continue;
    }
    st_pthread_mutex_unlock(&s->wake_mutex);

    for (uint8_t i = 0; i < st_frame_fmt_planes(frame->fmt); ++i)
      s->out.data[i] = frame->addr[i];

    obs_source_output_video(s->source, &s->out);
    frames++;

    st20p_rx_put_frame(handle, frame);
  }

  blog(LOG_INFO, "%s: Stopped rx after %" PRIu64 " frames", s->port, frames);
  return NULL;
}

static const char* kahawai_getname(void* unused) {
  UNUSED_PARAMETER(unused);
  return obs_module_text("KahawaiInput");
}

static void kahawai_defaults(obs_data_t* settings) {
  obs_data_set_default_string(settings, "port", "0000:4b:00.1");
  obs_data_set_default_string(settings, "lcores", "4,5");
  obs_data_set_default_string(settings, "sip", "192.168.96.2");
  obs_data_set_default_string(settings, "ip", "192.168.96.1");
  obs_data_set_default_int(settings, "udp_port", 20000);
  obs_data_set_default_int(settings, "payload_type", 112);
  obs_data_set_default_int(settings, "width", 1920);
  obs_data_set_default_int(settings, "height", 1080);
  obs_data_set_default_int(settings, "fps", ST_FPS_P59_94);
  obs_data_set_default_int(settings, "t_fmt", ST20_FMT_YUV_420_10BIT);
  obs_data_set_default_int(settings, "out_fmt", ST_FRAME_FMT_YUV422PACKED8);
  obs_data_set_default_int(settings, "framebuffer_cnt", 3);
  obs_data_set_default_int(settings, "log_level", MTL_LOG_LEVEL_ERROR);
}

/**
 * Enable/Disable all properties for the source.
 *
 * @note A property that should be ignored can be specified
 *
 * @param props the source properties
 * @param ignore ignore this property
 * @param enable enable/disable all properties
 */
static void kahawai_props_set_enabled(obs_properties_t* props, obs_property_t* ignore,
                                      bool enable) {
  if (!props) return;

  for (obs_property_t* prop = obs_properties_first(props); prop != NULL;
       obs_property_next(&prop)) {
    if (prop == ignore) continue;

    obs_property_set_enabled(prop, enable);
  }
}

static bool on_start_clicked(obs_properties_t* ps, obs_property_t* p, void* vptr) {
  KH_RX_SESSION(vptr);

  kahawai_init(s);
  obs_property_set_description(p, obs_module_text("Started"));

  obs_property_t* stop = obs_properties_get(ps, "stop");
  obs_property_set_description(stop, obs_module_text("Stop"));
  obs_property_set_enabled(stop, true);

  kahawai_props_set_enabled(ps, stop, false);

  return true;
}

static bool on_stop_clicked(obs_properties_t* ps, obs_property_t* p, void* vptr) {
  KH_RX_SESSION(vptr);

  kahawai_terminate(s);
  obs_property_set_description(p, obs_module_text("Stopped"));

  obs_property_t* start = obs_properties_get(ps, "start");
  obs_property_set_description(start, obs_module_text("Start"));
  obs_property_set_enabled(p, false);

  kahawai_props_set_enabled(ps, p, true);

  return true;
}

static obs_properties_t* kahawai_properties(void* vptr) {
  KH_RX_SESSION(vptr);

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
  obs_properties_add_int(props, "width", obs_module_text("Width"), 1, 65535, 1);
  obs_properties_add_int(props, "height", obs_module_text("Height"), 1, 65535, 1);
  obs_property_t* fps_list = obs_properties_add_list(
      props, "fps", obs_module_text("FPS"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
  obs_property_list_add_int(fps_list, obs_module_text("23.98"), ST_FPS_P23_98);
  obs_property_list_add_int(fps_list, obs_module_text("24"), ST_FPS_P24);
  obs_property_list_add_int(fps_list, obs_module_text("25"), ST_FPS_P25);
  obs_property_list_add_int(fps_list, obs_module_text("29.97"), ST_FPS_P29_97);
  obs_property_list_add_int(fps_list, obs_module_text("30"), ST_FPS_P30);
  obs_property_list_add_int(fps_list, obs_module_text("50"), ST_FPS_P50);
  obs_property_list_add_int(fps_list, obs_module_text("59.94"), ST_FPS_P59_94);
  obs_property_list_add_int(fps_list, obs_module_text("60"), ST_FPS_P60);
  obs_property_list_add_int(fps_list, obs_module_text("100"), ST_FPS_P100);
  obs_property_list_add_int(fps_list, obs_module_text("119.88"), ST_FPS_P119_88);
  obs_property_list_add_int(fps_list, obs_module_text("120"), ST_FPS_P120);

  obs_property_t* t_fmt_list =
      obs_properties_add_list(props, "t_fmt", obs_module_text("TransportFormat"),
                              OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
  obs_property_list_add_int(t_fmt_list, obs_module_text("YUV422BE10"),
                            ST20_FMT_YUV_422_10BIT);
  obs_property_list_add_int(t_fmt_list, obs_module_text("YUV422BE8"),
                            ST20_FMT_YUV_422_8BIT);

  obs_property_t* out_fmt_list =
      obs_properties_add_list(props, "out_fmt", obs_module_text("OutputFormat"),
                              OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
  obs_property_list_add_int(out_fmt_list, obs_module_text("UYVY"),
                            ST_FRAME_FMT_YUV422PACKED8);

  obs_property_t* log_level_list =
      obs_properties_add_list(props, "log_level", obs_module_text("LogLevel"),
                              OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
  obs_property_list_add_int(log_level_list, "ERROR", MTL_LOG_LEVEL_ERROR);
  obs_property_list_add_int(log_level_list, "INFO", MTL_LOG_LEVEL_INFO);
  obs_property_list_add_int(log_level_list, "NOTICE", MTL_LOG_LEVEL_NOTICE);
  obs_property_list_add_int(log_level_list, "WARNING", MTL_LOG_LEVEL_WARNING);
  obs_property_list_add_int(log_level_list, "DEBUG", MTL_LOG_LEVEL_DEBUG);

  obs_properties_add_button(props, "start", obs_module_text("Start"), on_start_clicked);
  obs_properties_add_button(props, "stop", obs_module_text("Stop"), on_stop_clicked);
  obs_property_t* stop = obs_properties_get(props, "stop");
  obs_property_set_enabled(stop, false);

  obs_data_t* settings = obs_source_get_settings(s->source);
  obs_data_release(settings);

  return props;
}

static void kahawai_terminate(struct kh_rx_session* s) {
  s->stop = true;
  pthread_mutex_lock(&s->wake_mutex);
  pthread_cond_signal(&s->wake_cond);
  pthread_mutex_unlock(&s->wake_mutex);
  pthread_join(s->thread, NULL);

  if (s->dev_handle) {
    mtl_stop(s->dev_handle);
  }

  if (s->handle) {
    st20p_rx_free(s->handle);
    s->handle = NULL;
  }
  pthread_mutex_destroy(&s->wake_mutex);
  pthread_cond_destroy(&s->wake_cond);

  if (s->dev_handle) {
    mtl_uninit(s->dev_handle);
    s->dev_handle = NULL;
  }
}

static void kahawai_destroy(void* vptr) {
  KH_RX_SESSION(vptr);

  if (!s) return;

  kahawai_terminate(s);

  bfree(s);
}

static void kahawai_init(struct kh_rx_session* s) {
  struct mtl_init_params param;

  memset(&param, 0, sizeof(param));
  param.num_ports = 1;
  strncpy(param.port[MTL_PORT_P], s->port, MTL_PORT_MAX_LEN);
  inet_pton(AF_INET, s->sip, param.sip_addr[MTL_PORT_P]);
  param.pmd[MTL_PORT_P] = MTL_PMD_DPDK_USER;
  param.xdp_info[MTL_PORT_P].queue_count = 1;
  param.xdp_info[MTL_PORT_P].start_queue = 16;
  param.flags = MTL_FLAG_BIND_NUMA;  // default bind to numa
  param.log_level = s->log_level;    // kahawai lib log level
  param.priv = s;                    // usr ctx pointer
  // user regist ptp func, if not regist, the internal ptp will be used
  param.ptp_get_time_fn = NULL;
  param.tx_sessions_cnt_max = 0;
  param.rx_sessions_cnt_max = 1;
  param.lcores = s->lcores;
  // create device
  mtl_handle dev_handle = mtl_init(&param);
  if (!dev_handle) {
    blog(LOG_ERROR, "mtl_init fail\n");
    return;
  }
  s->dev_handle = dev_handle;
  s->idx = 0;

  struct st20p_rx_ops ops_rx;
  memset(&ops_rx, 0, sizeof(ops_rx));
  ops_rx.name = "kahawai-input";
  ops_rx.priv = s;  // app handle register to lib
  ops_rx.port.num_port = 1;
  inet_pton(AF_INET, s->ip, ops_rx.port.sip_addr[MTL_PORT_P]);
  strncpy(ops_rx.port.port[MTL_PORT_P], s->port, MTL_PORT_MAX_LEN);
  ops_rx.port.udp_port[MTL_PORT_P] = s->udp_port;  // user config the udp port.
  ops_rx.width = s->width;
  ops_rx.height = s->height;
  ops_rx.fps = s->fps;
  ops_rx.output_fmt = s->out_fmt;
  ops_rx.transport_fmt = s->t_fmt;
  ops_rx.framebuff_cnt = s->framebuffer_cnt;
  ops_rx.port.payload_type = s->payload_type;
  // app regist non-block func, app get a frame ready notification info by this cb
  ops_rx.notify_frame_available = notify_frame_available;

  s->handle = st20p_rx_create(dev_handle, &ops_rx);
  if (!s->handle) {
    blog(LOG_ERROR, "rx_session is not correctly created\n");
    goto error;
  }

  s->stop = false;
  st_pthread_mutex_init(&s->wake_mutex, NULL);
  st_pthread_cond_init(&s->wake_cond, NULL);
  int ret = pthread_create(&s->thread, NULL, kahawai_thread, s);
  if (ret < 0) {
    blog(LOG_ERROR, "%s(%d), app_thread create fail\n", __func__, ret);
    goto error;
  }

  mtl_start(s->dev_handle);
  return;

error:
  blog(LOG_ERROR, "Initialization failed, errno: %s", strerror(errno));
  kahawai_terminate(s);
}

static void kahawai_update(void* vptr, obs_data_t* settings) {
  KH_RX_SESSION(vptr);

  s->port = (char*)obs_data_get_string(settings, "port");
  s->lcores = (char*)obs_data_get_string(settings, "lcores");
  s->sip = (char*)obs_data_get_string(settings, "sip");
  s->ip = (char*)obs_data_get_string(settings, "ip");
  s->udp_port = obs_data_get_int(settings, "udp_port");
  s->payload_type = obs_data_get_int(settings, "payload_type");
  s->width = obs_data_get_int(settings, "width");
  s->height = obs_data_get_int(settings, "height");
  s->fps = obs_data_get_int(settings, "fps");
  s->t_fmt = obs_data_get_int(settings, "t_fmt");
  s->out_fmt = obs_data_get_int(settings, "out_fmt");
  s->framebuffer_cnt = obs_data_get_int(settings, "framebuffer_cnt");
  s->log_level = obs_data_get_int(settings, "log_level");
}

static void* kahawai_create(obs_data_t* settings, obs_source_t* source) {
  struct kh_rx_session* s = bzalloc(sizeof(struct kh_rx_session));
  s->source = source;

  kahawai_update(s, settings);

  return s;
}

struct obs_source_info kahawai_input = {
    .id = "kahawai_input",
    .type = OBS_SOURCE_TYPE_INPUT,
    .output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_DO_NOT_DUPLICATE,
    .get_name = kahawai_getname,
    .create = kahawai_create,
    .destroy = kahawai_destroy,
    .update = kahawai_update,
    .get_defaults = kahawai_defaults,
    .get_properties = kahawai_properties,
    .icon_type = OBS_ICON_TYPE_MEDIA,
};
