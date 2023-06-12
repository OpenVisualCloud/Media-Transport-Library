/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "parse_json.h"

#include "app_base.h"
#include "log.h"

#if (JSON_C_VERSION_NUM >= ((0 << 16) | (13 << 8) | 0)) || \
    (JSON_C_VERSION_NUM < ((0 << 16) | (10 << 8) | 0))
static inline json_object* st_json_object_object_get(json_object* obj, const char* key) {
  return json_object_object_get(obj, key);
}
#else
static inline json_object* st_json_object_object_get(json_object* obj, const char* key) {
  json_object* value;
  int ret = json_object_object_get_ex(obj, key, &value);
  if (ret) return value;
  err("%s, can not get object with key: %s!\n", __func__, key);
  return NULL;
}
#endif

static const struct st_video_fmt_desc st_video_fmt_descs[] = {
    {
        .fmt = VIDEO_FORMAT_480I_59FPS,
        .name = "i480i59",
        .width = 720,
        .height = 480,
        .fps = ST_FPS_P59_94,
    },
    {
        .fmt = VIDEO_FORMAT_576I_50FPS,
        .name = "i576i50",
        .width = 720,
        .height = 576,
        .fps = ST_FPS_P50,
    },
    {
        .fmt = VIDEO_FORMAT_720P_119FPS,
        .name = "i720p119",
        .width = 1280,
        .height = 720,
        .fps = ST_FPS_P119_88,
    },
    {
        .fmt = VIDEO_FORMAT_720P_59FPS,
        .name = "i720p59",
        .width = 1280,
        .height = 720,
        .fps = ST_FPS_P59_94,
    },
    {
        .fmt = VIDEO_FORMAT_720P_50FPS,
        .name = "i720p50",
        .width = 1280,
        .height = 720,
        .fps = ST_FPS_P50,
    },
    {
        .fmt = VIDEO_FORMAT_720P_29FPS,
        .name = "i720p29",
        .width = 1280,
        .height = 720,
        .fps = ST_FPS_P29_97,
    },
    {
        .fmt = VIDEO_FORMAT_720P_25FPS,
        .name = "i720p25",
        .width = 1280,
        .height = 720,
        .fps = ST_FPS_P25,
    },
    {
        .fmt = VIDEO_FORMAT_720P_60FPS,
        .name = "i720p60",
        .width = 1280,
        .height = 720,
        .fps = ST_FPS_P60,
    },
    {
        .fmt = VIDEO_FORMAT_720P_30FPS,
        .name = "i720p30",
        .width = 1280,
        .height = 720,
        .fps = ST_FPS_P30,
    },
    {
        .fmt = VIDEO_FORMAT_720P_24FPS,
        .name = "i720p24",
        .width = 1280,
        .height = 720,
        .fps = ST_FPS_P24,
    },
    {
        .fmt = VIDEO_FORMAT_720P_23FPS,
        .name = "i720p23",
        .width = 1280,
        .height = 720,
        .fps = ST_FPS_P23_98,
    },
    {
        .fmt = VIDEO_FORMAT_1080P_119FPS,
        .name = "i1080p119",
        .width = 1920,
        .height = 1080,
        .fps = ST_FPS_P119_88,
    },
    {
        .fmt = VIDEO_FORMAT_1080P_59FPS,
        .name = "i1080p59",
        .width = 1920,
        .height = 1080,
        .fps = ST_FPS_P59_94,
    },
    {
        .fmt = VIDEO_FORMAT_1080P_50FPS,
        .name = "i1080p50",
        .width = 1920,
        .height = 1080,
        .fps = ST_FPS_P50,
    },
    {
        .fmt = VIDEO_FORMAT_1080P_29FPS,
        .name = "i1080p29",
        .width = 1920,
        .height = 1080,
        .fps = ST_FPS_P29_97,
    },
    {
        .fmt = VIDEO_FORMAT_1080P_25FPS,
        .name = "i1080p25",
        .width = 1920,
        .height = 1080,
        .fps = ST_FPS_P25,
    },
    {
        .fmt = VIDEO_FORMAT_1080P_60FPS,
        .name = "i1080p60",
        .width = 1920,
        .height = 1080,
        .fps = ST_FPS_P60,
    },
    {
        .fmt = VIDEO_FORMAT_1080P_30FPS,
        .name = "i1080p30",
        .width = 1920,
        .height = 1080,
        .fps = ST_FPS_P30,
    },
    {
        .fmt = VIDEO_FORMAT_1080P_24FPS,
        .name = "i1080p24",
        .width = 1920,
        .height = 1080,
        .fps = ST_FPS_P24,
    },
    {
        .fmt = VIDEO_FORMAT_1080P_23FPS,
        .name = "i1080p23",
        .width = 1920,
        .height = 1080,
        .fps = ST_FPS_P23_98,
    },
    {
        .fmt = VIDEO_FORMAT_1080I_59FPS,
        .name = "i1080i59",
        .width = 1920,
        .height = 1080,
        .fps = ST_FPS_P59_94,
    },
    {
        .fmt = VIDEO_FORMAT_1080I_50FPS,
        .name = "i1080i50",
        .width = 1920,
        .height = 1080,
        .fps = ST_FPS_P50,
    },
    {
        .fmt = VIDEO_FORMAT_2160P_119FPS,
        .name = "i2160p119",
        .width = 3840,
        .height = 2160,
        .fps = ST_FPS_P119_88,
    },
    {
        .fmt = VIDEO_FORMAT_2160P_59FPS,
        .name = "i2160p59",
        .width = 3840,
        .height = 2160,
        .fps = ST_FPS_P59_94,
    },
    {
        .fmt = VIDEO_FORMAT_2160P_50FPS,
        .name = "i2160p50",
        .width = 3840,
        .height = 2160,
        .fps = ST_FPS_P50,
    },
    {
        .fmt = VIDEO_FORMAT_2160P_29FPS,
        .name = "i2160p29",
        .width = 3840,
        .height = 2160,
        .fps = ST_FPS_P29_97,
    },
    {
        .fmt = VIDEO_FORMAT_2160P_25FPS,
        .name = "i2160p25",
        .width = 3840,
        .height = 2160,
        .fps = ST_FPS_P25,
    },
    {
        .fmt = VIDEO_FORMAT_2160P_60FPS,
        .name = "i2160p60",
        .width = 3840,
        .height = 2160,
        .fps = ST_FPS_P60,
    },
    {
        .fmt = VIDEO_FORMAT_2160P_30FPS,
        .name = "i2160p30",
        .width = 3840,
        .height = 2160,
        .fps = ST_FPS_P30,
    },
    {
        .fmt = VIDEO_FORMAT_2160P_24FPS,
        .name = "i2160p24",
        .width = 3840,
        .height = 2160,
        .fps = ST_FPS_P24,
    },
    {
        .fmt = VIDEO_FORMAT_2160P_23FPS,
        .name = "i2160p23",
        .width = 3840,
        .height = 2160,
        .fps = ST_FPS_P23_98,
    },
    {
        .fmt = VIDEO_FORMAT_4320P_119FPS,
        .name = "i4320p119",
        .width = 7680,
        .height = 4320,
        .fps = ST_FPS_P119_88,
    },
    {
        .fmt = VIDEO_FORMAT_4320P_59FPS,
        .name = "i4320p59",
        .width = 7680,
        .height = 4320,
        .fps = ST_FPS_P59_94,
    },
    {
        .fmt = VIDEO_FORMAT_4320P_50FPS,
        .name = "i4320p50",
        .width = 7680,
        .height = 4320,
        .fps = ST_FPS_P50,
    },
    {
        .fmt = VIDEO_FORMAT_4320P_29FPS,
        .name = "i4320p29",
        .width = 7680,
        .height = 4320,
        .fps = ST_FPS_P29_97,
    },
    {
        .fmt = VIDEO_FORMAT_4320P_25FPS,
        .name = "i4320p25",
        .width = 7680,
        .height = 4320,
        .fps = ST_FPS_P25,
    },
    {
        .fmt = VIDEO_FORMAT_4320P_60FPS,
        .name = "i4320p60",
        .width = 7680,
        .height = 4320,
        .fps = ST_FPS_P60,
    },
    {
        .fmt = VIDEO_FORMAT_4320P_30FPS,
        .name = "i4320p30",
        .width = 7680,
        .height = 4320,
        .fps = ST_FPS_P30,
    },
    {
        .fmt = VIDEO_FORMAT_4320P_24FPS,
        .name = "i4320p24",
        .width = 7680,
        .height = 4320,
        .fps = ST_FPS_P24,
    },
    {
        .fmt = VIDEO_FORMAT_4320P_23FPS,
        .name = "i4320p23",
        .width = 7680,
        .height = 4320,
        .fps = ST_FPS_P23_98,
    },
    {
        .fmt = VIDEO_FORMAT_DCI1080P_119FPS,
        .name = "idci1080p119",
        .width = 2048,
        .height = 1080,
        .fps = ST_FPS_P119_88,
    },
    {
        .fmt = VIDEO_FORMAT_DCI1080P_60FPS,
        .name = "idci1080p60",
        .width = 2048,
        .height = 1080,
        .fps = ST_FPS_P60,
    },
    {
        .fmt = VIDEO_FORMAT_DCI1080P_59FPS,
        .name = "idci1080p59",
        .width = 2048,
        .height = 1080,
        .fps = ST_FPS_P59_94,
    },
    {
        .fmt = VIDEO_FORMAT_DCI1080P_50FPS,
        .name = "idci1080p50",
        .width = 2048,
        .height = 1080,
        .fps = ST_FPS_P50,
    },
    {
        .fmt = VIDEO_FORMAT_DCI1080P_30FPS,
        .name = "idci1080p30",
        .width = 2048,
        .height = 1080,
        .fps = ST_FPS_P30,
    },
    {
        .fmt = VIDEO_FORMAT_DCI1080P_29FPS,
        .name = "idci1080p29",
        .width = 2048,
        .height = 1080,
        .fps = ST_FPS_P29_97,
    },
    {
        .fmt = VIDEO_FORMAT_DCI1080P_25FPS,
        .name = "idci1080p25",
        .width = 2048,
        .height = 1080,
        .fps = ST_FPS_P25,
    },
    {
        .fmt = VIDEO_FORMAT_DCI1080P_24FPS,
        .name = "idci1080p24",
        .width = 2048,
        .height = 1080,
        .fps = ST_FPS_P24,
    },
    {
        .fmt = VIDEO_FORMAT_DCI1080P_23FPS,
        .name = "idci1080p23",
        .width = 2048,
        .height = 1080,
        .fps = ST_FPS_P23_98,
    },
    {
        .fmt = VIDEO_FORMAT_DCI2160P_119FPS,
        .name = "idci2160p119",
        .width = 4096,
        .height = 2160,
        .fps = ST_FPS_P119_88,
    },
    {
        .fmt = VIDEO_FORMAT_DCI2160P_60FPS,
        .name = "idci2160p60",
        .width = 4096,
        .height = 2160,
        .fps = ST_FPS_P60,
    },
    {
        .fmt = VIDEO_FORMAT_DCI2160P_59FPS,
        .name = "idci2160p59",
        .width = 4096,
        .height = 2160,
        .fps = ST_FPS_P59_94,
    },
    {
        .fmt = VIDEO_FORMAT_DCI2160P_50FPS,
        .name = "idci2160p50",
        .width = 4096,
        .height = 2160,
        .fps = ST_FPS_P50,
    },
    {
        .fmt = VIDEO_FORMAT_DCI2160P_30FPS,
        .name = "idci2160p30",
        .width = 4096,
        .height = 2160,
        .fps = ST_FPS_P30,
    },
    {
        .fmt = VIDEO_FORMAT_DCI2160P_29FPS,
        .name = "idci2160p29",
        .width = 4096,
        .height = 2160,
        .fps = ST_FPS_P29_97,
    },
    {
        .fmt = VIDEO_FORMAT_DCI2160P_25FPS,
        .name = "idci2160p25",
        .width = 4096,
        .height = 2160,
        .fps = ST_FPS_P25,
    },
    {
        .fmt = VIDEO_FORMAT_DCI2160P_24FPS,
        .name = "idci2160p24",
        .width = 4096,
        .height = 2160,
        .fps = ST_FPS_P24,
    },
    {
        .fmt = VIDEO_FORMAT_DCI2160P_23FPS,
        .name = "idci2160p23",
        .width = 4096,
        .height = 2160,
        .fps = ST_FPS_P23_98,
    },
    {
        .fmt = VIDEO_FORMAT_AUTO,
        .name = "auto",
        .width = 1920,
        .height = 1080,
        .fps = ST_FPS_P59_94,
    },
};

