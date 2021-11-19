#include "parse_json.h"

#include "log.h"

#define VNAME(name) (#name)

#define CHECK_STRING(string)                                  \
  do {                                                        \
    if (string == NULL) {                                     \
      err("%s, can not parse %s\n", __func__, VNAME(string)); \
      return -ST_JSON_PARSE_FAIL;                             \
    }                                                         \
  } while (0)

static int st_json_parse_interfaces(json_object* interface_obj,
                                    st_json_interface_t* interface) {
  if (interface_obj == NULL || interface == NULL) {
    err("%s, can not parse interfaces!\n", __func__);
    return -ST_JSON_NULL;
  }

  const char* name =
      json_object_get_string(json_object_object_get(interface_obj, "name"));
  CHECK_STRING(name);
  snprintf(interface->name, sizeof(interface->name), "%s", name);

  const char* ip = json_object_get_string(json_object_object_get(interface_obj, "ip"));
  CHECK_STRING(ip);
  inet_pton(AF_INET, ip, interface->ip_addr);

  return ST_JSON_SUCCESS;
}

static int st_json_parse_tx_video(int idx, json_object* video_obj,
                                  st_json_tx_video_session_t* video) {
  if (video_obj == NULL || video == NULL) {
    err("%s, can not parse tx video session\n", __func__);
    return -ST_JSON_NULL;
  }

  /* parse video type */
  const char* type = json_object_get_string(json_object_object_get(video_obj, "type"));
  CHECK_STRING(type);
  if (strcmp(type, "frame") == 0) {
    video->type = ST20_TYPE_FRAME_LEVEL;
  } else if (strcmp(type, "rtp") == 0) {
    video->type = ST20_TYPE_RTP_LEVEL;
  } else {
    err("%s, invalid video type %s\n", __func__, type);
    return -ST_JSON_NOT_VALID;
  }

  /* parse video pacing */
  const char* pacing =
      json_object_get_string(json_object_object_get(video_obj, "pacing"));
  CHECK_STRING(pacing);
  if (strcmp(pacing, "gap") == 0) {
    video->pacing = PACING_GAP;
  } else if (strcmp(pacing, "linear") == 0) {
    video->pacing = PACING_LINEAR;
  } else {
    err("%s, invalid video pacing %s\n", __func__, pacing);
    return -ST_JSON_NOT_VALID;
  }

  /* parse udp port */
  int start_port = json_object_get_int(json_object_object_get(video_obj, "start_port"));
  if (start_port < 0 || start_port > 65535) {
    err("%s, invalid start port %d\n", __func__, start_port);
    return -ST_JSON_NOT_VALID;
  }
  video->udp_port = start_port + idx;

  /* parse tr offset */
  const char* tr_offset =
      json_object_get_string(json_object_object_get(video_obj, "tr_offset"));
  CHECK_STRING(tr_offset);
  if (strcmp(tr_offset, "default") == 0) {
    video->tr_offset = TR_OFFSET_DEFAULT;
  } else if (strcmp(pacing, "none") == 0) {
    video->tr_offset = TR_OFFSET_NONE;
  } else {
    err("%s, invalid video tr_offset %s\n", __func__, tr_offset);
    return -ST_JSON_NOT_VALID;
  }

  /* parse video format */
  const char* video_format =
      json_object_get_string(json_object_object_get(video_obj, "video_format"));
  CHECK_STRING(video_format);
  if (strcmp(video_format, "i1080p59") == 0) {
    video->video_format = VIDEO_FORMAT_1080P_59FPS;
  } else if (strcmp(video_format, "i1080p50") == 0) {
    video->video_format = VIDEO_FORMAT_1080P_50FPS;
  } else if (strcmp(video_format, "i1080p29") == 0) {
    video->video_format = VIDEO_FORMAT_1080P_29FPS;
  } else if (strcmp(video_format, "i2160p59") == 0) {
    video->video_format = VIDEO_FORMAT_2160P_59FPS;
  } else if (strcmp(video_format, "i2160p50") == 0) {
    video->video_format = VIDEO_FORMAT_2160P_50FPS;
  } else if (strcmp(video_format, "i2160p29") == 0) {
    video->video_format = VIDEO_FORMAT_2160P_29FPS;
  } else if (strcmp(video_format, "i720p59") == 0) {
    video->video_format = VIDEO_FORMAT_720P_59FPS;
  } else if (strcmp(video_format, "i720p50") == 0) {
    video->video_format = VIDEO_FORMAT_720P_50FPS;
  } else if (strcmp(video_format, "i720p29") == 0) {
    video->video_format = VIDEO_FORMAT_720P_29FPS;
  } else {
    err("%s, invalid video format %s\n", __func__, video_format);
    return -ST_JSON_NOT_VALID;
  }

  /* parse pixel group format */
  const char* pg_format =
      json_object_get_string(json_object_object_get(video_obj, "pg_format"));
  CHECK_STRING(pg_format);
  if (strcmp(pg_format, "YUV_422_10bit") == 0) {
    video->pg_format = ST20_FMT_YUV_422_10BIT;
  } else if (strcmp(pg_format, "YUV_422_8bit") == 0) {
    video->pg_format = ST20_FMT_YUV_422_8BIT;
  } else if (strcmp(pg_format, "YUV_422_12bit") == 0) {
    video->pg_format = ST20_FMT_YUV_422_12BIT;
  } else if (strcmp(pg_format, "YUV_422_16bit") == 0) {
    video->pg_format = ST20_FMT_YUV_422_16BIT;
  } else if (strcmp(pg_format, "YUV_420_8bit") == 0) {
    video->pg_format = ST20_FMT_YUV_420_8BIT;
  } else if (strcmp(pg_format, "YUV_420_10bit") == 0) {
    video->pg_format = ST20_FMT_YUV_420_10BIT;
  } else if (strcmp(pg_format, "YUV_420_12bit") == 0) {
    video->pg_format = ST20_FMT_YUV_420_12BIT;
  } else if (strcmp(pg_format, "RGB_8bit") == 0) {
    video->pg_format = ST20_FMT_RGB_8BIT;
  } else if (strcmp(pg_format, "RGB_10bit") == 0) {
    video->pg_format = ST20_FMT_RGB_10BIT;
  } else if (strcmp(pg_format, "RGB_12bit") == 0) {
    video->pg_format = ST20_FMT_RGB_12BIT;
  } else if (strcmp(pg_format, "RGB_16bit") == 0) {
    video->pg_format = ST20_FMT_RGB_16BIT;
  } else {
    err("%s, invalid pixel group format %s\n", __func__, pg_format);
    return -ST_JSON_NOT_VALID;
  }

  /* parse video url */
  const char* video_url =
      json_object_get_string(json_object_object_get(video_obj, "video_url"));
  CHECK_STRING(video_url);
  snprintf(video->video_url, sizeof(video->video_url), "%s", video_url);

  return ST_JSON_SUCCESS;
}

