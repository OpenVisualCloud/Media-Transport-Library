/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include <arpa/inet.h>
#include <inttypes.h>
#include <obs/obs-module.h>
#include <obs/util/bmem.h>
#include <obs/util/threading.h>
#include <st20_dpdk_api.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../../plugin_platform.h"

#define FRAMEBUFF_CNT (2)
#define PAYLOAD_TYPE (112)

#define KH_RX_SESSION(voidptr) struct kh_rx_session* s = voidptr;

#define timeval2ns(tv) \
  (((uint64_t)tv.tv_sec * 1000000000) + ((uint64_t)tv.tv_usec * 1000))

#define blog(level, msg, ...) blog(level, "kahawai-input: " msg, ##__VA_ARGS__)

struct st_rx_frame {
  void* frame;
  size_t size;
};

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
  enum st20_fmt fmt;
  enum st_log_level log_level;
  /* detected */
  uint32_t width;
  uint32_t height;
  enum st_fps fps;

  /* internal data */
  obs_source_t* source;
  st_handle dev_handle;
  struct obs_source_frame out;
  size_t plane_offsets[MAX_AV_PLANES];

  int idx;
  void* handle;

  bool stop;
  pthread_t thread;
  pthread_cond_t wake_cond;
  pthread_mutex_t wake_mutex;

  uint16_t framebuff_producer_idx;
  uint16_t framebuff_consumer_idx;
  struct st_rx_frame* framebuffs;

  volatile bool detected;
};

/* forward declarations */
static void kahawai_init(struct kh_rx_session* s);
static void kahawai_terminate(struct kh_rx_session* s);
static void kahawai_update(void* vptr, obs_data_t* settings);