#define VNAME(name) (#name)

#define REQUIRED_ITEM(string)                                 \
  do {                                                        \
    if (string == NULL) {                                     \
      err("%s, can not parse %s\n", __func__, VNAME(string)); \
      return -ST_JSON_PARSE_FAIL;                             \
    }                                                         \
  } while (0)

/* 7 bits payload type define in RFC3550 */
static inline bool st_json_is_valid_payload_type(int payload_type) {
  if (payload_type > 0 && payload_type < 0x7F)
    return true;
  else
    return false;
}

static int st_json_parse_interfaces(json_object* interface_obj,
                                    st_json_interface_t* interface) {
  if (interface_obj == NULL || interface == NULL) {
    err("%s, can not parse interfaces!\n", __func__);
    return -ST_JSON_NULL;
  }

  const char* name =
      json_object_get_string(st_json_object_object_get(interface_obj, "name"));
  REQUIRED_ITEM(name);
  snprintf(interface->name, sizeof(interface->name), "%s", name);

  const char* ip = json_object_get_string(st_json_object_object_get(interface_obj, "ip"));
  if (ip) inet_pton(AF_INET, ip, interface->ip_addr);

  json_object* obj = st_json_object_object_get(interface_obj, "netmask");
  if (obj) {
    inet_pton(AF_INET, json_object_get_string(obj), interface->netmask);
  }
  obj = st_json_object_object_get(interface_obj, "gateway");
  if (obj) {
    inet_pton(AF_INET, json_object_get_string(obj), interface->gateway);
  }
  obj = st_json_object_object_get(interface_obj, "proto");
  if (obj) {
    const char* proto = json_object_get_string(obj);
    if (strcmp(proto, "dhcp") == 0) {
      interface->net_proto = MTL_PROTO_DHCP;
    } else if (strcmp(proto, "static") == 0) {
      interface->net_proto = MTL_PROTO_STATIC;
    } else {
      err("%s, invalid network proto %s\n", __func__, proto);
      return -ST_JSON_NOT_VALID;
    }
  }

  return ST_JSON_SUCCESS;
}

static int parse_base_udp_port(json_object* obj, st_json_session_base_t* base, int idx) {
  int start_port = json_object_get_int(st_json_object_object_get(obj, "start_port"));
  if (start_port <= 0 || start_port > 65535) {
    err("%s, invalid start port %d\n", __func__, start_port);
    return -ST_JSON_NOT_VALID;
  }
  base->udp_port = start_port + idx;

  return ST_JSON_SUCCESS;
}

static int parse_base_payload_type(json_object* obj, st_json_session_base_t* base) {
  json_object* payload_type_object = st_json_object_object_get(obj, "payload_type");
  if (payload_type_object) {
    base->payload_type = json_object_get_int(payload_type_object);
    if (!st_json_is_valid_payload_type(base->payload_type)) {
      err("%s, invalid payload type %d\n", __func__, base->payload_type);
      return -ST_JSON_NOT_VALID;
    }
  } else {
    return -ST_JSON_NULL;
  }

  return ST_JSON_SUCCESS;
}

static int parse_video_type(json_object* video_obj, st_json_video_session_t* video) {
  const char* type = json_object_get_string(st_json_object_object_get(video_obj, "type"));
  REQUIRED_ITEM(type);
  if (strcmp(type, "frame") == 0) {
    video->info.type = ST20_TYPE_FRAME_LEVEL;
  } else if (strcmp(type, "rtp") == 0) {
    video->info.type = ST20_TYPE_RTP_LEVEL;
  } else if (strcmp(type, "slice") == 0) {
    video->info.type = ST20_TYPE_SLICE_LEVEL;
  } else {
    err("%s, invalid video type %s\n", __func__, type);
    return -ST_JSON_NOT_VALID;
  }
  return ST_JSON_SUCCESS;
}

static int parse_video_pacing(json_object* video_obj, st_json_video_session_t* video) {
  const char* pacing =
      json_object_get_string(st_json_object_object_get(video_obj, "pacing"));
  REQUIRED_ITEM(pacing);
  if (strcmp(pacing, "gap") == 0) {
    video->info.pacing = PACING_GAP;
  } else if (strcmp(pacing, "linear") == 0) {
    video->info.pacing = PACING_LINEAR;
  } else {
    err("%s, invalid video pacing %s\n", __func__, pacing);
    return -ST_JSON_NOT_VALID;
  }
  return ST_JSON_SUCCESS;
}

static int parse_video_packing(json_object* video_obj, st_json_video_session_t* video) {
  const char* packing =
      json_object_get_string(st_json_object_object_get(video_obj, "packing"));
  if (packing) {
    if (strcmp(packing, "GPM_SL") == 0) {
      video->info.packing = ST20_PACKING_GPM_SL;
    } else if (strcmp(packing, "BPM") == 0) {
      video->info.packing = ST20_PACKING_BPM;
    } else if (strcmp(packing, "GPM") == 0) {
      video->info.packing = ST20_PACKING_GPM;
    } else {
      err("%s, invalid video packing mode %s\n", __func__, packing);
      return -ST_JSON_NOT_VALID;
    }
  } else { /* default set to bpm */
    video->info.packing = ST20_PACKING_BPM;
  }
  return ST_JSON_SUCCESS;
}

static int parse_video_tr_offset(json_object* video_obj, st_json_video_session_t* video) {
  const char* tr_offset =
      json_object_get_string(st_json_object_object_get(video_obj, "tr_offset"));
  REQUIRED_ITEM(tr_offset);
  if (strcmp(tr_offset, "default") == 0) {
    video->info.tr_offset = TR_OFFSET_DEFAULT;
  } else if (strcmp(tr_offset, "none") == 0) {
    video->info.tr_offset = TR_OFFSET_NONE;
  } else {
    err("%s, invalid video tr_offset %s\n", __func__, tr_offset);
    return -ST_JSON_NOT_VALID;
  }
  return ST_JSON_SUCCESS;
}

static int parse_video_format(json_object* video_obj, st_json_video_session_t* video) {
  const char* video_format =
      json_object_get_string(st_json_object_object_get(video_obj, "video_format"));
  REQUIRED_ITEM(video_format);
  int i;
  for (i = 0; i < ARRAY_SIZE(st_video_fmt_descs); i++) {
    if (strcmp(video_format, st_video_fmt_descs[i].name) == 0) {
      video->info.video_format = st_video_fmt_descs[i].fmt;
      return ST_JSON_SUCCESS;
    }
  }
  err("%s, invalid video format %s\n", __func__, video_format);
  return -ST_JSON_NOT_VALID;
}

static int parse_video_pg_format(json_object* video_obj, st_json_video_session_t* video) {
  const char* pg_format =
      json_object_get_string(st_json_object_object_get(video_obj, "pg_format"));
  REQUIRED_ITEM(pg_format);
  if (strcmp(pg_format, "YUV_422_10bit") == 0) {
    video->info.pg_format = ST20_FMT_YUV_422_10BIT;
  } else if (strcmp(pg_format, "YUV_422_8bit") == 0) {
    video->info.pg_format = ST20_FMT_YUV_422_8BIT;
  } else if (strcmp(pg_format, "YUV_422_12bit") == 0) {
    video->info.pg_format = ST20_FMT_YUV_422_12BIT;
  } else if (strcmp(pg_format, "YUV_422_16bit") == 0) {
    video->info.pg_format = ST20_FMT_YUV_422_16BIT;
  } else if (strcmp(pg_format, "YUV_444_10bit") == 0) {
    video->info.pg_format = ST20_FMT_YUV_444_10BIT;
  } else if (strcmp(pg_format, "YUV_444_12bit") == 0) {
    video->info.pg_format = ST20_FMT_YUV_444_12BIT;
  } else if (strcmp(pg_format, "YUV_420_8bit") == 0) {
    video->info.pg_format = ST20_FMT_YUV_420_8BIT;
  } else if (strcmp(pg_format, "YUV_420_10bit") == 0) {
    video->info.pg_format = ST20_FMT_YUV_420_10BIT;
  } else if (strcmp(pg_format, "YUV_420_12bit") == 0) {
    video->info.pg_format = ST20_FMT_YUV_420_12BIT;
  } else if (strcmp(pg_format, "RGB_8bit") == 0) {
    video->info.pg_format = ST20_FMT_RGB_8BIT;
  } else if (strcmp(pg_format, "RGB_10bit") == 0) {
    video->info.pg_format = ST20_FMT_RGB_10BIT;
  } else if (strcmp(pg_format, "RGB_12bit") == 0) {
    video->info.pg_format = ST20_FMT_RGB_12BIT;
  } else if (strcmp(pg_format, "RGB_16bit") == 0) {
    video->info.pg_format = ST20_FMT_RGB_16BIT;
  } else {
    err("%s, invalid pixel group format %s\n", __func__, pg_format);
    return -ST_JSON_NOT_VALID;
  }
  return ST_JSON_SUCCESS;
}

static int parse_url(json_object* obj, const char* name, char* url) {
  const char* url_src = json_object_get_string(st_json_object_object_get(obj, name));
  REQUIRED_ITEM(url_src);
  snprintf(url, ST_APP_URL_MAX_LEN, "%s", url_src);
  return -ST_JSON_SUCCESS;
}

static int st_json_parse_tx_video(int idx, json_object* video_obj,
                                  st_json_video_session_t* video) {
  if (video_obj == NULL || video == NULL) {
    err("%s, can not parse tx video session\n", __func__);
    return -ST_JSON_NULL;
  }
  int ret;

  /* parse udp port  */
  ret = parse_base_udp_port(video_obj, &video->base, idx);
  if (ret < 0) return ret;

  /* parse payload type */
  ret = parse_base_payload_type(video_obj, &video->base);
  if (ret < 0) {
    err("%s, use default pt %u\n", __func__, ST_APP_PAYLOAD_TYPE_VIDEO);
    video->base.payload_type = ST_APP_PAYLOAD_TYPE_VIDEO;
  }

  /* parse video type */
  ret = parse_video_type(video_obj, video);
  if (ret < 0) return ret;

  /* parse video pacing */
  ret = parse_video_pacing(video_obj, video);
  if (ret < 0) return ret;

  /* parse video packing mode */
  ret = parse_video_packing(video_obj, video);
  if (ret < 0) return ret;

  /* parse tr offset */
  ret = parse_video_tr_offset(video_obj, video);
  if (ret < 0) return ret;

  /* parse video format */
  ret = parse_video_format(video_obj, video);
  if (ret < 0) return ret;

  /* parse pixel group format */
  ret = parse_video_pg_format(video_obj, video);
  if (ret < 0) return ret;

  /* parse video url */
  ret = parse_url(video_obj, "video_url", video->info.video_url);
  if (ret < 0) return ret;

  /* parse display option */
  video->display =
      json_object_get_boolean(st_json_object_object_get(video_obj, "display"));

  return ST_JSON_SUCCESS;
}

static int st_json_parse_rx_video(int idx, json_object* video_obj,
                                  st_json_video_session_t* video) {
  if (video_obj == NULL || video == NULL) {
    err("%s, can not parse rx video session\n", __func__);
    return -ST_JSON_NULL;
  }
  int ret;

  /* parse udp port  */
  ret = parse_base_udp_port(video_obj, &video->base, idx);
  if (ret < 0) return ret;

  /* parse payload type */
  ret = parse_base_payload_type(video_obj, &video->base);
  if (ret < 0) {
    err("%s, use default pt %u\n", __func__, ST_APP_PAYLOAD_TYPE_VIDEO);
    video->base.payload_type = ST_APP_PAYLOAD_TYPE_VIDEO;
  }

  /* parse video type */
  ret = parse_video_type(video_obj, video);
  if (ret < 0) return ret;

  /* parse video pacing */
  ret = parse_video_pacing(video_obj, video);
  if (ret < 0) return ret;

  /* parse tr offset */
  ret = parse_video_tr_offset(video_obj, video);
  if (ret < 0) return ret;

  /* parse video format */
  ret = parse_video_format(video_obj, video);
  if (ret < 0) return ret;

  /* parse pixel group format */
  ret = parse_video_pg_format(video_obj, video);
  if (ret < 0) return ret;

  /* parse user pixel group format */
  video->user_pg_format = USER_FMT_MAX;
  const char* user_pg_format =
      json_object_get_string(st_json_object_object_get(video_obj, "user_pg_format"));
  if (user_pg_format != NULL) {
    if (strcmp(user_pg_format, "YUV_422_8bit") == 0) {
      video->user_pg_format = USER_FMT_YUV_422_8BIT;
    } else {
      err("%s, invalid pixel group format %s\n", __func__, user_pg_format);
      return -ST_JSON_NOT_VALID;
    }
  }

  /* parse display option */
  video->display =
      json_object_get_boolean(st_json_object_object_get(video_obj, "display"));

  /* parse measure_latency option */
  video->measure_latency =
      json_object_get_boolean(st_json_object_object_get(video_obj, "measure_latency"));

  return ST_JSON_SUCCESS;
}