static int st_json_parse_tx_audio(int idx, json_object* audio_obj,
                                  st_json_tx_audio_session_t* audio) {
  if (audio_obj == NULL || audio == NULL) {
    err("%s, can not parse tx audio session\n", __func__);
    return -ST_JSON_NULL;
  }

  /* parse audio type */
  const char* type = json_object_get_string(json_object_object_get(audio_obj, "type"));
  CHECK_STRING(type);
  if (strcmp(type, "frame") == 0) {
    audio->type = ST30_TYPE_FRAME_LEVEL;
  } else if (strcmp(type, "rtp") == 0) {
    audio->type = ST30_TYPE_RTP_LEVEL;
  } else {
    err("%s, invalid audio type %s\n", __func__, type);
    return -ST_JSON_NOT_VALID;
  }

  /* parse audio format */
  const char* audio_format =
      json_object_get_string(json_object_object_get(audio_obj, "audio_format"));
  CHECK_STRING(audio_format);
  if (strcmp(audio_format, "PCM8") == 0) {
    audio->audio_format = ST30_FMT_PCM8;
  } else if (strcmp(audio_format, "PCM16") == 0) {
    audio->audio_format = ST30_FMT_PCM16;
  } else if (strcmp(audio_format, "PCM24") == 0) {
    audio->audio_format = ST30_FMT_PCM24;
  } else {
    err("%s, invalid audio format %s\n", __func__, audio_format);
    return -ST_JSON_NOT_VALID;
  }

  /* parse audio channel */
  const char* audio_channel =
      json_object_get_string(json_object_object_get(audio_obj, "audio_channel"));
  CHECK_STRING(audio_channel);
  if (strcmp(audio_channel, "mono") == 0) {
    audio->audio_channel = ST30_CHAN_MONO;
  } else if (strcmp(audio_channel, "stereo") == 0) {
    audio->audio_channel = ST30_CHAN_STEREO;
  } else {
    err("%s, invalid audio channel %s\n", __func__, audio_channel);
    return -ST_JSON_NOT_VALID;
  }

  /* parse audio sampling */
  const char* audio_sampling =
      json_object_get_string(json_object_object_get(audio_obj, "audio_sampling"));
  CHECK_STRING(audio_sampling);
  if (strcmp(audio_sampling, "48kHz") == 0) {
    audio->audio_sampling = ST30_SAMPLING_48K;
  } else if (strcmp(audio_sampling, "96kHz") == 0) {
    audio->audio_sampling = ST30_SAMPLING_96K;
  } else {
    err("%s, invalid audio sampling %s\n", __func__, audio_sampling);
    return -ST_JSON_NOT_VALID;
  }

  /* parse audio frame time (ms) */
  int frametime_ms =
      json_object_get_int(json_object_object_get(audio_obj, "audio_frametime_ms"));
  if (frametime_ms < 0) {
    err("%s, invalid audio frame time %d\n", __func__, frametime_ms);
    return -ST_JSON_NOT_VALID;
  }
  audio->audio_frametime_ms = frametime_ms;

  /* parse udp port */
  int start_port = json_object_get_int(json_object_object_get(audio_obj, "start_port"));
  if (start_port < 0 || start_port > 65535) {
    err("%s, invalid start port %d\n", __func__, start_port);
    return -ST_JSON_NOT_VALID;
  }
  audio->udp_port = start_port + idx;

  /* parse audio url */
  const char* audio_url =
      json_object_get_string(json_object_object_get(audio_obj, "audio_url"));
  CHECK_STRING(audio_url);
  snprintf(audio->audio_url, sizeof(audio->audio_url), "%s", audio_url);

  return ST_JSON_SUCCESS;
}