static inline enum video_format kahawai_to_obs_video_format(enum st20_fmt fmt) {
  switch (fmt) {
    case ST20_FMT_YUV_420_8BIT:
      return VIDEO_FORMAT_I420;
    case ST20_FMT_YUV_422_8BIT:
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
  const enum video_format format = kahawai_to_obs_video_format(s->fmt);

  frame->width = s->width;
  frame->height = s->height;
  frame->format = format;
  video_format_get_parameters_for_format(VIDEO_CS_DEFAULT, VIDEO_RANGE_DEFAULT, format,
                                         frame->color_matrix, frame->color_range_min,
                                         frame->color_range_max);

  switch (s->fmt) {
    case ST20_FMT_YUV_420_8BIT:
      frame->linesize[0] = s->width;
      frame->linesize[1] = s->width / 2;
      frame->linesize[2] = s->width / 2;
      plane_offsets[1] = s->width * s->height;
      plane_offsets[2] = s->width * s->height * 5 / 4;
      break;
    default:
      frame->linesize[0] = s->width * 2; /* only uyvy */
      break;
  }
}

static int rx_video_enqueue_frame(struct kh_rx_session* s, void* frame, size_t size) {
  uint16_t producer_idx = s->framebuff_producer_idx;
  struct st_rx_frame* framebuff = &s->framebuffs[producer_idx];

  if (framebuff->frame) {
    return -EBUSY;
  }

  framebuff->frame = frame;
  framebuff->size = size;
  /* point to next */
  producer_idx++;
  if (producer_idx >= FRAMEBUFF_CNT) producer_idx = 0;
  s->framebuff_producer_idx = producer_idx;
  return 0;
}

static int rx_video_frame_ready(void* priv, void* frame,
                                struct st20_rx_frame_meta* meta) {
  KH_RX_SESSION(priv);

  if (!s->handle) return -EIO;

  /* incomplete frame */
  if (!st_is_frame_complete(meta->status)) {
    st20_rx_put_framebuff(s->handle, frame);
    return 0;
  }

  st_pthread_mutex_lock(&s->wake_mutex);
  int ret = rx_video_enqueue_frame(s, frame, meta->frame_total_size);
  if (ret < 0) {
    blog(LOG_ERROR, "%s(%d), frame %p dropped\n", __func__, s->idx, frame);
    /* free the queue */
    st20_rx_put_framebuff(s->handle, frame);
    st_pthread_mutex_unlock(&s->wake_mutex);
    return ret;
  }
  st_pthread_cond_signal(&s->wake_cond);
  st_pthread_mutex_unlock(&s->wake_mutex);

  return 0;
}

static int rx_video_detected(void* priv, const struct st20_detect_meta* meta,
                             struct st20_detect_reply* reply) {
  KH_RX_SESSION(priv);

  s->width = meta->width;
  s->height = meta->height;
  s->fps = meta->fps;

  kahawai_prep_obs_frame(s, &s->out, s->plane_offsets);
  s->detected = true;

  blog(LOG_INFO, "Video info detected, frame created");
  blog(LOG_INFO, "width: %u height: %u", s->width, s->height);
  blog(LOG_INFO, "framerate: %.2f fps", st_frame_rate(s->fps));

  return 0;
}

/*
 * Worker thread to get video data
 */
static void* kahawai_thread(void* vptr) {
  KH_RX_SESSION(vptr);
  uint8_t* start;
  uint64_t frames;

  blog(LOG_DEBUG, "%s: new rx thread", s->port);
  os_set_thread_name("kahawai: rx");

  // start
  frames = 0;

  int consumer_idx;
  struct st_rx_frame* framebuff;
  blog(LOG_DEBUG, "%s: obs frame prepared", s->port);

  while (!s->stop) {
    st_pthread_mutex_lock(&s->wake_mutex);
    consumer_idx = s->framebuff_consumer_idx;
    framebuff = &s->framebuffs[consumer_idx];
    if (!framebuff->frame) {
      /* no ready frame */
      if (!s->stop) st_pthread_cond_wait(&s->wake_cond, &s->wake_mutex);
      st_pthread_mutex_unlock(&s->wake_mutex);
      continue;
    }
    st_pthread_mutex_unlock(&s->wake_mutex);

    if (s->detected) {
      start = (uint8_t*)framebuff->frame;

      for (uint_fast32_t i = 0; i < MAX_AV_PLANES; ++i)
        s->out.data[i] = start + s->plane_offsets[i];
      obs_source_output_video(s->source, &s->out);
      frames++;
    }

    st20_rx_put_framebuff(s->handle, framebuff->frame);
    /* point to next */
    st_pthread_mutex_lock(&s->wake_mutex);
    framebuff->frame = NULL;
    consumer_idx++;
    if (consumer_idx >= FRAMEBUFF_CNT) consumer_idx = 0;
    s->framebuff_consumer_idx = consumer_idx;
    st_pthread_mutex_unlock(&s->wake_mutex);
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
  obs_data_set_default_string(settings, "sip", "192.168.96.189");
  obs_data_set_default_string(settings, "ip", "192.168.96.188");
  obs_data_set_default_int(settings, "udp_port", 20000);
  obs_data_set_default_int(settings, "fmt", ST20_FMT_YUV_420_8BIT);
  obs_data_set_default_int(settings, "log_level", ST_LOG_LEVEL_ERROR);
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

  obs_property_t* fmt_list = obs_properties_add_list(
      props, "fmt", obs_module_text("Format"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
  obs_property_list_add_int(fmt_list, obs_module_text("I420"), ST20_FMT_YUV_420_8BIT);
  obs_property_list_add_int(fmt_list, obs_module_text("UYVY"), ST20_FMT_YUV_422_8BIT);

  obs_property_t* log_level_list =
      obs_properties_add_list(props, "log_level", obs_module_text("LogLevel"),
                              OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
  obs_property_list_add_int(log_level_list, "ERROR", ST_LOG_LEVEL_ERROR);
  obs_property_list_add_int(log_level_list, "INFO", ST_LOG_LEVEL_INFO);
  obs_property_list_add_int(log_level_list, "WARNING", ST_LOG_LEVEL_WARNING);
  obs_property_list_add_int(log_level_list, "DEBUG", ST_LOG_LEVEL_DEBUG);

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
    st_stop(s->dev_handle);
  }

  if (s->handle) {
    st20_rx_free(s->handle);
    s->handle = NULL;
  }
  pthread_mutex_destroy(&s->wake_mutex);
  pthread_cond_destroy(&s->wake_cond);

  if (s->dev_handle) {
    st_uninit(s->dev_handle);
    s->dev_handle = NULL;
  }

  if (s->framebuffs) {
    free(s->framebuffs);
    s->framebuffs = NULL;
  }
}

static void kahawai_destroy(void* vptr) {
  KH_RX_SESSION(vptr);

  if (!s) return;

  kahawai_terminate(s);

  bfree(s);
}

static void kahawai_init(struct kh_rx_session* s) {
  struct st_init_params param;

  memset(&param, 0, sizeof(param));
  param.num_ports = 1;
  strncpy(param.port[ST_PORT_P], s->port, ST_PORT_MAX_LEN);
  inet_pton(AF_INET, s->sip, param.sip_addr[ST_PORT_P]);
  param.pmd[ST_PORT_P] = ST_PMD_DPDK_USER;
  param.xdp_info[ST_PORT_P].queue_count = 1;
  param.xdp_info[ST_PORT_P].start_queue = 16;
  param.flags = ST_FLAG_BIND_NUMA;  // default bind to numa
  param.log_level = s->log_level;   // kahawai lib log level
  param.priv = s;                   // usr ctx pointer
  // user regist ptp func, if not regist, the internal ptp will be used
  param.ptp_get_time_fn = NULL;
  param.tx_sessions_cnt_max = 0;
  param.rx_sessions_cnt_max = 1;
  param.lcores = s->lcores;
  // create device
  st_handle dev_handle = st_init(&param);
  if (!dev_handle) {
    blog(LOG_ERROR, "st_init fail\n");
    return;
  }
  s->dev_handle = dev_handle;

  s->idx = 0;
  s->framebuffs = (struct st_rx_frame*)malloc(sizeof(*s->framebuffs) * FRAMEBUFF_CNT);
  if (!s->framebuffs) {
    blog(LOG_ERROR, "%s(%d), framebuffs malloc fail\n", __func__, s->idx);
    goto error;
  }
  for (uint16_t j = 0; j < FRAMEBUFF_CNT; j++) s->framebuffs[j].frame = NULL;
  s->framebuff_producer_idx = 0;
  s->framebuff_consumer_idx = 0;

  struct st20_rx_ops ops_rx;
  memset(&ops_rx, 0, sizeof(ops_rx));
  ops_rx.name = "kahawai-input";
  ops_rx.priv = s;  // app handle register to lib
  ops_rx.num_port = 1;
  inet_pton(AF_INET, s->ip, ops_rx.sip_addr[ST_PORT_P]);
  strncpy(ops_rx.port[ST_PORT_P], s->port, ST_PORT_MAX_LEN);
  ops_rx.udp_port[ST_PORT_P] = s->udp_port;  // user config the udp port.
  ops_rx.pacing = ST21_PACING_NARROW;
  ops_rx.type = ST20_TYPE_FRAME_LEVEL;
  ops_rx.width = 1920;
  ops_rx.height = 1080;
  ops_rx.fps = ST_FPS_P59_94;
  ops_rx.fmt = s->fmt;
  ops_rx.framebuff_cnt = FRAMEBUFF_CNT;
  ops_rx.payload_type = PAYLOAD_TYPE;
  // app regist non-block func, app get a frame ready notification info by this cb
  ops_rx.notify_frame_ready = rx_video_frame_ready;
  ops_rx.notify_detected = rx_video_detected;
  ops_rx.flags |= ST20_RX_FLAG_AUTO_DETECT;

  s->handle = st20_rx_create(dev_handle, &ops_rx);
  if (!s->handle) {
    blog(LOG_ERROR, "rx_session is not correctly created\n");
    goto error;
  }
  struct st_queue_meta queue_meta;
  st20_rx_get_queue_meta(s->handle, &queue_meta);
  blog(LOG_DEBUG, "queue_id %u, start_queue %u\n", queue_meta.queue_id[ST_PORT_P],
       queue_meta.start_queue[ST_PORT_P]);
  s->stop = false;
  st_pthread_mutex_init(&s->wake_mutex, NULL);
  st_pthread_cond_init(&s->wake_cond, NULL);
  int ret = pthread_create(&s->thread, NULL, kahawai_thread, s);
  if (ret < 0) {
    blog(LOG_ERROR, "%s(%d), app_thread create fail\n", __func__, ret);
    goto error;
  }

  st_start(s->dev_handle);
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
  s->fmt = obs_data_get_int(settings, "fmt");
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