static int parse_audio_type(json_object* audio_obj, st_json_audio_session_t* audio) {
  const char* type = json_object_get_string(st_json_object_object_get(audio_obj, "type"));
  REQUIRED_ITEM(type);
  if (strcmp(type, "frame") == 0) {
    audio->info.type = ST30_TYPE_FRAME_LEVEL;
  } else if (strcmp(type, "rtp") == 0) {
    audio->info.type = ST30_TYPE_RTP_LEVEL;
  } else {
    err("%s, invalid audio type %s\n", __func__, type);
    return -ST_JSON_NOT_VALID;
  }
  return ST_JSON_SUCCESS;
}

static int parse_audio_format(json_object* audio_obj, st_json_audio_session_t* audio) {
  const char* audio_format =
      json_object_get_string(st_json_object_object_get(audio_obj, "audio_format"));
  REQUIRED_ITEM(audio_format);
  if (strcmp(audio_format, "PCM8") == 0) {
    audio->info.audio_format = ST30_FMT_PCM8;
  } else if (strcmp(audio_format, "PCM16") == 0) {
    audio->info.audio_format = ST30_FMT_PCM16;
  } else if (strcmp(audio_format, "PCM24") == 0) {
    audio->info.audio_format = ST30_FMT_PCM24;
  } else if (strcmp(audio_format, "AM824") == 0) {
    audio->info.audio_format = ST31_FMT_AM824;
  } else {
    err("%s, invalid audio format %s\n", __func__, audio_format);
    return -ST_JSON_NOT_VALID;
  }
  return ST_JSON_SUCCESS;
}

static int parse_audio_channel(json_object* audio_obj, st_json_audio_session_t* audio) {
  json_object* audio_channel_array =
      st_json_object_object_get(audio_obj, "audio_channel");
  if (audio_channel_array == NULL ||
      json_object_get_type(audio_channel_array) != json_type_array) {
    err("%s, can not parse audio channel\n", __func__);
    return -ST_JSON_PARSE_FAIL;
  }
  audio->info.audio_channel = 0; /* reset channel number*/
  for (int i = 0; i < json_object_array_length(audio_channel_array); ++i) {
    json_object* channel_obj = json_object_array_get_idx(audio_channel_array, i);
    const char* channel = json_object_get_string(channel_obj);
    REQUIRED_ITEM(channel);
    if (strcmp(channel, "M") == 0) {
      audio->info.audio_channel += 1;
    } else if (strcmp(channel, "DM") == 0 || strcmp(channel, "ST") == 0 ||
               strcmp(channel, "LtRt") == 0 || strcmp(channel, "AES3") == 0) {
      audio->info.audio_channel += 2;
    } else if (strcmp(channel, "51") == 0) {
      audio->info.audio_channel += 6;
    } else if (strcmp(channel, "71") == 0) {
      audio->info.audio_channel += 8;
    } else if (strcmp(channel, "222") == 0) {
      audio->info.audio_channel += 24;
    } else if (strcmp(channel, "SGRP") == 0) {
      audio->info.audio_channel += 4;
    } else if (channel[0] == 'U' && channel[1] >= '0' && channel[1] <= '9' &&
               channel[2] >= '0' && channel[2] <= '9' && channel[3] == '\0') {
      int num_channel = (channel[1] - '0') * 10 + (channel[2] - '0');
      if (num_channel < 1 || num_channel > 64) {
        err("%s, audio undefined channel number out of range %s\n", __func__, channel);
        return -ST_JSON_NOT_VALID;
      }
      audio->info.audio_channel += num_channel;
    } else {
      err("%s, invalid audio channel %s\n", __func__, channel);
      return -ST_JSON_NOT_VALID;
    }
  }
  return ST_JSON_SUCCESS;
}

static int parse_audio_sampling(json_object* audio_obj, st_json_audio_session_t* audio) {
  const char* audio_sampling =
      json_object_get_string(st_json_object_object_get(audio_obj, "audio_sampling"));
  REQUIRED_ITEM(audio_sampling);
  if (strcmp(audio_sampling, "48kHz") == 0) {
    audio->info.audio_sampling = ST30_SAMPLING_48K;
  } else if (strcmp(audio_sampling, "96kHz") == 0) {
    audio->info.audio_sampling = ST30_SAMPLING_96K;
  } else if (strcmp(audio_sampling, "44.1kHz") == 0) {
    audio->info.audio_sampling = ST31_SAMPLING_44K;
  } else {
    err("%s, invalid audio sampling %s\n", __func__, audio_sampling);
    return -ST_JSON_NOT_VALID;
  }
  return ST_JSON_SUCCESS;
}

static int parse_audio_ptime(json_object* audio_obj, st_json_audio_session_t* audio) {
  const char* audio_ptime =
      json_object_get_string(st_json_object_object_get(audio_obj, "audio_ptime"));
  if (audio_ptime) {
    if (strcmp(audio_ptime, "1") == 0) {
      audio->info.audio_ptime = ST30_PTIME_1MS;
    } else if (strcmp(audio_ptime, "0.12") == 0) {
      audio->info.audio_ptime = ST30_PTIME_125US;
    } else if (strcmp(audio_ptime, "0.25") == 0) {
      audio->info.audio_ptime = ST30_PTIME_250US;
    } else if (strcmp(audio_ptime, "0.33") == 0) {
      audio->info.audio_ptime = ST30_PTIME_333US;
    } else if (strcmp(audio_ptime, "4") == 0) {
      audio->info.audio_ptime = ST30_PTIME_4MS;
    } else if (strcmp(audio_ptime, "0.08") == 0) {
      audio->info.audio_ptime = ST31_PTIME_80US;
    } else if (strcmp(audio_ptime, "1.09") == 0) {
      audio->info.audio_ptime = ST31_PTIME_1_09MS;
    } else if (strcmp(audio_ptime, "0.14") == 0) {
      audio->info.audio_ptime = ST31_PTIME_0_14MS;
    } else if (strcmp(audio_ptime, "0.09") == 0) {
      audio->info.audio_ptime = ST31_PTIME_0_09MS;
    } else {
      err("%s, invalid audio ptime %s\n", __func__, audio_ptime);
      return -ST_JSON_NOT_VALID;
    }
  } else { /* default ptime 1ms */
    audio->info.audio_ptime = ST30_PTIME_1MS;
  }
  return ST_JSON_SUCCESS;
}

static int st_json_parse_tx_audio(int idx, json_object* audio_obj,
                                  st_json_audio_session_t* audio) {
  if (audio_obj == NULL || audio == NULL) {
    err("%s, can not parse tx audio session\n", __func__);
    return -ST_JSON_NULL;
  }
  int ret;

  /* parse udp port  */
  ret = parse_base_udp_port(audio_obj, &audio->base, idx);
  if (ret < 0) return ret;

  /* parse payload type */
  ret = parse_base_payload_type(audio_obj, &audio->base);
  if (ret < 0) {
    err("%s, use default pt %u\n", __func__, ST_APP_PAYLOAD_TYPE_AUDIO);
    audio->base.payload_type = ST_APP_PAYLOAD_TYPE_AUDIO;
  }

  /* parse audio type */
  ret = parse_audio_type(audio_obj, audio);
  if (ret < 0) return ret;

  /* parse audio format */
  ret = parse_audio_format(audio_obj, audio);
  if (ret < 0) return ret;

  /* parse audio channel */
  ret = parse_audio_channel(audio_obj, audio);
  if (ret < 0) return ret;

  /* parse audio sampling */
  ret = parse_audio_sampling(audio_obj, audio);
  if (ret < 0) return ret;

  /* parse audio packet time */
  ret = parse_audio_ptime(audio_obj, audio);
  if (ret < 0) return ret;

  /* parse audio url */
  ret = parse_url(audio_obj, "audio_url", audio->info.audio_url);
  if (ret < 0) return ret;

  return ST_JSON_SUCCESS;
}

static int st_json_parse_rx_audio(int idx, json_object* audio_obj,
                                  st_json_audio_session_t* audio) {
  if (audio_obj == NULL || audio == NULL) {
    err("%s, can not parse rx audio session\n", __func__);
    return -ST_JSON_NULL;
  }
  int ret;

  /* parse udp port  */
  ret = parse_base_udp_port(audio_obj, &audio->base, idx);
  if (ret < 0) return ret;

  /* parse payload type */
  ret = parse_base_payload_type(audio_obj, &audio->base);
  if (ret < 0) {
    err("%s, use default pt %u\n", __func__, ST_APP_PAYLOAD_TYPE_AUDIO);
    audio->base.payload_type = ST_APP_PAYLOAD_TYPE_AUDIO;
  }

  /* parse audio type */
  ret = parse_audio_type(audio_obj, audio);
  if (ret < 0) return ret;

  /* parse audio format */
  ret = parse_audio_format(audio_obj, audio);
  if (ret < 0) return ret;

  /* parse audio channel */
  ret = parse_audio_channel(audio_obj, audio);
  if (ret < 0) return ret;

  /* parse audio sampling */
  ret = parse_audio_sampling(audio_obj, audio);
  if (ret < 0) return ret;

  /* parse audio packet time */
  ret = parse_audio_ptime(audio_obj, audio);
  if (ret < 0) return ret;

  /* parse audio url */
  ret = parse_url(audio_obj, "audio_url", audio->info.audio_url);
  if (ret < 0) {
    err("%s, no reference file\n", __func__);
  }

  return ST_JSON_SUCCESS;
}

static int st_json_parse_tx_anc(int idx, json_object* anc_obj,
                                st_json_ancillary_session_t* anc) {
  if (anc_obj == NULL || anc == NULL) {
    err("%s, can not parse tx anc session\n", __func__);
    return -ST_JSON_NULL;
  }
  int ret;

  /* parse udp port  */
  ret = parse_base_udp_port(anc_obj, &anc->base, idx);
  if (ret < 0) return ret;

  /* parse payload type */
  ret = parse_base_payload_type(anc_obj, &anc->base);
  if (ret < 0) {
    err("%s, use default pt %u\n", __func__, ST_APP_PAYLOAD_TYPE_ANCILLARY);
    anc->base.payload_type = ST_APP_PAYLOAD_TYPE_ANCILLARY;
  }

  /* parse anc type */
  const char* type = json_object_get_string(st_json_object_object_get(anc_obj, "type"));
  REQUIRED_ITEM(type);
  if (strcmp(type, "frame") == 0) {
    anc->info.type = ST40_TYPE_FRAME_LEVEL;
  } else if (strcmp(type, "rtp") == 0) {
    anc->info.type = ST40_TYPE_RTP_LEVEL;
  } else {
    err("%s, invalid anc type %s\n", __func__, type);
    return -ST_JSON_NOT_VALID;
  }
  /* parse anc format */
  const char* anc_format =
      json_object_get_string(st_json_object_object_get(anc_obj, "ancillary_format"));
  REQUIRED_ITEM(anc_format);
  if (strcmp(anc_format, "closed_caption") == 0) {
    anc->info.anc_format = ANC_FORMAT_CLOSED_CAPTION;
  } else {
    err("%s, invalid anc format %s\n", __func__, anc_format);
    return -ST_JSON_NOT_VALID;
  }

  /* parse anc fps */
  const char* anc_fps =
      json_object_get_string(st_json_object_object_get(anc_obj, "ancillary_fps"));
  REQUIRED_ITEM(anc_fps);
  if (strcmp(anc_fps, "p59") == 0) {
    anc->info.anc_fps = ST_FPS_P59_94;
  } else if (strcmp(anc_fps, "p50") == 0) {
    anc->info.anc_fps = ST_FPS_P50;
  } else if (strcmp(anc_fps, "p25") == 0) {
    anc->info.anc_fps = ST_FPS_P25;
  } else if (strcmp(anc_fps, "p29") == 0) {
    anc->info.anc_fps = ST_FPS_P29_97;
  } else {
    err("%s, invalid anc fps %s\n", __func__, anc_fps);
    return -ST_JSON_NOT_VALID;
  }

  /* parse anc url */
  ret = parse_url(anc_obj, "ancillary_url", anc->info.anc_url);
  if (ret < 0) return ret;

  return ST_JSON_SUCCESS;
}