static int st_json_parse_tx_anc(int idx, json_object* anc_obj,
                                st_json_tx_ancillary_session_t* anc) {
  if (anc_obj == NULL || anc == NULL) {
    err("%s, can not parse tx anc session\n", __func__);
    return -ST_JSON_NULL;
  }
  /* parse anc type */
  const char* type = json_object_get_string(json_object_object_get(anc_obj, "type"));
  CHECK_STRING(type);
  if (strcmp(type, "frame") == 0) {
    anc->type = ST40_TYPE_FRAME_LEVEL;
  } else if (strcmp(type, "rtp") == 0) {
    anc->type = ST40_TYPE_RTP_LEVEL;
  } else {
    err("%s, invalid anc type %s\n", __func__, type);
    return -ST_JSON_NOT_VALID;
  }
  /* parse anc format */
  const char* anc_format =
      json_object_get_string(json_object_object_get(anc_obj, "ancillary_format"));
  CHECK_STRING(anc_format);
  if (strcmp(anc_format, "closed_caption") == 0) {
    anc->anc_format = ANC_FORMAT_CLOSED_CAPTION;
  } else {
    err("%s, invalid anc format %s\n", __func__, anc_format);
    return -ST_JSON_NOT_VALID;
  }

  /* parse anc fps */
  const char* anc_fps =
      json_object_get_string(json_object_object_get(anc_obj, "ancillary_fps"));
  CHECK_STRING(anc_fps);
  if (strcmp(anc_fps, "p59") == 0) {
    anc->anc_fps = ST_FPS_P59_94;
  } else if (strcmp(anc_fps, "p50") == 0) {
    anc->anc_fps = ST_FPS_P50;
  } else if (strcmp(anc_fps, "p29") == 0) {
    anc->anc_fps = ST_FPS_P29_97;
  } else {
    err("%s, invalid anc fps %s\n", __func__, anc_fps);
    return -ST_JSON_NOT_VALID;
  }

  /* parse udp port */
  int start_port = json_object_get_int(json_object_object_get(anc_obj, "start_port"));
  if (start_port < 0 || start_port > 65535) {
    err("%s, invalid start port %d\n", __func__, start_port);
    return -ST_JSON_NOT_VALID;
  }
  anc->udp_port = start_port + idx;

  /* parse anc url */
  const char* anc_url =
      json_object_get_string(json_object_object_get(anc_obj, "ancillary_url"));
  CHECK_STRING(anc_url);
  snprintf(anc->anc_url, sizeof(anc->anc_url), "%s", anc_url);

  return ST_JSON_SUCCESS;
}

