/* SPDX-License-Identifier: BSD-3-Clause
 * poc_14 — JSON config parser for 14-stream demo
 */

#include "config_parser.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

static int parse_ip(const char* str, uint8_t out[4]) {
  struct in_addr addr;
  if (!str || inet_pton(AF_INET, str, &addr) != 1) return -1;
  memcpy(out, &addr, 4);
  return 0;
}

static const char* json_get_string(const cJSON* obj, const char* key, const char* def) {
  const cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
  if (cJSON_IsString(item) && item->valuestring) return item->valuestring;
  return def;
}

static int json_get_int(const cJSON* obj, const char* key, int def) {
  const cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
  if (cJSON_IsNumber(item)) return item->valueint;
  return def;
}

static bool json_get_bool(const cJSON* obj, const char* key, bool def) {
  const cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
  if (cJSON_IsBool(item)) return cJSON_IsTrue(item);
  return def;
}

int poc14_config_parse(const char* json_path, poc14_config_t* out) {
  memset(out, 0, sizeof(*out));

  /* Defaults */
  strncpy(out->mtl_port, "0000:31:01.1", sizeof(out->mtl_port) - 1);
  out->sip[0] = 192;
  out->sip[1] = 168;
  out->sip[2] = 2;
  out->sip[3] = 10;
  strncpy(out->mxl_domain, "/tmp/mxl_16", sizeof(out->mxl_domain) - 1);
  strncpy(out->fabrics_provider, "verbs", sizeof(out->fabrics_provider) - 1);
  strncpy(out->fabrics_local_ip, "0.0.0.0", sizeof(out->fabrics_local_ip) - 1);
  out->framebuff_cnt = 4;
  out->accept_incomplete = true;
  out->video_width = 1920;
  out->video_height = 1080;
  out->fps_num = 30000;
  out->fps_den = 1001;
  out->payload_type = 112;
  out->active_streams = 0;  /* 0 = use all streams in array */
  out->auto_cpu_start = 21; /* avoid CPUs 0-20 */

  /* Load file */
  FILE* fp = fopen(json_path, "rb");
  if (!fp) {
    fprintf(stderr, "[CONFIG] Cannot open %s: %m\n", json_path);
    return -1;
  }
  fseek(fp, 0, SEEK_END);
  long fsize = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  char* buf = malloc(fsize + 1);
  if (!buf) {
    fclose(fp);
    return -1;
  }
  fread(buf, 1, fsize, fp);
  buf[fsize] = '\0';
  fclose(fp);

  /* Parse JSON */
  cJSON* root = cJSON_Parse(buf);
  free(buf);
  if (!root) {
    fprintf(stderr, "[CONFIG] JSON parse error: %s\n",
            cJSON_GetErrorPtr() ? cJSON_GetErrorPtr() : "unknown");
    return -1;
  }

  /* ── Global section ── */
  const cJSON* global = cJSON_GetObjectItemCaseSensitive(root, "global");
  if (global) {
    const char* s;
    s = json_get_string(global, "mtl_port", NULL);
    if (s) strncpy(out->mtl_port, s, sizeof(out->mtl_port) - 1);

    s = json_get_string(global, "sip", NULL);
    if (s) parse_ip(s, out->sip);

    s = json_get_string(global, "mxl_domain", NULL);
    if (s) strncpy(out->mxl_domain, s, sizeof(out->mxl_domain) - 1);

    s = json_get_string(global, "fabrics_provider", NULL);
    if (s) strncpy(out->fabrics_provider, s, sizeof(out->fabrics_provider) - 1);

    s = json_get_string(global, "fabrics_local_ip", NULL);
    if (s) strncpy(out->fabrics_local_ip, s, sizeof(out->fabrics_local_ip) - 1);

    s = json_get_string(global, "mtl_lcores", NULL);
    if (s) strncpy(out->mtl_lcores, s, sizeof(out->mtl_lcores) - 1);

    out->framebuff_cnt = json_get_int(global, "framebuff_cnt", out->framebuff_cnt);
    out->accept_incomplete =
        json_get_bool(global, "accept_incomplete", out->accept_incomplete);
    out->duration_sec = json_get_int(global, "duration_sec", 0);
    out->payload_type = (uint8_t)json_get_int(global, "payload_type", 112);
    out->active_streams = (uint32_t)json_get_int(global, "active_streams", 0);
    out->auto_cpu_start = json_get_int(global, "auto_cpu_start", 21);

    /* TX-specific globals */
    s = json_get_string(global, "tx_port", NULL);
    if (s) strncpy(out->tx_port, s, sizeof(out->tx_port) - 1);

    s = json_get_string(global, "tx_sip", NULL);
    if (s) parse_ip(s, out->tx_sip);

    s = json_get_string(global, "tx_lcores", NULL);
    if (s) strncpy(out->tx_lcores, s, sizeof(out->tx_lcores) - 1);

    /* Video */
    const cJSON* video = cJSON_GetObjectItemCaseSensitive(global, "video");
    if (video) {
      out->video_width = (uint32_t)json_get_int(video, "width", (int)out->video_width);
      out->video_height = (uint32_t)json_get_int(video, "height", (int)out->video_height);
      out->fps_num = (uint32_t)json_get_int(video, "fps_num", (int)out->fps_num);
      out->fps_den = (uint32_t)json_get_int(video, "fps_den", (int)out->fps_den);
    }
  }

  /* ── Streams array ── */
  const cJSON* streams = cJSON_GetObjectItemCaseSensitive(root, "streams");
  if (!cJSON_IsArray(streams)) {
    fprintf(stderr, "[CONFIG] Missing or invalid 'streams' array\n");
    cJSON_Delete(root);
    return -1;
  }

  int count = cJSON_GetArraySize(streams);
  if (count > POC14_MAX_STREAMS) {
    fprintf(stderr, "[CONFIG] Too many streams (%d > %d)\n", count, POC14_MAX_STREAMS);
    cJSON_Delete(root);
    return -1;
  }
  /* Honour active_streams knob: 0 = use all in array */
  if (out->active_streams > 0 && (int)out->active_streams < count)
    count = (int)out->active_streams;
  out->num_streams = (uint32_t)count;

  int next_worker_cpu = out->auto_cpu_start;
  int next_consumer_cpu = out->auto_cpu_start;

  for (int i = 0; i < count; i++) {
    const cJSON* item = cJSON_GetArrayItem(streams, i);
    poc14_stream_config_t* sc = &out->streams[i];

    sc->id = json_get_int(item, "id", i);
    strncpy(sc->label, json_get_string(item, "label", ""), sizeof(sc->label) - 1);
    strncpy(sc->source, json_get_string(item, "source", "synthetic"),
            sizeof(sc->source) - 1);

    const char* mip = json_get_string(item, "multicast_ip", NULL);
    if (mip) parse_ip(mip, sc->multicast_ip);

    const char* csip = json_get_string(item, "camera_src_ip", NULL);
    if (csip) parse_ip(csip, sc->camera_src_ip);

    sc->udp_port = (uint16_t)json_get_int(item, "udp_port", 20000);

    const char* fj = json_get_string(item, "flow_json", "");
    if (fj[0]) {
      strncpy(sc->flow_json_path, fj, sizeof(sc->flow_json_path) - 1);
    } else {
      /* Auto-generate path from stream id */
      snprintf(sc->flow_json_path, sizeof(sc->flow_json_path), "config/flow_%d.json",
               sc->id);
    }

    strncpy(sc->fabrics_local_port, json_get_string(item, "fabrics_local_port", "5000"),
            sizeof(sc->fabrics_local_port) - 1);

    sc->worker_cpu = json_get_int(item, "worker_cpu", -1);
    sc->consumer_cpu = json_get_int(item, "consumer_cpu", -1);

    /* Auto-assign CPUs when not set, avoiding reserved range */
    if (sc->worker_cpu < 0) sc->worker_cpu = next_worker_cpu++;
    if (sc->consumer_cpu < 0) sc->consumer_cpu = next_consumer_cpu++;
  }

  cJSON_Delete(root);
  return 0;
}