static int st_json_parse_rx_anc(int idx, json_object* anc_obj,
                                st_json_ancillary_session_t* anc) {
  if (anc_obj == NULL || anc == NULL) {
    err("%s, can not parse rx anc session\n", __func__);
    return -ST_JSON_NULL;
  }
  int ret;

  /* parse udp port  */
  ret = parse_base_udp_port(anc_obj, &anc->base, idx);
  if (ret < 0) return ret;

  /* parse payload type */
  ret = parse_base_payload_type(anc_obj, &anc->base);
  if (ret < 0) {
    err("%s, use default pt %u\n", __func__, ST_APP_PAYLOAD_TYPE_ANCILLARY);
    anc->base.payload_type = ST_APP_PAYLOAD_TYPE_ANCILLARY;
  }

  return ST_JSON_SUCCESS;
}

static int parse_st22p_width(json_object* st22p_obj, st_json_st22p_session_t* st22p) {
  int width = json_object_get_int(st_json_object_object_get(st22p_obj, "width"));
  if (width <= 0) {
    err("%s, invalid width %d\n", __func__, width);
    return -ST_JSON_NOT_VALID;
  }
  st22p->info.width = width;
  return ST_JSON_SUCCESS;
}

static int parse_st22p_height(json_object* st22p_obj, st_json_st22p_session_t* st22p) {
  int height = json_object_get_int(st_json_object_object_get(st22p_obj, "height"));
  if (height <= 0) {
    err("%s, invalid height %d\n", __func__, height);
    return -ST_JSON_NOT_VALID;
  }
  st22p->info.height = height;
  return ST_JSON_SUCCESS;
}

static int parse_st22p_fps(json_object* st22p_obj, st_json_st22p_session_t* st22p) {
  const char* fps = json_object_get_string(st_json_object_object_get(st22p_obj, "fps"));
  REQUIRED_ITEM(fps);
  if (strcmp(fps, "p59") == 0) {
    st22p->info.fps = ST_FPS_P59_94;
  } else if (strcmp(fps, "p50") == 0) {
    st22p->info.fps = ST_FPS_P50;
  } else if (strcmp(fps, "p25") == 0) {
    st22p->info.fps = ST_FPS_P25;
  } else if (strcmp(fps, "p29") == 0) {
    st22p->info.fps = ST_FPS_P29_97;
  } else {
    err("%s, invalid anc fps %s\n", __func__, fps);
    return -ST_JSON_NOT_VALID;
  }
  return ST_JSON_SUCCESS;
}

static int parse_st22p_pack_type(json_object* st22p_obj, st_json_st22p_session_t* st22p) {
  const char* pack_type =
      json_object_get_string(st_json_object_object_get(st22p_obj, "pack_type"));
  REQUIRED_ITEM(pack_type);
  if (strcmp(pack_type, "codestream") == 0) {
    st22p->info.pack_type = ST22_PACK_CODESTREAM;
  } else if (strcmp(pack_type, "slice") == 0) {
    st22p->info.pack_type = ST22_PACK_SLICE;
  } else {
    err("%s, invalid pack_type %s\n", __func__, pack_type);
    return -ST_JSON_NOT_VALID;
  }
  return ST_JSON_SUCCESS;
}

static int parse_st22p_codec(json_object* st22p_obj, st_json_st22p_session_t* st22p) {
  const char* codec =
      json_object_get_string(st_json_object_object_get(st22p_obj, "codec"));
  REQUIRED_ITEM(codec);
  if (strcmp(codec, "JPEG-XS") == 0) {
    st22p->info.codec = ST22_CODEC_JPEGXS;
  } else {
    err("%s, invalid codec %s\n", __func__, codec);
    return -ST_JSON_NOT_VALID;
  }
  return ST_JSON_SUCCESS;
}

static int parse_st22p_device(json_object* st22p_obj, st_json_st22p_session_t* st22p) {
  const char* device =
      json_object_get_string(st_json_object_object_get(st22p_obj, "device"));
  REQUIRED_ITEM(device);
  if (strcmp(device, "AUTO") == 0) {
    st22p->info.device = ST_PLUGIN_DEVICE_AUTO;
  } else if (strcmp(device, "CPU") == 0) {
    st22p->info.device = ST_PLUGIN_DEVICE_CPU;
  } else if (strcmp(device, "GPU") == 0) {
    st22p->info.device = ST_PLUGIN_DEVICE_GPU;
  } else if (strcmp(device, "FPGA") == 0) {
    st22p->info.device = ST_PLUGIN_DEVICE_FPGA;
  } else {
    err("%s, invalid plugin device type %s\n", __func__, device);
    return -ST_JSON_NOT_VALID;
  }
  return ST_JSON_SUCCESS;
}

static int parse_st22p_quality(json_object* st22p_obj, st_json_st22p_session_t* st22p) {
  const char* quality =
      json_object_get_string(st_json_object_object_get(st22p_obj, "quality"));
  if (quality) {
    if (strcmp(quality, "quality") == 0) {
      st22p->info.quality = ST22_QUALITY_MODE_QUALITY;
    } else if (strcmp(quality, "speed") == 0) {
      st22p->info.quality = ST22_QUALITY_MODE_SPEED;
    } else {
      err("%s, invalid plugin quality type %s\n", __func__, quality);
      return -ST_JSON_NOT_VALID;
    }
  } else { /* default use speed mode */
    st22p->info.quality = ST22_QUALITY_MODE_SPEED;
  }
  return ST_JSON_SUCCESS;
}

static int parse_st22p_format(json_object* st22p_obj, st_json_st22p_session_t* st22p,
                              const char* format_name) {
  const char* format =
      json_object_get_string(st_json_object_object_get(st22p_obj, format_name));
  REQUIRED_ITEM(format);
  if (strcmp(format, "YUV422PLANAR10LE") == 0) {
    st22p->info.format = ST_FRAME_FMT_YUV422PLANAR10LE;
  } else if (strcmp(format, "YUV422PLANAR12LE") == 0) {
    st22p->info.format = ST_FRAME_FMT_YUV422PLANAR12LE;
  } else if (strcmp(format, "ARGB") == 0) {
    st22p->info.format = ST_FRAME_FMT_ARGB;
  } else if (strcmp(format, "BGRA") == 0) {
    st22p->info.format = ST_FRAME_FMT_BGRA;
  } else if (strcmp(format, "V210") == 0) {
    st22p->info.format = ST_FRAME_FMT_V210;
  } else if (strcmp(format, "YUV422PLANAR8") == 0) {
    st22p->info.format = ST_FRAME_FMT_YUV422PLANAR8;
  } else if (strcmp(format, "UYVY") == 0) {
    st22p->info.format = ST_FRAME_FMT_UYVY;
  } else if (strcmp(format, "YUV444PLANAR10LE") == 0) {
    st22p->info.format = ST_FRAME_FMT_YUV444PLANAR10LE;
  } else if (strcmp(format, "YUV444PLANAR12LE") == 0) {
    st22p->info.format = ST_FRAME_FMT_YUV444PLANAR12LE;
  } else if (strcmp(format, "GBRPLANAR10LE") == 0) {
    st22p->info.format = ST_FRAME_FMT_GBRPLANAR10LE;
  } else if (strcmp(format, "GBRPLANAR12LE") == 0) {
    st22p->info.format = ST_FRAME_FMT_GBRPLANAR12LE;
  } else if (strcmp(format, "YUV422RFC4175PG2BE10") == 0) {
    st22p->info.format = ST_FRAME_FMT_YUV422RFC4175PG2BE10;
  } else if (strcmp(format, "YUV422RFC4175PG2BE12") == 0) {
    st22p->info.format = ST_FRAME_FMT_YUV422RFC4175PG2BE12;
  } else if (strcmp(format, "YUV444RFC4175PG4BE10") == 0) {
    st22p->info.format = ST_FRAME_FMT_YUV444RFC4175PG4BE10;
  } else if (strcmp(format, "YUV444RFC4175PG2BE12") == 0) {
    st22p->info.format = ST_FRAME_FMT_YUV444RFC4175PG2BE12;
  } else if (strcmp(format, "RGBRFC4175PG4BE10") == 0) {
    st22p->info.format = ST_FRAME_FMT_RGBRFC4175PG4BE10;
  } else if (strcmp(format, "RGBRFC4175PG2BE12") == 0) {
    st22p->info.format = ST_FRAME_FMT_RGBRFC4175PG2BE12;
  } else if (strcmp(format, "RGB8") == 0) {
    st22p->info.format = ST_FRAME_FMT_RGB8;
  } else if (strcmp(format, "JPEGXS_CODESTREAM") == 0) {
    st22p->info.format = ST_FRAME_FMT_JPEGXS_CODESTREAM;
  } else {
    err("%s, invalid output format %s\n", __func__, format);
    return -ST_JSON_NOT_VALID;
  }
  return ST_JSON_SUCCESS;
}

static int st_json_parse_tx_st22p(int idx, json_object* st22p_obj,
                                  st_json_st22p_session_t* st22p) {
  if (st22p_obj == NULL || st22p == NULL) {
    err("%s, can not parse tx st22p session\n", __func__);
    return -ST_JSON_NULL;
  }
  int ret;

  /* parse udp port  */
  ret = parse_base_udp_port(st22p_obj, &st22p->base, idx);
  if (ret < 0) return ret;

  /* parse payload type */
  ret = parse_base_payload_type(st22p_obj, &st22p->base);
  if (ret < 0) {
    err("%s, use default pt %u\n", __func__, ST_APP_PAYLOAD_TYPE_ST22);
    st22p->base.payload_type = ST_APP_PAYLOAD_TYPE_ST22;
  }

  /* parse width */
  ret = parse_st22p_width(st22p_obj, st22p);
  if (ret < 0) return ret;

  /* parse height */
  ret = parse_st22p_height(st22p_obj, st22p);
  if (ret < 0) return ret;

  /* parse fps */
  ret = parse_st22p_fps(st22p_obj, st22p);
  if (ret < 0) return ret;

  /* parse pack_type */
  ret = parse_st22p_pack_type(st22p_obj, st22p);
  if (ret < 0) return ret;

  /* parse codec */
  ret = parse_st22p_codec(st22p_obj, st22p);
  if (ret < 0) return ret;

  /* parse device */
  ret = parse_st22p_device(st22p_obj, st22p);
  if (ret < 0) return ret;

  /* parse quality */
  ret = parse_st22p_quality(st22p_obj, st22p);
  if (ret < 0) return ret;

  /* parse input format */
  ret = parse_st22p_format(st22p_obj, st22p, "input_format");
  if (ret < 0) return ret;

  /* parse st22p url */
  ret = parse_url(st22p_obj, "st22p_url", st22p->info.st22p_url);
  if (ret < 0) return ret;

  /* parse codec_thread_count option */
  st22p->info.codec_thread_count =
      json_object_get_int(st_json_object_object_get(st22p_obj, "codec_thread_count"));

  /* parse display option */
  st22p->display =
      json_object_get_boolean(st_json_object_object_get(st22p_obj, "display"));

  return ST_JSON_SUCCESS;
}

static int st_json_parse_rx_st22p(int idx, json_object* st22p_obj,
                                  st_json_st22p_session_t* st22p) {
  if (st22p_obj == NULL || st22p == NULL) {
    err("%s, can not parse rx st22p session\n", __func__);
    return -ST_JSON_NULL;
  }
  int ret;

  /* parse udp port  */
  ret = parse_base_udp_port(st22p_obj, &st22p->base, idx);
  if (ret < 0) return ret;

  /* parse payload type */
  ret = parse_base_payload_type(st22p_obj, &st22p->base);
  if (ret < 0) {
    err("%s, use default pt %u\n", __func__, ST_APP_PAYLOAD_TYPE_ST22);
    st22p->base.payload_type = ST_APP_PAYLOAD_TYPE_ST22;
  }

  /* parse width */
  ret = parse_st22p_width(st22p_obj, st22p);
  if (ret < 0) return ret;

  /* parse height */
  ret = parse_st22p_height(st22p_obj, st22p);
  if (ret < 0) return ret;

  /* parse fps */
  ret = parse_st22p_fps(st22p_obj, st22p);
  if (ret < 0) return ret;

  /* parse pack_type */
  ret = parse_st22p_pack_type(st22p_obj, st22p);
  if (ret < 0) return ret;

  /* parse codec */
  ret = parse_st22p_codec(st22p_obj, st22p);
  if (ret < 0) return ret;

  /* parse device */
  ret = parse_st22p_device(st22p_obj, st22p);
  if (ret < 0) return ret;

  /* parse quality */
  ret = parse_st22p_quality(st22p_obj, st22p);
  if (ret < 0) return ret;

  /* parse output format */
  ret = parse_st22p_format(st22p_obj, st22p, "output_format");
  if (ret < 0) return ret;

  /* parse display option */
  st22p->display =
      json_object_get_boolean(st_json_object_object_get(st22p_obj, "display"));

  /* parse measure_latency option */
  st22p->measure_latency =
      json_object_get_boolean(st_json_object_object_get(st22p_obj, "measure_latency"));

  /* parse codec_thread_count option */
  st22p->info.codec_thread_count =
      json_object_get_int(st_json_object_object_get(st22p_obj, "codec_thread_count"));

  return ST_JSON_SUCCESS;
}