static int st_json_parse_rx_video(int idx, json_object* video_obj,
                                  st_json_rx_video_session_t* video) {
  if (video_obj == NULL || video == NULL) {
    err("%s, can not parse rx video session\n", __func__);
    return -ST_JSON_NULL;
  }

  /* parse video type */
  const char* type = json_object_get_string(json_object_object_get(video_obj, "type"));
  CHECK_STRING(type);
  if (strcmp(type, "frame") == 0) {
    video->type = ST20_TYPE_FRAME_LEVEL;
  } else if (strcmp(type, "rtp") == 0) {
    video->type = ST20_TYPE_RTP_LEVEL;
  } else {
    err("%s, invalid video type %s\n", __func__, type);
    return -ST_JSON_NOT_VALID;
  }

  /* parse video pacing */
  const char* pacing =
      json_object_get_string(json_object_object_get(video_obj, "pacing"));
  CHECK_STRING(pacing);
  if (strcmp(pacing, "gap") == 0) {
    video->pacing = PACING_GAP;
  } else if (strcmp(pacing, "linear") == 0) {
    video->pacing = PACING_LINEAR;
  } else {
    err("%s, invalid video pacing %s\n", __func__, pacing);
    return -ST_JSON_NOT_VALID;
  }

  /* parse udp port */
  int start_port = json_object_get_int(json_object_object_get(video_obj, "start_port"));
  if (start_port < 0 || start_port > 65535) {
    err("%s, invalid start port %d\n", __func__, start_port);
    return -ST_JSON_NOT_VALID;
  }
  video->udp_port = start_port + idx;

  /* parse tr offset */
  const char* tr_offset =
      json_object_get_string(json_object_object_get(video_obj, "tr_offset"));
  CHECK_STRING(tr_offset);
  if (strcmp(tr_offset, "default") == 0) {
    video->tr_offset = TR_OFFSET_DEFAULT;
  } else if (strcmp(pacing, "none") == 0) {
    video->tr_offset = TR_OFFSET_NONE;
  } else {
    err("%s, invalid video tr_offset %s\n", __func__, tr_offset);
    return -ST_JSON_NOT_VALID;
  }

  /* parse video format */
  const char* video_format =
      json_object_get_string(json_object_object_get(video_obj, "video_format"));
  CHECK_STRING(video_format);
  if (strcmp(video_format, "i1080p59") == 0) {
    video->video_format = VIDEO_FORMAT_1080P_59FPS;
  } else if (strcmp(video_format, "i1080p50") == 0) {
    video->video_format = VIDEO_FORMAT_1080P_50FPS;
  } else if (strcmp(video_format, "i1080p29") == 0) {
    video->video_format = VIDEO_FORMAT_1080P_29FPS;
  } else if (strcmp(video_format, "i2160p59") == 0) {
    video->video_format = VIDEO_FORMAT_2160P_59FPS;
  } else if (strcmp(video_format, "i2160p50") == 0) {
    video->video_format = VIDEO_FORMAT_2160P_50FPS;
  } else if (strcmp(video_format, "i2160p29") == 0) {
    video->video_format = VIDEO_FORMAT_2160P_29FPS;
  } else if (strcmp(video_format, "i720p59") == 0) {
    video->video_format = VIDEO_FORMAT_720P_59FPS;
  } else if (strcmp(video_format, "i720p50") == 0) {
    video->video_format = VIDEO_FORMAT_720P_50FPS;
  } else if (strcmp(video_format, "i720p29") == 0) {
    video->video_format = VIDEO_FORMAT_720P_29FPS;
  } else {
    err("%s, invalid video format %s\n", __func__, video_format);
    return -ST_JSON_NOT_VALID;
  }

  /* parse pixel group format */
  const char* pg_format =
      json_object_get_string(json_object_object_get(video_obj, "pg_format"));
  CHECK_STRING(pg_format);
  if (strcmp(pg_format, "YUV_422_10bit") == 0) {
    video->pg_format = ST20_FMT_YUV_422_10BIT;
  } else if (strcmp(pg_format, "YUV_422_8bit") == 0) {
    video->pg_format = ST20_FMT_YUV_422_8BIT;
  } else if (strcmp(pg_format, "YUV_422_12bit") == 0) {
    video->pg_format = ST20_FMT_YUV_422_12BIT;
  } else if (strcmp(pg_format, "YUV_422_16bit") == 0) {
    video->pg_format = ST20_FMT_YUV_422_16BIT;
  } else if (strcmp(pg_format, "YUV_420_8bit") == 0) {
    video->pg_format = ST20_FMT_YUV_420_8BIT;
  } else if (strcmp(pg_format, "YUV_420_10bit") == 0) {
    video->pg_format = ST20_FMT_YUV_420_10BIT;
  } else if (strcmp(pg_format, "YUV_420_12bit") == 0) {
    video->pg_format = ST20_FMT_YUV_420_12BIT;
  } else if (strcmp(pg_format, "RGB_8bit") == 0) {
    video->pg_format = ST20_FMT_RGB_8BIT;
  } else if (strcmp(pg_format, "RGB_10bit") == 0) {
    video->pg_format = ST20_FMT_RGB_10BIT;
  } else if (strcmp(pg_format, "RGB_12bit") == 0) {
    video->pg_format = ST20_FMT_RGB_12BIT;
  } else if (strcmp(pg_format, "RGB_16bit") == 0) {
    video->pg_format = ST20_FMT_RGB_16BIT;
  } else {
    err("%s, invalid pixel group format %s\n", __func__, pg_format);
    return -ST_JSON_NOT_VALID;
  }

  /* parse display option */
  video->display = json_object_get_boolean(json_object_object_get(video_obj, "display"));

  return ST_JSON_SUCCESS;
}