void poc14_config_print(const poc14_config_t* cfg) {
  char sip_str[INET_ADDRSTRLEN];
  inet_ntop(AF_INET, cfg->sip, sip_str, sizeof(sip_str));

  printf("══════════════════════════════════════════════════════\n");
  printf("  16-Stream Demo Configuration\n");
  printf("══════════════════════════════════════════════════════\n");
  printf("  MTL RX port:    %s  SIP: %s\n", cfg->mtl_port, sip_str);
  if (cfg->tx_port[0]) {
    char tx_sip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, cfg->tx_sip, tx_sip_str, sizeof(tx_sip_str));
    printf("  MTL TX port:    %s  SIP: %s\n", cfg->tx_port, tx_sip_str);
  }
  printf("  MXL domain:     %s\n", cfg->mxl_domain);
  printf("  Provider:       %s → %s\n", cfg->fabrics_provider, cfg->fabrics_local_ip);
  printf("  Video:          %ux%u @ %u/%u\n", cfg->video_width, cfg->video_height,
         cfg->fps_num, cfg->fps_den);
  printf("  Streams:        %u (active_streams=%u, auto_cpu_start=%d)\n",
         cfg->num_streams, cfg->active_streams, cfg->auto_cpu_start);
  printf("──────────────────────────────────────────────────────\n");
  for (uint32_t i = 0; i < cfg->num_streams; i++) {
    const poc14_stream_config_t* s = &cfg->streams[i];
    char mip[INET_ADDRSTRLEN] = "0.0.0.0";
    inet_ntop(AF_INET, s->multicast_ip, mip, sizeof(mip));

    printf("  [%2d] %-20s  %s  mcast=%s:%u  fabrics=:%s  cpu=%d  con_cpu=%d", s->id,
           s->label, s->source, mip, s->udp_port, s->fabrics_local_port, s->worker_cpu,
           s->consumer_cpu);
    if (strcmp(s->source, "camera") == 0) {
      char csip[INET_ADDRSTRLEN] = "0.0.0.0";
      inet_ntop(AF_INET, s->camera_src_ip, csip, sizeof(csip));
      printf("  cam_src=%s", csip);
    }
    printf("\n");
  }
  printf("══════════════════════════════════════════════════════\n\n");
}