static int parse_st20p_width(json_object* st20p_obj, st_json_st20p_session_t* st20p) {
  int width = json_object_get_int(st_json_object_object_get(st20p_obj, "width"));
  if (width <= 0) {
    err("%s, invalid width %d\n", __func__, width);
    return -ST_JSON_NOT_VALID;
  }
  st20p->info.width = width;
  return ST_JSON_SUCCESS;
}

static int parse_st20p_height(json_object* st20p_obj, st_json_st20p_session_t* st20p) {
  int height = json_object_get_int(st_json_object_object_get(st20p_obj, "height"));
  if (height <= 0) {
    err("%s, invalid height %d\n", __func__, height);
    return -ST_JSON_NOT_VALID;
  }
  st20p->info.height = height;
  return ST_JSON_SUCCESS;
}

static int parse_st20p_fps(json_object* st20p_obj, st_json_st20p_session_t* st20p) {
  const char* fps = json_object_get_string(st_json_object_object_get(st20p_obj, "fps"));
  REQUIRED_ITEM(fps);
  if (strcmp(fps, "p59") == 0) {
    st20p->info.fps = ST_FPS_P59_94;
  } else if (strcmp(fps, "p50") == 0) {
    st20p->info.fps = ST_FPS_P50;
  } else if (strcmp(fps, "p25") == 0) {
    st20p->info.fps = ST_FPS_P25;
  } else if (strcmp(fps, "p29") == 0) {
    st20p->info.fps = ST_FPS_P29_97;
  } else {
    err("%s, invalid anc fps %s\n", __func__, fps);
    return -ST_JSON_NOT_VALID;
  }
  return ST_JSON_SUCCESS;
}

static int parse_st20p_device(json_object* st20p_obj, st_json_st20p_session_t* st20p) {
  const char* device =
      json_object_get_string(st_json_object_object_get(st20p_obj, "device"));
  REQUIRED_ITEM(device);
  if (strcmp(device, "AUTO") == 0) {
    st20p->info.device = ST_PLUGIN_DEVICE_AUTO;
  } else if (strcmp(device, "CPU") == 0) {
    st20p->info.device = ST_PLUGIN_DEVICE_CPU;
  } else if (strcmp(device, "GPU") == 0) {
    st20p->info.device = ST_PLUGIN_DEVICE_GPU;
  } else if (strcmp(device, "FPGA") == 0) {
    st20p->info.device = ST_PLUGIN_DEVICE_FPGA;
  } else {
    err("%s, invalid plugin device type %s\n", __func__, device);
    return -ST_JSON_NOT_VALID;
  }
  return ST_JSON_SUCCESS;
}

static int parse_st20p_format(json_object* st20p_obj, st_json_st20p_session_t* st20p,
                              const char* format_name) {
  const char* format =
      json_object_get_string(st_json_object_object_get(st20p_obj, format_name));
  REQUIRED_ITEM(format);
  if (strcmp(format, "YUV422PLANAR10LE") == 0) {
    st20p->info.format = ST_FRAME_FMT_YUV422PLANAR10LE;
  } else if (strcmp(format, "YUV422PLANAR12LE") == 0) {
    st20p->info.format = ST_FRAME_FMT_YUV422PLANAR12LE;
  } else if (strcmp(format, "ARGB") == 0) {
    st20p->info.format = ST_FRAME_FMT_ARGB;
  } else if (strcmp(format, "BGRA") == 0) {
    st20p->info.format = ST_FRAME_FMT_BGRA;
  } else if (strcmp(format, "V210") == 0) {
    st20p->info.format = ST_FRAME_FMT_V210;
  } else if (strcmp(format, "Y210") == 0) {
    st20p->info.format = ST_FRAME_FMT_Y210;
  } else if (strcmp(format, "YUV422PLANAR8") == 0) {
    st20p->info.format = ST_FRAME_FMT_YUV422PLANAR8;
  } else if (strcmp(format, "UYVY") == 0) {
    st20p->info.format = ST_FRAME_FMT_UYVY;
  } else if (strcmp(format, "YUV444PLANAR10LE") == 0) {
    st20p->info.format = ST_FRAME_FMT_YUV444PLANAR10LE;
  } else if (strcmp(format, "YUV444PLANAR12LE") == 0) {
    st20p->info.format = ST_FRAME_FMT_YUV444PLANAR12LE;
  } else if (strcmp(format, "GBRPLANAR10LE") == 0) {
    st20p->info.format = ST_FRAME_FMT_GBRPLANAR10LE;
  } else if (strcmp(format, "GBRPLANAR12LE") == 0) {
    st20p->info.format = ST_FRAME_FMT_GBRPLANAR12LE;
  } else if (strcmp(format, "YUV422RFC4175PG2BE10") == 0) {
    st20p->info.format = ST_FRAME_FMT_YUV422RFC4175PG2BE10;
  } else if (strcmp(format, "YUV422RFC4175PG2BE12") == 0) {
    st20p->info.format = ST_FRAME_FMT_YUV422RFC4175PG2BE12;
  } else if (strcmp(format, "YUV444RFC4175PG4BE10") == 0) {
    st20p->info.format = ST_FRAME_FMT_YUV444RFC4175PG4BE10;
  } else if (strcmp(format, "YUV444RFC4175PG2BE12") == 0) {
    st20p->info.format = ST_FRAME_FMT_YUV444RFC4175PG2BE12;
  } else if (strcmp(format, "RGBRFC4175PG4BE10") == 0) {
    st20p->info.format = ST_FRAME_FMT_RGBRFC4175PG4BE10;
  } else if (strcmp(format, "RGBRFC4175PG2BE12") == 0) {
    st20p->info.format = ST_FRAME_FMT_RGBRFC4175PG2BE12;
  } else if (strcmp(format, "RGB8") == 0) {
    st20p->info.format = ST_FRAME_FMT_RGB8;
  } else {
    err("%s, invalid output format %s\n", __func__, format);
    return -ST_JSON_NOT_VALID;
  }
  return ST_JSON_SUCCESS;
}

static int parse_st20p_transport_format(json_object* st20p_obj,
                                        st_json_st20p_session_t* st20p) {
  const char* t_format =
      json_object_get_string(st_json_object_object_get(st20p_obj, "transport_format"));
  REQUIRED_ITEM(t_format);
  if (strcmp(t_format, "YUV_422_10bit") == 0) {
    st20p->info.transport_format = ST20_FMT_YUV_422_10BIT;
  } else if (strcmp(t_format, "YUV_422_8bit") == 0) {
    st20p->info.transport_format = ST20_FMT_YUV_422_8BIT;
  } else if (strcmp(t_format, "YUV_422_12bit") == 0) {
    st20p->info.transport_format = ST20_FMT_YUV_422_12BIT;
  } else if (strcmp(t_format, "YUV_422_16bit") == 0) {
    st20p->info.transport_format = ST20_FMT_YUV_422_16BIT;
  } else if (strcmp(t_format, "YUV_444_10bit") == 0) {
    st20p->info.transport_format = ST20_FMT_YUV_444_10BIT;
  } else if (strcmp(t_format, "YUV_444_12bit") == 0) {
    st20p->info.transport_format = ST20_FMT_YUV_444_12BIT;
  } else if (strcmp(t_format, "YUV_420_8bit") == 0) {
    st20p->info.transport_format = ST20_FMT_YUV_420_8BIT;
  } else if (strcmp(t_format, "YUV_420_10bit") == 0) {
    st20p->info.transport_format = ST20_FMT_YUV_420_10BIT;
  } else if (strcmp(t_format, "YUV_420_12bit") == 0) {
    st20p->info.transport_format = ST20_FMT_YUV_420_12BIT;
  } else if (strcmp(t_format, "RGB_8bit") == 0) {
    st20p->info.transport_format = ST20_FMT_RGB_8BIT;
  } else if (strcmp(t_format, "RGB_10bit") == 0) {
    st20p->info.transport_format = ST20_FMT_RGB_10BIT;
  } else if (strcmp(t_format, "RGB_12bit") == 0) {
    st20p->info.transport_format = ST20_FMT_RGB_12BIT;
  } else if (strcmp(t_format, "RGB_16bit") == 0) {
    st20p->info.transport_format = ST20_FMT_RGB_16BIT;
  } else {
    err("%s, invalid transport format %s\n", __func__, t_format);
    return -ST_JSON_NOT_VALID;
  }
  return ST_JSON_SUCCESS;
}

static int st_json_parse_tx_st20p(int idx, json_object* st20p_obj,
                                  st_json_st20p_session_t* st20p) {
  if (st20p_obj == NULL || st20p == NULL) {
    err("%s, can not parse tx st20p session\n", __func__);
    return -ST_JSON_NULL;
  }
  int ret;

  /* parse udp port  */
  ret = parse_base_udp_port(st20p_obj, &st20p->base, idx);
  if (ret < 0) return ret;

  /* parse payload type */
  ret = parse_base_payload_type(st20p_obj, &st20p->base);
  if (ret < 0) {
    err("%s, use default pt %u\n", __func__, ST_APP_PAYLOAD_TYPE_ST22);
    st20p->base.payload_type = ST_APP_PAYLOAD_TYPE_ST22;
  }

  /* parse width */
  ret = parse_st20p_width(st20p_obj, st20p);
  if (ret < 0) return ret;

  /* parse height */
  ret = parse_st20p_height(st20p_obj, st20p);
  if (ret < 0) return ret;

  /* parse fps */
  ret = parse_st20p_fps(st20p_obj, st20p);
  if (ret < 0) return ret;

  /* parse device */
  ret = parse_st20p_device(st20p_obj, st20p);
  if (ret < 0) return ret;

  /* parse input format */
  ret = parse_st20p_format(st20p_obj, st20p, "input_format");
  if (ret < 0) return ret;

  /* parse transport format */
  ret = parse_st20p_transport_format(st20p_obj, st20p);
  if (ret < 0) return ret;

  /* parse st20p url */
  ret = parse_url(st20p_obj, "st20p_url", st20p->info.st20p_url);
  if (ret < 0) return ret;

  /* parse display option */
  st20p->display =
      json_object_get_boolean(st_json_object_object_get(st20p_obj, "display"));

  return ST_JSON_SUCCESS;
}

static int st_json_parse_rx_st20p(int idx, json_object* st20p_obj,
                                  st_json_st20p_session_t* st20p) {
  if (st20p_obj == NULL || st20p == NULL) {
    err("%s, can not parse rx st20p session\n", __func__);
    return -ST_JSON_NULL;
  }
  int ret;

  /* parse udp port  */
  ret = parse_base_udp_port(st20p_obj, &st20p->base, idx);
  if (ret < 0) return ret;

  /* parse payload type */
  ret = parse_base_payload_type(st20p_obj, &st20p->base);
  if (ret < 0) {
    err("%s, use default pt %u\n", __func__, ST_APP_PAYLOAD_TYPE_ST22);
    st20p->base.payload_type = ST_APP_PAYLOAD_TYPE_ST22;
  }

  /* parse width */
  ret = parse_st20p_width(st20p_obj, st20p);
  if (ret < 0) return ret;

  /* parse height */
  ret = parse_st20p_height(st20p_obj, st20p);
  if (ret < 0) return ret;

  /* parse fps */
  ret = parse_st20p_fps(st20p_obj, st20p);
  if (ret < 0) return ret;

  /* parse device */
  ret = parse_st20p_device(st20p_obj, st20p);
  if (ret < 0) return ret;

  /* parse output format */
  ret = parse_st20p_format(st20p_obj, st20p, "output_format");
  if (ret < 0) return ret;

  /* parse transport format */
  ret = parse_st20p_transport_format(st20p_obj, st20p);
  if (ret < 0) return ret;

  /* parse display option */
  st20p->display =
      json_object_get_boolean(st_json_object_object_get(st20p_obj, "display"));

  /* parse measure_latency option */
  st20p->measure_latency =
      json_object_get_boolean(st_json_object_object_get(st20p_obj, "measure_latency"));

  return ST_JSON_SUCCESS;
}