static int st_json_parse_rx_audio(int idx, json_object* audio_obj,
                                  st_json_rx_audio_session_t* audio) {
  if (audio_obj == NULL || audio == NULL) {
    err("%s, can not parse rx audio session\n", __func__);
    return -ST_JSON_NULL;
  }

  /* parse audio type */
  const char* type = json_object_get_string(json_object_object_get(audio_obj, "type"));
  CHECK_STRING(type);
  if (strcmp(type, "frame") == 0) {
    audio->type = ST30_TYPE_FRAME_LEVEL;
  } else if (strcmp(type, "rtp") == 0) {
    audio->type = ST30_TYPE_RTP_LEVEL;
  } else {
    err("%s, invalid audio type %s\n", __func__, type);
    return -ST_JSON_NOT_VALID;
  }

  /* parse audio format */
  const char* audio_format =
      json_object_get_string(json_object_object_get(audio_obj, "audio_format"));
  CHECK_STRING(audio_format);
  if (strcmp(audio_format, "PCM8") == 0) {
    audio->audio_format = ST30_FMT_PCM8;
  } else if (strcmp(audio_format, "PCM16") == 0) {
    audio->audio_format = ST30_FMT_PCM16;
  } else if (strcmp(audio_format, "PCM24") == 0) {
    audio->audio_format = ST30_FMT_PCM24;
  } else {
    err("%s, invalid audio format %s\n", __func__, audio_format);
    return -ST_JSON_NOT_VALID;
  }

  /* parse audio channel */
  const char* audio_channel =
      json_object_get_string(json_object_object_get(audio_obj, "audio_channel"));
  CHECK_STRING(audio_channel);
  if (strcmp(audio_channel, "mono") == 0) {
    audio->audio_channel = ST30_CHAN_MONO;
  } else if (strcmp(audio_channel, "stereo") == 0) {
    audio->audio_channel = ST30_CHAN_STEREO;
  } else {
    err("%s, invalid audio channel %s\n", __func__, audio_channel);
    return -ST_JSON_NOT_VALID;
  }

  /* parse audio sampling */
  const char* audio_sampling =
      json_object_get_string(json_object_object_get(audio_obj, "audio_sampling"));
  CHECK_STRING(audio_sampling);
  if (strcmp(audio_sampling, "48kHz") == 0) {
    audio->audio_sampling = ST30_SAMPLING_48K;
  } else if (strcmp(audio_sampling, "96kHz") == 0) {
    audio->audio_sampling = ST30_SAMPLING_96K;
  } else {
    err("%s, invalid audio sampling %s\n", __func__, audio_sampling);
    return -ST_JSON_NOT_VALID;
  }

  /* parse audio frame time (ms) */
  int frametime_ms =
      json_object_get_int(json_object_object_get(audio_obj, "audio_frametime_ms"));
  if (frametime_ms < 0) {
    err("%s, invalid audio frame time %d\n", __func__, frametime_ms);
    return -ST_JSON_NOT_VALID;
  }
  audio->audio_frametime_ms = frametime_ms;

  /* parse udp port */
  int start_port = json_object_get_int(json_object_object_get(audio_obj, "start_port"));
  if (start_port < 0 || start_port > 65535) {
    err("%s, invalid start port %d\n", __func__, start_port);
    return -ST_JSON_NOT_VALID;
  }
  audio->udp_port = start_port + idx;

  /* parse audio url */
  const char* audio_url =
      json_object_get_string(json_object_object_get(audio_obj, "audio_url"));
  CHECK_STRING(audio_url);
  snprintf(audio->audio_url, sizeof(audio->audio_url), "%s", audio_url);

  return ST_JSON_SUCCESS;
}

static int st_json_parse_rx_anc(int idx, json_object* anc_obj,
                                st_json_rx_ancillary_session_t* anc) {
  if (anc_obj == NULL || anc == NULL) {
    err("%s, can not parse rx anc session\n", __func__);
    return -ST_JSON_NULL;
  }

  /* parse udp port */
  int start_port = json_object_get_int(json_object_object_get(anc_obj, "start_port"));
  if (start_port < 0 || start_port > 65535) {
    err("%s, invalid start port %d\n", __func__, start_port);
    return -ST_JSON_NOT_VALID;
  }
  anc->udp_port = start_port + idx;

  return ST_JSON_SUCCESS;
}

int st_app_parse_json(st_json_context_t* ctx, const char* filename) {
  info("%s, using json-c version: %s\n", __func__, json_c_version());
  int ret = ST_JSON_SUCCESS;

  json_object* root_object = json_object_from_file(filename);
  if (root_object == NULL) {
    err("%s, can not parse json file %s, err: %s\n", __func__, filename,
        json_util_get_last_err());
    return -ST_JSON_PARSE_FAIL;
  }

  /* parse interfaces for system */
  json_object* interfaces_array = json_object_object_get(root_object, "interfaces");
  if (interfaces_array == NULL ||
      json_object_get_type(interfaces_array) != json_type_array) {
    err("%s, can not parse interfaces\n", __func__);
    ret = -ST_JSON_PARSE_FAIL;
    goto exit;
  }
  int num_interfaces = json_object_array_length(interfaces_array);
  for (int i = 0; i < num_interfaces; ++i) {
    ret = st_json_parse_interfaces(json_object_array_get_idx(interfaces_array, i),
                                   &ctx->interfaces[i]);
    if (ret) goto exit;
  }
  ctx->num_interfaces = num_interfaces;

  /* parse tx sessions  */
  json_object* tx_group_array = json_object_object_get(root_object, "tx_sessions");
  if (tx_group_array != NULL && json_object_get_type(tx_group_array) == json_type_array) {
    int num_inf = 0;
    int num_video = 0;
    int num_audio = 0;
    int num_anc = 0;

    for (int i = 0; i < json_object_array_length(tx_group_array); ++i) {
      json_object* tx_group = json_object_array_get_idx(tx_group_array, i);
      if (tx_group == NULL) {
        err("%s, can not parse tx session group\n", __func__);
        ret = -ST_JSON_PARSE_FAIL;
        goto exit;
      }

      /* parse destination ip */
      json_object* dip_p = NULL;
      json_object* dip_r = NULL;
      json_object* dip_array = json_object_object_get(tx_group, "dip");
      if (dip_array != NULL && json_object_get_type(dip_array) == json_type_array) {
        int len = json_object_array_length(dip_array);
        if (len < 1 || len > ST_PORT_MAX) {
          err("%s, wrong dip number\n", __func__);
          ret = -ST_JSON_NOT_VALID;
          goto exit;
        }
        dip_p = json_object_array_get_idx(dip_array, 0);
        if (len == 2) {
          dip_r = json_object_array_get_idx(dip_array, 1);
        }
        num_inf = len;
      } else {
        err("%s, can not parse dip_array\n", __func__);
        ret = -ST_JSON_PARSE_FAIL;
        goto exit;
      }

      /* parse interface */
      int inf_p, inf_r = 0;
      json_object* interface_array = json_object_object_get(tx_group, "interface");
      if (interface_array != NULL &&
          json_object_get_type(interface_array) == json_type_array) {
        int len = json_object_array_length(interface_array);
        if (len != num_inf) {
          err("%s, wrong interface number\n", __func__);
          ret = -ST_JSON_NOT_VALID;
          goto exit;
        }
        inf_p = json_object_get_int(json_object_array_get_idx(interface_array, 0));
        if (inf_p < 0 || inf_p > num_interfaces) {
          err("%s, wrong interface index\n", __func__);
          ret = -ST_JSON_NOT_VALID;
          goto exit;
        }
        if (len == 2) {
          inf_r = json_object_get_int(json_object_array_get_idx(interface_array, 1));
          if (inf_r < 0 || inf_r > num_interfaces) {
            err("%s, wrong interface index\n", __func__);
            ret = -ST_JSON_NOT_VALID;
            goto exit;
          }
        }
      } else {
        err("%s, can not parse interface_array\n", __func__);
        ret = -ST_JSON_PARSE_FAIL;
        goto exit;
      }

      /* parse tx video sessions */
      json_object* video_array = json_object_object_get(tx_group, "video");
      if (video_array != NULL && json_object_get_type(video_array) == json_type_array) {
        for (int j = 0; j < json_object_array_length(video_array); ++j) {
          json_object* video_session = json_object_array_get_idx(video_array, 0);
          int replicas =
              json_object_get_int(json_object_object_get(video_session, "replicas"));
          if (replicas < 0) {
            err("%s, invalid replicas number: %d\n", __func__, replicas);
            ret = -ST_JSON_NOT_VALID;
            goto exit;
          }
          for (int k = 0; k < replicas; ++k) {
            inet_pton(AF_INET, json_object_get_string(dip_p),
                      ctx->tx_video[num_video].dip[0]);
            ctx->tx_video[num_video].inf[0] = &ctx->interfaces[inf_p];
            if (num_inf == 2) {
              inet_pton(AF_INET, json_object_get_string(dip_r),
                        ctx->tx_video[num_video].dip[1]);
              ctx->tx_video[num_video].inf[1] = &ctx->interfaces[inf_r];
            }
            ctx->tx_video[num_video].num_inf = num_inf;
            ret = st_json_parse_tx_video(k, video_session, &ctx->tx_video[num_video]);
            if (ret) goto exit;
            num_video++;
          }
        }
      }

      /* parse tx audio sessions */
      json_object* audio_array = json_object_object_get(tx_group, "audio");
      if (audio_array != NULL && json_object_get_type(audio_array) == json_type_array) {
        for (int j = 0; j < json_object_array_length(audio_array); ++j) {
          json_object* audio_session = json_object_array_get_idx(audio_array, j);
          int replicas =
              json_object_get_int(json_object_object_get(audio_session, "replicas"));
          if (replicas < 0) {
            err("%s, invalid replicas number: %d\n", __func__, replicas);
            ret = -ST_JSON_NOT_VALID;
            goto exit;
          }
          for (int k = 0; k < replicas; ++k) {
            inet_pton(AF_INET, json_object_get_string(dip_p),
                      ctx->tx_audio[num_audio].dip[0]);
            ctx->tx_audio[num_audio].inf[0] = &ctx->interfaces[inf_p];
            if (num_inf == 2) {
              inet_pton(AF_INET, json_object_get_string(dip_r),
                        ctx->tx_audio[num_audio].dip[1]);
              ctx->tx_audio[num_audio].inf[1] = &ctx->interfaces[inf_r];
            }
            ctx->tx_audio[num_audio].num_inf = num_inf;
            ret = st_json_parse_tx_audio(k, audio_session, &ctx->tx_audio[num_audio]);
            if (ret) goto exit;
            num_audio++;
          }
        }
      }

      /* parse tx ancillary sessions */
      json_object* anc_array = json_object_object_get(tx_group, "ancillary");
      if (anc_array != NULL && json_object_get_type(anc_array) == json_type_array) {
        for (int j = 0; j < json_object_array_length(anc_array); ++j) {
          json_object* anc_session = json_object_array_get_idx(anc_array, j);
          int replicas =
              json_object_get_int(json_object_object_get(anc_session, "replicas"));
          if (replicas < 0) {
            err("%s, invalid replicas number: %d\n", __func__, replicas);
            ret = -ST_JSON_NOT_VALID;
            goto exit;
          }
          for (int k = 0; k < replicas; ++k) {
            inet_pton(AF_INET, json_object_get_string(dip_p),
                      ctx->tx_anc[num_anc].dip[0]);
            ctx->tx_anc[num_anc].inf[0] = &ctx->interfaces[inf_p];
            if (num_inf == 2) {
              inet_pton(AF_INET, json_object_get_string(dip_r),
                        ctx->tx_anc[num_anc].dip[1]);
              ctx->tx_anc[num_anc].inf[1] = &ctx->interfaces[inf_r];
            }
            ctx->tx_anc[num_anc].num_inf = num_inf;
            ret = st_json_parse_tx_anc(k, anc_session, &ctx->tx_anc[num_anc]);
            if (ret) goto exit;
            num_anc++;
          }
        }
      }
    }

    ctx->tx_video_session_cnt = num_video;
    ctx->tx_audio_session_cnt = num_audio;
    ctx->tx_anc_session_cnt = num_anc;
  }

  /* parse rx sessions */
  json_object* rx_group_array = json_object_object_get(root_object, "rx_sessions");
  if (rx_group_array != NULL && json_object_get_type(rx_group_array) == json_type_array) {
    int num_inf = 0;
    int num_video = 0;
    int num_audio = 0;
    int num_anc = 0;

    for (int i = 0; i < json_object_array_length(rx_group_array); ++i) {
      json_object* rx_group = json_object_array_get_idx(rx_group_array, i);
      if (rx_group == NULL) {
        err("%s, can not parse rx session group\n", __func__);
        ret = -ST_JSON_PARSE_FAIL;
        goto exit;
      }

      /* parse receiving ip */
      json_object* ip_p = NULL;
      json_object* ip_r = NULL;
      json_object* ip_array = json_object_object_get(rx_group, "ip");
      if (ip_array != NULL && json_object_get_type(ip_array) == json_type_array) {
        int len = json_object_array_length(ip_array);
        if (len < 1 || len > ST_PORT_MAX) {
          err("%s, wrong dip number\n", __func__);
          ret = -ST_JSON_NOT_VALID;
          goto exit;
        }
        ip_p = json_object_array_get_idx(ip_array, 0);
        if (len == 2) {
          ip_r = json_object_array_get_idx(ip_array, 1);
        }
        num_inf = len;
      } else {
        err("%s, can not parse dip_array\n", __func__);
        ret = -ST_JSON_PARSE_FAIL;
        goto exit;
      }

      /* parse interface */
      int inf_p, inf_r = 0;
      json_object* interface_array = json_object_object_get(rx_group, "interface");
      if (interface_array != NULL &&
          json_object_get_type(interface_array) == json_type_array) {
        int len = json_object_array_length(interface_array);
        if (len != num_inf) {
          err("%s, wrong interface number\n", __func__);
          ret = -ST_JSON_NOT_VALID;
          goto exit;
        }
        inf_p = json_object_get_int(json_object_array_get_idx(interface_array, 0));
        if (inf_p < 0 || inf_p > num_interfaces) {
          err("%s, wrong interface index\n", __func__);
          ret = -ST_JSON_NOT_VALID;
          goto exit;
        }
        if (len == 2) {
          inf_r = json_object_get_int(json_object_array_get_idx(interface_array, 1));
          if (inf_r < 0 || inf_r > num_interfaces) {
            err("%s, wrong interface index\n", __func__);
            ret = -ST_JSON_NOT_VALID;
            goto exit;
          }
        }
      } else {
        err("%s, can not parse interface_array\n", __func__);
        ret = -ST_JSON_PARSE_FAIL;
        goto exit;
      }

      /* parse rx video sessions */
      json_object* video_array = json_object_object_get(rx_group, "video");
      if (video_array != NULL && json_object_get_type(video_array) == json_type_array) {
        for (int j = 0; j < json_object_array_length(video_array); ++j) {
          json_object* video_session = json_object_array_get_idx(video_array, 0);
          int replicas =
              json_object_get_int(json_object_object_get(video_session, "replicas"));
          if (replicas < 0) {
            err("%s, invalid replicas number: %d\n", __func__, replicas);
            ret = -ST_JSON_NOT_VALID;
            goto exit;
          }
          for (int k = 0; k < replicas; ++k) {
            inet_pton(AF_INET, json_object_get_string(ip_p),
                      ctx->rx_video[num_video].ip[0]);
            ctx->rx_video[num_video].inf[0] = &ctx->interfaces[inf_p];
            if (num_inf == 2) {
              inet_pton(AF_INET, json_object_get_string(ip_r),
                        ctx->rx_video[num_video].ip[1]);
              ctx->rx_video[num_video].inf[1] = &ctx->interfaces[inf_r];
            }
            ctx->rx_video[num_video].num_inf = num_inf;
            ret = st_json_parse_rx_video(k, video_session, &ctx->rx_video[num_video]);
            if (ret) goto exit;
            num_video++;
          }
        }
      }

      /* parse rx audio sessions */
      json_object* audio_array = json_object_object_get(rx_group, "audio");
      if (audio_array != NULL && json_object_get_type(audio_array) == json_type_array) {
        for (int j = 0; j < json_object_array_length(audio_array); ++j) {
          json_object* audio_session = json_object_array_get_idx(audio_array, j);
          int replicas =
              json_object_get_int(json_object_object_get(audio_session, "replicas"));
          if (replicas < 0) {
            err("%s, invalid replicas number: %d\n", __func__, replicas);
            ret = -ST_JSON_NOT_VALID;
            goto exit;
          }
          for (int k = 0; k < replicas; ++k) {
            inet_pton(AF_INET, json_object_get_string(ip_p),
                      ctx->rx_audio[num_audio].ip[0]);
            ctx->rx_audio[num_audio].inf[0] = &ctx->interfaces[inf_p];
            if (num_inf == 2) {
              inet_pton(AF_INET, json_object_get_string(ip_r),
                        ctx->rx_audio[num_audio].ip[1]);
              ctx->rx_audio[num_audio].inf[1] = &ctx->interfaces[inf_r];
            }
            ctx->rx_audio[num_audio].num_inf = num_inf;
            ret = st_json_parse_rx_audio(k, audio_session, &ctx->rx_audio[num_audio]);
            if (ret) goto exit;
            num_audio++;
          }
        }
      }

      /* parse rx ancillary sessions */
      json_object* anc_array = json_object_object_get(rx_group, "ancillary");
      if (anc_array != NULL && json_object_get_type(anc_array) == json_type_array) {
        for (int j = 0; j < json_object_array_length(anc_array); ++j) {
          json_object* anc_session = json_object_array_get_idx(anc_array, j);
          int replicas =
              json_object_get_int(json_object_object_get(anc_session, "replicas"));
          if (replicas < 0) {
            err("%s, invalid replicas number: %d\n", __func__, replicas);
            ret = -ST_JSON_NOT_VALID;
            goto exit;
          }
          for (int k = 0; k < replicas; ++k) {
            inet_pton(AF_INET, json_object_get_string(ip_p), ctx->rx_anc[num_anc].ip[0]);
            ctx->rx_anc[num_anc].inf[0] = &ctx->interfaces[inf_p];
            if (num_inf == 2) {
              inet_pton(AF_INET, json_object_get_string(ip_r),
                        ctx->rx_anc[num_anc].ip[1]);
              ctx->rx_anc[num_anc].inf[1] = &ctx->interfaces[inf_r];
            }
            ctx->rx_anc[num_anc].num_inf = num_inf;
            ret = st_json_parse_rx_anc(k, anc_session, &ctx->rx_anc[num_anc]);
            if (ret) goto exit;
            num_anc++;
          }
        }
      }
    }

    ctx->rx_video_session_cnt = num_video;
    ctx->rx_audio_session_cnt = num_audio;
    ctx->rx_anc_session_cnt = num_anc;
  }

exit:
  json_object_put(root_object);
  return ret;
}