static int parse_session_num(json_object* group, const char* name) {
  int num = 0;
  json_object* session_array = st_json_object_object_get(group, name);
  if (session_array != NULL && json_object_get_type(session_array) == json_type_array) {
    for (int j = 0; j < json_object_array_length(session_array); ++j) {
      json_object* session = json_object_array_get_idx(session_array, j);
      int replicas = json_object_get_int(st_json_object_object_get(session, "replicas"));
      if (replicas < 0) {
        err("%s, invalid replicas number: %d\n", __func__, replicas);
        return -ST_JSON_NOT_VALID;
      }
      num += replicas;
    }
  }
  return num;
}

static int parse_session_ip(const char* str_ip, struct st_json_session_base* base,
                            enum mtl_session_port port) {
  int ret = inet_pton(AF_INET, str_ip, base->ip[port]);
  if (ret == 1) return 0;

  /* if it's local interface case for test */
  ret = strtol(str_ip, NULL, 10);
  if (ret < 0) return ret;
  base->type[port] = ST_JSON_IP_LOCAL_IF;
  base->local[port] = ret;
  dbg("%s, local if port %d\n", __func__, ret);
  return 0;
}

void st_app_free_json(st_json_context_t* ctx) {
  if (ctx->interfaces) {
    st_app_free(ctx->interfaces);
    ctx->interfaces = NULL;
  }
  if (ctx->tx_video_sessions) {
    st_app_free(ctx->tx_video_sessions);
    ctx->tx_video_sessions = NULL;
  }
  if (ctx->tx_audio_sessions) {
    st_app_free(ctx->tx_audio_sessions);
    ctx->tx_audio_sessions = NULL;
  }
  if (ctx->tx_anc_sessions) {
    st_app_free(ctx->tx_anc_sessions);
    ctx->tx_anc_sessions = NULL;
  }
  if (ctx->tx_st22p_sessions) {
    st_app_free(ctx->tx_st22p_sessions);
    ctx->tx_st22p_sessions = NULL;
  }
  if (ctx->tx_st20p_sessions) {
    st_app_free(ctx->tx_st20p_sessions);
    ctx->tx_st20p_sessions = NULL;
  }
  if (ctx->rx_video_sessions) {
    st_app_free(ctx->rx_video_sessions);
    ctx->rx_video_sessions = NULL;
  }
  if (ctx->rx_audio_sessions) {
    st_app_free(ctx->rx_audio_sessions);
    ctx->rx_audio_sessions = NULL;
  }
  if (ctx->rx_anc_sessions) {
    st_app_free(ctx->rx_anc_sessions);
    ctx->rx_anc_sessions = NULL;
  }
  if (ctx->rx_st22p_sessions) {
    st_app_free(ctx->rx_st22p_sessions);
    ctx->rx_st22p_sessions = NULL;
  }
  if (ctx->rx_st20p_sessions) {
    st_app_free(ctx->rx_st20p_sessions);
    ctx->rx_st20p_sessions = NULL;
  }
  if (ctx->rx_st20r_sessions) {
    st_app_free(ctx->rx_st20r_sessions);
    ctx->rx_st20r_sessions = NULL;
  }
}

int st_app_parse_json(st_json_context_t* ctx, const char* filename) {
  info("%s, using json-c version: %s\n", __func__, json_c_version());
  int ret = ST_JSON_SUCCESS;

  json_object* root_object = json_object_from_file(filename);
  if (root_object == NULL) {
    err("%s, can not parse json file %s, please check the format\n", __func__, filename);
    return -ST_JSON_PARSE_FAIL;
  }

  /* parse quota for system */
  json_object* sch_quota_object =
      st_json_object_object_get(root_object, "sch_session_quota");
  if (sch_quota_object != NULL) {
    int sch_quota = json_object_get_int(sch_quota_object);
    if (sch_quota <= 0) {
      err("%s, invalid quota number\n", __func__);
      ret = -ST_JSON_NOT_VALID;
      goto error;
    }
    ctx->sch_quota = sch_quota;
  }

  /* parse interfaces for system */
  json_object* interfaces_array = st_json_object_object_get(root_object, "interfaces");
  if (interfaces_array == NULL ||
      json_object_get_type(interfaces_array) != json_type_array) {
    err("%s, can not parse interfaces\n", __func__);
    ret = -ST_JSON_PARSE_FAIL;
    goto error;
  }
  int num_interfaces = json_object_array_length(interfaces_array);
  if (num_interfaces > MTL_PORT_MAX) {
    err("%s, invalid num_interfaces %d\n", __func__, num_interfaces);
    ret = -ST_JSON_NOT_VALID;
    goto error;
  }
  ctx->interfaces =
      (st_json_interface_t*)st_app_zmalloc(num_interfaces * sizeof(st_json_interface_t));
  if (!ctx->interfaces) {
    err("%s, failed to allocate interfaces\n", __func__);
    ret = -ST_JSON_NULL;
    goto error;
  }
  for (int i = 0; i < num_interfaces; ++i) {
    ret = st_json_parse_interfaces(json_object_array_get_idx(interfaces_array, i),
                                   &ctx->interfaces[i]);
    if (ret) goto error;
  }
  ctx->num_interfaces = num_interfaces;
  ctx->has_display = false;

  /* parse tx sessions  */
  json_object* tx_group_array = st_json_object_object_get(root_object, "tx_sessions");
  if (tx_group_array != NULL && json_object_get_type(tx_group_array) == json_type_array) {
    /* parse session numbers for array allocation */
    for (int i = 0; i < json_object_array_length(tx_group_array); ++i) {
      json_object* tx_group = json_object_array_get_idx(tx_group_array, i);
      if (tx_group == NULL) {
        err("%s, can not parse tx session group\n", __func__);
        ret = -ST_JSON_PARSE_FAIL;
        goto error;
      }
      int num = 0;
      /* parse tx video sessions */
      num = parse_session_num(tx_group, "video");
      if (num < 0) goto error;
      ctx->tx_video_session_cnt += num;
      /* parse tx audio sessions */
      num = parse_session_num(tx_group, "audio");
      if (num < 0) goto error;
      ctx->tx_audio_session_cnt += num;
      /* parse tx ancillary sessions */
      num = parse_session_num(tx_group, "ancillary");
      if (num < 0) goto error;
      ctx->tx_anc_session_cnt += num;
      /* parse tx st22p sessions */
      num = parse_session_num(tx_group, "st22p");
      if (num < 0) goto error;
      ctx->tx_st22p_session_cnt += num;
      /* parse tx st20p sessions */
      num = parse_session_num(tx_group, "st20p");
      if (num < 0) goto error;
      ctx->tx_st20p_session_cnt += num;
    }

    /* allocate tx sessions */
    ctx->tx_video_sessions = (st_json_video_session_t*)st_app_zmalloc(
        ctx->tx_video_session_cnt * sizeof(st_json_video_session_t));
    if (!ctx->tx_video_sessions) {
      err("%s, failed to allocate tx_video_sessions\n", __func__);
      ret = -ST_JSON_NULL;
      goto error;
    }
    ctx->tx_audio_sessions = (st_json_audio_session_t*)st_app_zmalloc(
        ctx->tx_audio_session_cnt * sizeof(st_json_audio_session_t));
    if (!ctx->tx_audio_sessions) {
      err("%s, failed to allocate tx_audio_sessions\n", __func__);
      ret = -ST_JSON_NULL;
      goto error;
    }
    ctx->tx_anc_sessions = (st_json_ancillary_session_t*)st_app_zmalloc(
        ctx->tx_anc_session_cnt * sizeof(st_json_ancillary_session_t));
    if (!ctx->tx_anc_sessions) {
      err("%s, failed to allocate tx_anc_sessions\n", __func__);
      ret = -ST_JSON_NULL;
      goto error;
    }
    ctx->tx_st22p_sessions = (st_json_st22p_session_t*)st_app_zmalloc(
        ctx->tx_st22p_session_cnt * sizeof(st_json_st22p_session_t));
    if (!ctx->tx_st22p_sessions) {
      err("%s, failed to allocate tx_st22p_sessions\n", __func__);
      ret = -ST_JSON_NULL;
      goto error;
    }
    ctx->tx_st20p_sessions = (st_json_st20p_session_t*)st_app_zmalloc(
        ctx->tx_st20p_session_cnt * sizeof(st_json_st20p_session_t));
    if (!ctx->tx_st20p_sessions) {
      err("%s, failed to allocate tx_st20p_sessions\n", __func__);
      ret = -ST_JSON_NULL;
      goto error;
    }

    int num_inf = 0;
    int num_video = 0;
    int num_audio = 0;
    int num_anc = 0;
    int num_st22p = 0;
    int num_st20p = 0;

    for (int i = 0; i < json_object_array_length(tx_group_array); ++i) {
      json_object* tx_group = json_object_array_get_idx(tx_group_array, i);
      if (tx_group == NULL) {
        err("%s, can not parse tx session group\n", __func__);
        ret = -ST_JSON_PARSE_FAIL;
        goto error;
      }

      /* parse destination ip */
      json_object* dip_p = NULL;
      json_object* dip_r = NULL;
      json_object* dip_array = st_json_object_object_get(tx_group, "dip");
      if (dip_array != NULL && json_object_get_type(dip_array) == json_type_array) {
        int len = json_object_array_length(dip_array);
        if (len < 1 || len > MTL_PORT_MAX) {
          err("%s, wrong dip number\n", __func__);
          ret = -ST_JSON_NOT_VALID;
          goto error;
        }
        dip_p = json_object_array_get_idx(dip_array, 0);
        if (len == 2) {
          dip_r = json_object_array_get_idx(dip_array, 1);
        }
        num_inf = len;
      } else {
        err("%s, can not parse dip_array\n", __func__);
        ret = -ST_JSON_PARSE_FAIL;
        goto error;
      }

      /* parse interface */
      int inf_p, inf_r = 0;
      json_object* interface_array = st_json_object_object_get(tx_group, "interface");
      if (interface_array != NULL &&
          json_object_get_type(interface_array) == json_type_array) {
        int len = json_object_array_length(interface_array);
        if (len != num_inf) {
          err("%s, wrong interface number\n", __func__);
          ret = -ST_JSON_NOT_VALID;
          goto error;
        }
        inf_p = json_object_get_int(json_object_array_get_idx(interface_array, 0));
        if (inf_p < 0 || inf_p > num_interfaces) {
          err("%s, wrong interface index\n", __func__);
          ret = -ST_JSON_NOT_VALID;
          goto error;
        }
        if (len == 2) {
          inf_r = json_object_get_int(json_object_array_get_idx(interface_array, 1));
          if (inf_r < 0 || inf_r > num_interfaces) {
            err("%s, wrong interface index\n", __func__);
            ret = -ST_JSON_NOT_VALID;
            goto error;
          }
        }
      } else {
        err("%s, can not parse interface_array\n", __func__);
        ret = -ST_JSON_PARSE_FAIL;
        goto error;
      }

      /* parse tx video sessions */
      json_object* video_array = st_json_object_object_get(tx_group, "video");
      if (video_array != NULL && json_object_get_type(video_array) == json_type_array) {
        for (int j = 0; j < json_object_array_length(video_array); ++j) {
          json_object* video_session = json_object_array_get_idx(video_array, j);
          int replicas =
              json_object_get_int(st_json_object_object_get(video_session, "replicas"));
          if (replicas < 0) {
            err("%s, invalid replicas number: %d\n", __func__, replicas);
            ret = -ST_JSON_NOT_VALID;
            goto error;
          }
          for (int k = 0; k < replicas; ++k) {
            parse_session_ip(json_object_get_string(dip_p),
                             &ctx->tx_video_sessions[num_video].base, MTL_SESSION_PORT_P);
            ctx->tx_video_sessions[num_video].base.inf[0] = &ctx->interfaces[inf_p];
            if (num_inf == 2) {
              parse_session_ip(json_object_get_string(dip_r),
                               &ctx->tx_video_sessions[num_video].base,
                               MTL_SESSION_PORT_R);
              ctx->tx_video_sessions[num_video].base.inf[1] = &ctx->interfaces[inf_r];
            }
            ctx->tx_video_sessions[num_video].base.num_inf = num_inf;
            ret = st_json_parse_tx_video(k, video_session,
                                         &ctx->tx_video_sessions[num_video]);
            if (ret) goto error;
            if (ctx->tx_video_sessions[num_video].display) ctx->has_display = true;
            num_video++;
          }
        }
      }

      /* parse tx audio sessions */
      json_object* audio_array = st_json_object_object_get(tx_group, "audio");
      if (audio_array != NULL && json_object_get_type(audio_array) == json_type_array) {
        for (int j = 0; j < json_object_array_length(audio_array); ++j) {
          json_object* audio_session = json_object_array_get_idx(audio_array, j);
          int replicas =
              json_object_get_int(st_json_object_object_get(audio_session, "replicas"));
          if (replicas < 0) {
            err("%s, invalid replicas number: %d\n", __func__, replicas);
            ret = -ST_JSON_NOT_VALID;
            goto error;
          }
          for (int k = 0; k < replicas; ++k) {
            parse_session_ip(json_object_get_string(dip_p),
                             &ctx->tx_audio_sessions[num_audio].base, MTL_SESSION_PORT_P);
            ctx->tx_audio_sessions[num_audio].base.inf[0] = &ctx->interfaces[inf_p];
            if (num_inf == 2) {
              parse_session_ip(json_object_get_string(dip_r),
                               &ctx->tx_audio_sessions[num_audio].base,
                               MTL_SESSION_PORT_R);
              ctx->tx_audio_sessions[num_audio].base.inf[1] = &ctx->interfaces[inf_r];
            }
            ctx->tx_audio_sessions[num_audio].base.num_inf = num_inf;
            ret = st_json_parse_tx_audio(k, audio_session,
                                         &ctx->tx_audio_sessions[num_audio]);
            if (ret) goto error;
            num_audio++;
          }
        }
      }

      /* parse tx ancillary sessions */
      json_object* anc_array = st_json_object_object_get(tx_group, "ancillary");
      if (anc_array != NULL && json_object_get_type(anc_array) == json_type_array) {
        for (int j = 0; j < json_object_array_length(anc_array); ++j) {
          json_object* anc_session = json_object_array_get_idx(anc_array, j);
          int replicas =
              json_object_get_int(st_json_object_object_get(anc_session, "replicas"));
          if (replicas < 0) {
            err("%s, invalid replicas number: %d\n", __func__, replicas);
            ret = -ST_JSON_NOT_VALID;
            goto error;
          }
          for (int k = 0; k < replicas; ++k) {
            parse_session_ip(json_object_get_string(dip_p),
                             &ctx->tx_anc_sessions[num_anc].base, MTL_SESSION_PORT_P);
            ctx->tx_anc_sessions[num_anc].base.inf[0] = &ctx->interfaces[inf_p];
            if (num_inf == 2) {
              parse_session_ip(json_object_get_string(dip_r),
                               &ctx->tx_anc_sessions[num_anc].base, MTL_SESSION_PORT_R);
              ctx->tx_anc_sessions[num_anc].base.inf[1] = &ctx->interfaces[inf_r];
            }
            ctx->tx_anc_sessions[num_anc].base.num_inf = num_inf;
            ret = st_json_parse_tx_anc(k, anc_session, &ctx->tx_anc_sessions[num_anc]);
            if (ret) goto error;
            num_anc++;
          }
        }
      }

      /* parse tx st22p sessions */
      json_object* st22p_array = st_json_object_object_get(tx_group, "st22p");
      if (st22p_array != NULL && json_object_get_type(st22p_array) == json_type_array) {
        for (int j = 0; j < json_object_array_length(st22p_array); ++j) {
          json_object* st22p_session = json_object_array_get_idx(st22p_array, j);
          int replicas =
              json_object_get_int(st_json_object_object_get(st22p_session, "replicas"));
          if (replicas < 0) {
            err("%s, invalid replicas number: %d\n", __func__, replicas);
            ret = -ST_JSON_NOT_VALID;
            goto error;
          }
          for (int k = 0; k < replicas; ++k) {
            parse_session_ip(json_object_get_string(dip_p),
                             &ctx->tx_st22p_sessions[num_st22p].base, MTL_SESSION_PORT_P);
            ctx->tx_st22p_sessions[num_st22p].base.inf[0] = &ctx->interfaces[inf_p];
            if (num_inf == 2) {
              parse_session_ip(json_object_get_string(dip_r),
                               &ctx->tx_st22p_sessions[num_st22p].base,
                               MTL_SESSION_PORT_R);
              ctx->tx_st22p_sessions[num_st22p].base.inf[1] = &ctx->interfaces[inf_r];
            }
            ctx->tx_st22p_sessions[num_st22p].base.num_inf = num_inf;
            ret = st_json_parse_tx_st22p(k, st22p_session,
                                         &ctx->tx_st22p_sessions[num_st22p]);
            if (ret) goto error;
            if (ctx->tx_st22p_sessions[num_st22p].display) ctx->has_display = true;
            num_st22p++;
          }
        }
      }

      /* parse tx st20p sessions */
      json_object* st20p_array = st_json_object_object_get(tx_group, "st20p");
      if (st20p_array != NULL && json_object_get_type(st20p_array) == json_type_array) {
        for (int j = 0; j < json_object_array_length(st20p_array); ++j) {
          json_object* st20p_session = json_object_array_get_idx(st20p_array, j);
          int replicas =
              json_object_get_int(st_json_object_object_get(st20p_session, "replicas"));
          if (replicas < 0) {
            err("%s, invalid replicas number: %d\n", __func__, replicas);
            ret = -ST_JSON_NOT_VALID;
            goto error;
          }
          for (int k = 0; k < replicas; ++k) {
            parse_session_ip(json_object_get_string(dip_p),
                             &ctx->tx_st20p_sessions[num_st20p].base, MTL_SESSION_PORT_P);
            ctx->tx_st20p_sessions[num_st20p].base.inf[0] = &ctx->interfaces[inf_p];
            if (num_inf == 2) {
              parse_session_ip(json_object_get_string(dip_r),
                               &ctx->tx_st20p_sessions[num_st20p].base,
                               MTL_SESSION_PORT_R);
              ctx->tx_st20p_sessions[num_st20p].base.inf[1] = &ctx->interfaces[inf_r];
            }
            ctx->tx_st20p_sessions[num_st20p].base.num_inf = num_inf;
            ret = st_json_parse_tx_st20p(k, st20p_session,
                                         &ctx->tx_st20p_sessions[num_st20p]);
            if (ret) goto error;
            if (ctx->tx_st20p_sessions[num_st20p].display) ctx->has_display = true;
            num_st20p++;
          }
        }
      }
    }
  }

  /* parse rx sessions */
  json_object* rx_group_array = st_json_object_object_get(root_object, "rx_sessions");
  if (rx_group_array != NULL && json_object_get_type(rx_group_array) == json_type_array) {
    /* parse session numbers for array allocation */
    for (int i = 0; i < json_object_array_length(rx_group_array); ++i) {
      json_object* rx_group = json_object_array_get_idx(rx_group_array, i);
      if (rx_group == NULL) {
        err("%s, can not parse rx session group\n", __func__);
        ret = -ST_JSON_PARSE_FAIL;
        goto error;
      }
      int num = 0;
      /* parse rx video sessions */
      num = parse_session_num(rx_group, "video");
      if (num < 0) goto error;
      ctx->rx_video_session_cnt += num;
      /* parse rx audio sessions */
      num = parse_session_num(rx_group, "audio");
      if (num < 0) goto error;
      ctx->rx_audio_session_cnt += num;
      /* parse rx ancillary sessions */
      num = parse_session_num(rx_group, "ancillary");
      if (num < 0) goto error;
      ctx->rx_anc_session_cnt += num;
      /* parse rx st22p sessions */
      num = parse_session_num(rx_group, "st22p");
      if (num < 0) goto error;
      ctx->rx_st22p_session_cnt += num;
      /* parse rx st20p sessions */
      num = parse_session_num(rx_group, "st20p");
      if (num < 0) goto error;
      ctx->rx_st20p_session_cnt += num;
      /* parse rx st20r sessions */
      num = parse_session_num(rx_group, "st20r");
      if (num < 0) goto error;
      ctx->rx_st20r_session_cnt += num;
    }

    /* allocate rx sessions */
    ctx->rx_video_sessions = (st_json_video_session_t*)st_app_zmalloc(
        ctx->rx_video_session_cnt * sizeof(st_json_video_session_t));
    if (!ctx->rx_video_sessions) {
      err("%s, failed to allocate rx_video_sessions\n", __func__);
      ret = -ST_JSON_NULL;
      goto error;
    }
    ctx->rx_audio_sessions = (st_json_audio_session_t*)st_app_zmalloc(
        ctx->rx_audio_session_cnt * sizeof(st_json_audio_session_t));
    if (!ctx->rx_audio_sessions) {
      err("%s, failed to allocate rx_audio_sessions\n", __func__);
      ret = -ST_JSON_NULL;
      goto error;
    }
    ctx->rx_anc_sessions = (st_json_ancillary_session_t*)st_app_zmalloc(
        ctx->rx_anc_session_cnt * sizeof(st_json_ancillary_session_t));
    if (!ctx->rx_anc_sessions) {
      err("%s, failed to allocate rx_anc_sessions\n", __func__);
      ret = -ST_JSON_NULL;
      goto error;
    }
    ctx->rx_st22p_sessions = (st_json_st22p_session_t*)st_app_zmalloc(
        ctx->rx_st22p_session_cnt * sizeof(st_json_st22p_session_t));
    if (!ctx->rx_st22p_sessions) {
      err("%s, failed to allocate rx_st22p_sessions\n", __func__);
      ret = -ST_JSON_NULL;
      goto error;
    }
    ctx->rx_st20p_sessions = (st_json_st20p_session_t*)st_app_zmalloc(
        ctx->rx_st20p_session_cnt * sizeof(st_json_st20p_session_t));
    if (!ctx->rx_st20p_sessions) {
      err("%s, failed to allocate rx_st20p_sessions\n", __func__);
      ret = -ST_JSON_NULL;
      goto error;
    }
    ctx->rx_st20r_sessions = (st_json_video_session_t*)st_app_zmalloc(
        ctx->rx_st20r_session_cnt * sizeof(*ctx->rx_st20r_sessions));
    if (!ctx->rx_st20r_sessions) {
      err("%s, failed to allocate rx_st20r_sessions\n", __func__);
      ret = -ST_JSON_NULL;
      goto error;
    }

    int num_inf = 0;
    int num_video = 0;
    int num_audio = 0;
    int num_anc = 0;
    int num_st22p = 0;
    int num_st20p = 0;
    int num_st20r = 0;

    for (int i = 0; i < json_object_array_length(rx_group_array); ++i) {
      json_object* rx_group = json_object_array_get_idx(rx_group_array, i);
      if (rx_group == NULL) {
        err("%s, can not parse rx session group\n", __func__);
        ret = -ST_JSON_PARSE_FAIL;
        goto error;
      }

      /* parse receiving ip */
      json_object* ip_p = NULL;
      json_object* ip_r = NULL;
      json_object* ip_array = st_json_object_object_get(rx_group, "ip");
      if (ip_array != NULL && json_object_get_type(ip_array) == json_type_array) {
        int len = json_object_array_length(ip_array);
        if (len < 1 || len > MTL_PORT_MAX) {
          err("%s, wrong dip number\n", __func__);
          ret = -ST_JSON_NOT_VALID;
          goto error;
        }
        ip_p = json_object_array_get_idx(ip_array, 0);
        if (len == 2) {
          ip_r = json_object_array_get_idx(ip_array, 1);
        }
        num_inf = len;
      } else {
        err("%s, can not parse dip_array\n", __func__);
        ret = -ST_JSON_PARSE_FAIL;
        goto error;
      }

      /* parse interface */
      int inf_p, inf_r = 0;
      json_object* interface_array = st_json_object_object_get(rx_group, "interface");
      if (interface_array != NULL &&
          json_object_get_type(interface_array) == json_type_array) {
        int len = json_object_array_length(interface_array);
        if (len != num_inf) {
          err("%s, wrong interface number\n", __func__);
          ret = -ST_JSON_NOT_VALID;
          goto error;
        }
        inf_p = json_object_get_int(json_object_array_get_idx(interface_array, 0));
        if (inf_p < 0 || inf_p > num_interfaces) {
          err("%s, wrong interface index\n", __func__);
          ret = -ST_JSON_NOT_VALID;
          goto error;
        }
        if (len == 2) {
          inf_r = json_object_get_int(json_object_array_get_idx(interface_array, 1));
          if (inf_r < 0 || inf_r > num_interfaces) {
            err("%s, wrong interface index\n", __func__);
            ret = -ST_JSON_NOT_VALID;
            goto error;
          }
        }
      } else {
        err("%s, can not parse interface_array\n", __func__);
        ret = -ST_JSON_PARSE_FAIL;
        goto error;
      }

      /* parse rx video sessions */
      json_object* video_array = st_json_object_object_get(rx_group, "video");
      if (video_array != NULL && json_object_get_type(video_array) == json_type_array) {
        for (int j = 0; j < json_object_array_length(video_array); ++j) {
          json_object* video_session = json_object_array_get_idx(video_array, j);
          int replicas =
              json_object_get_int(st_json_object_object_get(video_session, "replicas"));
          if (replicas < 0) {
            err("%s, invalid replicas number: %d\n", __func__, replicas);
            ret = -ST_JSON_NOT_VALID;
            goto error;
          }
          for (int k = 0; k < replicas; ++k) {
            parse_session_ip(json_object_get_string(ip_p),
                             &ctx->rx_video_sessions[num_video].base, MTL_SESSION_PORT_P);
            ctx->rx_video_sessions[num_video].base.inf[0] = &ctx->interfaces[inf_p];
            if (num_inf == 2) {
              parse_session_ip(json_object_get_string(ip_r),
                               &ctx->rx_video_sessions[num_video].base,
                               MTL_SESSION_PORT_R);
              ctx->rx_video_sessions[num_video].base.inf[1] = &ctx->interfaces[inf_r];
            }
            ctx->rx_video_sessions[num_video].base.num_inf = num_inf;
            ret = st_json_parse_rx_video(k, video_session,
                                         &ctx->rx_video_sessions[num_video]);
            if (ret) goto error;
            if (ctx->rx_video_sessions[num_video].display) ctx->has_display = true;
            num_video++;
          }
        }
      }

      /* parse rx audio sessions */
      json_object* audio_array = st_json_object_object_get(rx_group, "audio");
      if (audio_array != NULL && json_object_get_type(audio_array) == json_type_array) {
        for (int j = 0; j < json_object_array_length(audio_array); ++j) {
          json_object* audio_session = json_object_array_get_idx(audio_array, j);
          int replicas =
              json_object_get_int(st_json_object_object_get(audio_session, "replicas"));
          if (replicas < 0) {
            err("%s, invalid replicas number: %d\n", __func__, replicas);
            ret = -ST_JSON_NOT_VALID;
            goto error;
          }
          for (int k = 0; k < replicas; ++k) {
            parse_session_ip(json_object_get_string(ip_p),
                             &ctx->rx_audio_sessions[num_audio].base, MTL_SESSION_PORT_P);
            ctx->rx_audio_sessions[num_audio].base.inf[0] = &ctx->interfaces[inf_p];
            if (num_inf == 2) {
              parse_session_ip(json_object_get_string(ip_r),
                               &ctx->rx_audio_sessions[num_audio].base,
                               MTL_SESSION_PORT_R);
              ctx->rx_audio_sessions[num_audio].base.inf[1] = &ctx->interfaces[inf_r];
            }
            ctx->rx_audio_sessions[num_audio].base.num_inf = num_inf;
            ret = st_json_parse_rx_audio(k, audio_session,
                                         &ctx->rx_audio_sessions[num_audio]);
            if (ret) goto error;
            num_audio++;
          }
        }
      }

      /* parse rx ancillary sessions */
      json_object* anc_array = st_json_object_object_get(rx_group, "ancillary");
      if (anc_array != NULL && json_object_get_type(anc_array) == json_type_array) {
        for (int j = 0; j < json_object_array_length(anc_array); ++j) {
          json_object* anc_session = json_object_array_get_idx(anc_array, j);
          int replicas =
              json_object_get_int(st_json_object_object_get(anc_session, "replicas"));
          if (replicas < 0) {
            err("%s, invalid replicas number: %d\n", __func__, replicas);
            ret = -ST_JSON_NOT_VALID;
            goto error;
          }
          for (int k = 0; k < replicas; ++k) {
            parse_session_ip(json_object_get_string(ip_p),
                             &ctx->rx_anc_sessions[num_anc].base, MTL_SESSION_PORT_P);
            ctx->rx_anc_sessions[num_anc].base.inf[0] = &ctx->interfaces[inf_p];
            if (num_inf == 2) {
              parse_session_ip(json_object_get_string(ip_r),
                               &ctx->rx_anc_sessions[num_anc].base, MTL_SESSION_PORT_R);
              ctx->rx_anc_sessions[num_anc].base.inf[1] = &ctx->interfaces[inf_r];
            }
            ctx->rx_anc_sessions[num_anc].base.num_inf = num_inf;
            ret = st_json_parse_rx_anc(k, anc_session, &ctx->rx_anc_sessions[num_anc]);
            if (ret) goto error;
            num_anc++;
          }
        }
      }

      /* parse rx st22p sessions */
      json_object* st22p_array = st_json_object_object_get(rx_group, "st22p");
      if (st22p_array != NULL && json_object_get_type(st22p_array) == json_type_array) {
        for (int j = 0; j < json_object_array_length(st22p_array); ++j) {
          json_object* st22p_session = json_object_array_get_idx(st22p_array, j);
          int replicas =
              json_object_get_int(st_json_object_object_get(st22p_session, "replicas"));
          if (replicas < 0) {
            err("%s, invalid replicas number: %d\n", __func__, replicas);
            ret = -ST_JSON_NOT_VALID;
            goto error;
          }
          for (int k = 0; k < replicas; ++k) {
            parse_session_ip(json_object_get_string(ip_p),
                             &ctx->rx_st22p_sessions[num_st22p].base, MTL_SESSION_PORT_P);
            ctx->rx_st22p_sessions[num_st22p].base.inf[0] = &ctx->interfaces[inf_p];
            if (num_inf == 2) {
              parse_session_ip(json_object_get_string(ip_r),
                               &ctx->rx_st22p_sessions[num_st22p].base,
                               MTL_SESSION_PORT_R);
              ctx->rx_st22p_sessions[num_st22p].base.inf[1] = &ctx->interfaces[inf_r];
            }
            ctx->rx_st22p_sessions[num_st22p].base.num_inf = num_inf;
            ret = st_json_parse_rx_st22p(k, st22p_session,
                                         &ctx->rx_st22p_sessions[num_st22p]);
            if (ret) goto error;
            if (ctx->rx_st22p_sessions[num_st22p].display) ctx->has_display = true;
            num_st22p++;
          }
        }
      }

      /* parse rx st20p sessions */
      json_object* st20p_array = st_json_object_object_get(rx_group, "st20p");
      if (st20p_array != NULL && json_object_get_type(st20p_array) == json_type_array) {
        for (int j = 0; j < json_object_array_length(st20p_array); ++j) {
          json_object* st20p_session = json_object_array_get_idx(st20p_array, j);
          int replicas =
              json_object_get_int(st_json_object_object_get(st20p_session, "replicas"));
          if (replicas < 0) {
            err("%s, invalid replicas number: %d\n", __func__, replicas);
            ret = -ST_JSON_NOT_VALID;
            goto error;
          }
          for (int k = 0; k < replicas; ++k) {
            parse_session_ip(json_object_get_string(ip_p),
                             &ctx->rx_st20p_sessions[num_st20p].base, MTL_SESSION_PORT_P);
            ctx->rx_st20p_sessions[num_st20p].base.inf[0] = &ctx->interfaces[inf_p];
            if (num_inf == 2) {
              parse_session_ip(json_object_get_string(ip_r),
                               &ctx->rx_st20p_sessions[num_st20p].base,
                               MTL_SESSION_PORT_R);
              ctx->rx_st20p_sessions[num_st20p].base.inf[1] = &ctx->interfaces[inf_r];
            }
            ctx->rx_st20p_sessions[num_st20p].base.num_inf = num_inf;
            ret = st_json_parse_rx_st20p(k, st20p_session,
                                         &ctx->rx_st20p_sessions[num_st20p]);
            if (ret) goto error;
            if (ctx->rx_st20p_sessions[num_st20p].display) ctx->has_display = true;
            num_st20p++;
          }
        }
      }

      /* parse rx st20r sessions */
      json_object* st20r_array = st_json_object_object_get(rx_group, "st20r");
      if (st20r_array != NULL && json_object_get_type(st20r_array) == json_type_array) {
        if (num_inf != 2) {
          err("%s, invalid num_inf number for st20r: %d\n", __func__, num_inf);
          ret = -ST_JSON_NOT_VALID;
          goto error;
        }
        for (int j = 0; j < json_object_array_length(st20r_array); ++j) {
          json_object* st20r_session = json_object_array_get_idx(st20r_array, j);
          int replicas =
              json_object_get_int(st_json_object_object_get(st20r_session, "replicas"));
          if (replicas < 0) {
            err("%s, invalid replicas number for st20r: %d\n", __func__, replicas);
            ret = -ST_JSON_NOT_VALID;
            goto error;
          }
          for (int k = 0; k < replicas; ++k) {
            parse_session_ip(json_object_get_string(ip_p),
                             &ctx->rx_st20r_sessions[num_st20r].base, MTL_SESSION_PORT_P);
            ctx->rx_st20r_sessions[num_st20r].base.inf[0] = &ctx->interfaces[inf_p];
            if (num_inf == 2) {
              parse_session_ip(json_object_get_string(ip_r),
                               &ctx->rx_st20r_sessions[num_st20r].base,
                               MTL_SESSION_PORT_R);
              ctx->rx_st20r_sessions[num_st20r].base.inf[1] = &ctx->interfaces[inf_r];
            }
            ctx->rx_st20r_sessions[num_st20r].base.num_inf = num_inf;
            ret = st_json_parse_rx_video(k, st20r_session,
                                         &ctx->rx_st20r_sessions[num_st20r]);
            if (ret) goto error;
            if (ctx->rx_st20r_sessions[num_st20p].display) ctx->has_display = true;
            num_st20r++;
          }
        }
      }
    }
  }

  json_object_put(root_object);
  return 0;

error:
  st_app_free_json(ctx);
  json_object_put(root_object);
  return ret < 0 ? ret : -ST_JSON_PARSE_FAIL;
}