enum st_fps st_app_get_fps(enum video_format fmt) {
  switch (fmt) {
    case VIDEO_FORMAT_720P_59FPS:
    case VIDEO_FORMAT_1080P_59FPS:
    case VIDEO_FORMAT_2160P_59FPS:
      return ST_FPS_P59_94;
    case VIDEO_FORMAT_720P_50FPS:
    case VIDEO_FORMAT_1080P_50FPS:
    case VIDEO_FORMAT_2160P_50FPS:
      return ST_FPS_P50;
    case VIDEO_FORMAT_720P_29FPS:
    case VIDEO_FORMAT_1080P_29FPS:
    case VIDEO_FORMAT_2160P_29FPS:
      return ST_FPS_P29_97;
    default: {
      err("%s, invalid video fmt %d\n", __func__, fmt);
      return ST_FPS_P59_94;
    }
  }
}
int st_app_get_width(enum video_format fmt) {
  switch (fmt) {
    case VIDEO_FORMAT_720P_59FPS:
    case VIDEO_FORMAT_720P_50FPS:
    case VIDEO_FORMAT_720P_29FPS:
      return 1280;
    case VIDEO_FORMAT_1080P_59FPS:
    case VIDEO_FORMAT_1080P_50FPS:
    case VIDEO_FORMAT_1080P_29FPS:
      return 1920;
    case VIDEO_FORMAT_2160P_59FPS:
    case VIDEO_FORMAT_2160P_50FPS:
    case VIDEO_FORMAT_2160P_29FPS:
      return 3840;
    default: {
      err("%s, invalid video fmt %d\n", __func__, fmt);
      return 1920;
    }
  }
}
int st_app_get_height(enum video_format fmt) {
  switch (fmt) {
    case VIDEO_FORMAT_720P_59FPS:
    case VIDEO_FORMAT_720P_50FPS:
    case VIDEO_FORMAT_720P_29FPS:
      return 720;
    case VIDEO_FORMAT_1080P_59FPS:
    case VIDEO_FORMAT_1080P_50FPS:
    case VIDEO_FORMAT_1080P_29FPS:
      return 1080;
    case VIDEO_FORMAT_2160P_59FPS:
    case VIDEO_FORMAT_2160P_50FPS:
    case VIDEO_FORMAT_2160P_29FPS:
      return 2160;
    default: {
      err("%s, invalid video fmt %d\n", __func__, fmt);
      return 1080;
    }
  }
}