enum st_fps st_app_get_fps(enum video_format fmt) {
  int i;

  for (i = 0; i < ARRAY_SIZE(st_video_fmt_descs); i++) {
    if (fmt == st_video_fmt_descs[i].fmt) {
      return st_video_fmt_descs[i].fps;
    }
  }

  err("%s, invalid fmt %d\n", __func__, fmt);
  return ST_FPS_P59_94;
}

uint32_t st_app_get_width(enum video_format fmt) {
  int i;

  for (i = 0; i < ARRAY_SIZE(st_video_fmt_descs); i++) {
    if (fmt == st_video_fmt_descs[i].fmt) {
      return st_video_fmt_descs[i].width;
    }
  }

  err("%s, invalid fmt %d\n", __func__, fmt);
  return 1920;
}

uint32_t st_app_get_height(enum video_format fmt) {
  int i;

  for (i = 0; i < ARRAY_SIZE(st_video_fmt_descs); i++) {
    if (fmt == st_video_fmt_descs[i].fmt) {
      return st_video_fmt_descs[i].height;
    }
  }

  err("%s, invalid fmt %d\n", __func__, fmt);
  return 1080;
}

bool st_app_get_interlaced(enum video_format fmt) {
  switch (fmt) {
    case VIDEO_FORMAT_480I_59FPS:
    case VIDEO_FORMAT_576I_50FPS:
    case VIDEO_FORMAT_1080I_59FPS:
    case VIDEO_FORMAT_1080I_50FPS:
      return true;
    default:
      return false;
  }
}

uint8_t* st_json_ip(struct st_app_context* ctx, st_json_session_base_t* base,
                    enum mtl_session_port port) {
  if (base->type[port] == ST_JSON_IP_LOCAL_IF) {
    mtl_port_ip_info(ctx->st, base->local[port], base->local_ip[port], NULL, NULL);
    return base->local_ip[port];
  } else {
    return base->ip[port];
  }
}
